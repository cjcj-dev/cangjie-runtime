#include "Handshake.h"

#include <thread>

#include "MutatorManager.h"
#include "ThreadLocal.h"

namespace MapleRuntime {
namespace {
thread_local HandshakeState* tlHandshakeState = nullptr;
}

HandshakeState& Handshake::Current()
{
    if (tlHandshakeState != nullptr) {
        tlHandshakeState->set_handshakee(ThreadLocal::GetThreadLocalData());
        return *tlHandshakeState;
    }
    MutatorManager::Instance().RegisterMarkFlushThread(ThreadLocal::GetThreadLocalData());
    if (tlHandshakeState == nullptr) {
        static HandshakeState fallback(ThreadLocal::GetThreadLocalData());
        return fallback;
    }
    return *tlHandshakeState;
}

void Handshake::BindCurrent(HandshakeState* state)
{
    tlHandshakeState = state;
}

HandshakeState* Handshake::ForTls(ThreadLocalData* tls)
{
    if (tls == nullptr) {
        return nullptr;
    }
    if (tls == ThreadLocal::GetThreadLocalData()) {
        return &Current();
    }
    return MutatorManager::Instance().HandshakeStateForTls(tls);
}

void HandshakeState::add_operation(HandshakeOperation* op)
{
    {
        std::lock_guard<std::mutex> lock(lock_);
        queue_.push_back(op);
    }
    ArmThreadPoll(handshakee_);
}

bool HandshakeState::has_operation()
{
    std::lock_guard<std::mutex> lock(lock_);
    return !queue_.empty();
}

bool HandshakeState::operation_pending(HandshakeOperation* op)
{
    std::lock_guard<std::mutex> lock(lock_);
    for (HandshakeOperation* cur : queue_) {
        if (cur == op) {
            return true;
        }
    }
    return false;
}

HandshakeOperation* HandshakeState::get_op_for_self()
{
    if (queue_.empty()) {
        return nullptr;
    }
    return queue_.front();
}

HandshakeOperation* HandshakeState::get_op()
{
    return get_op_for_self();
}

void HandshakeState::remove_op(HandshakeOperation* op)
{
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (*it == op) {
            queue_.erase(it);
            return;
        }
    }
}

void HandshakeState::process_by_self()
{
    for (;;) {
        std::lock_guard<std::mutex> lock(lock_);
        HandshakeOperation* op = get_op_for_self();
        if (op == nullptr) {
            break;
        }
        if (op->target() != nullptr && op->target() != handshakee_) {
            break;
        }
        op->do_handshake(handshakee_);
        remove_op(op);
    }
    UpdatePollValues(handshakee_);
}

bool HandshakeState::possibly_can_process()
{
    return observed_safe();
}

bool HandshakeState::claim_handshake()
{
    if (!lock_.try_lock()) {
        return false;
    }
    if (queue_.empty()) {
        lock_.unlock();
        return false;
    }
    return true;
}

bool HandshakeState::try_process()
{
    if (!has_operation()) {
        return false;
    }
    if (!possibly_can_process()) {
        return false;
    }
    if (!claim_handshake()) {
        return false;
    }
    if (!possibly_can_process()) {
        lock_.unlock();
        return false;
    }
    HandshakeOperation* op = get_op();
    if (op == nullptr) {
        lock_.unlock();
        return false;
    }
    if (op->target() != nullptr && op->target() != handshakee_) {
        lock_.unlock();
        return false;
    }
    op->do_handshake(handshakee_);
    remove_op(op);
    lock_.unlock();
    return true;
}

void HandshakeState::enter_safe()
{
    std::lock_guard<std::mutex> lock(lock_);
    inSafe_.store(1, std::memory_order_release);
}

void HandshakeState::leave_safe()
{
    std::lock_guard<std::mutex> lock(lock_);
    inSafe_.store(0, std::memory_order_release);
}

void HandshakeState::process_queued_then_detach(void (*flush)(ThreadLocalData*))
{
    std::lock_guard<std::mutex> lock(lock_);
    for (;;) {
        HandshakeOperation* op = get_op_for_self();
        if (op == nullptr) {
            break;
        }
        if (op->target() != nullptr && op->target() != handshakee_) {
            break;
        }
        op->do_handshake(handshakee_);
        remove_op(op);
    }
    if (flush != nullptr) {
        flush(handshakee_);
    }
    inSafe_.store(1, std::memory_order_release);
}

namespace {
void WaitHandshakeOps(std::list<HandshakeOperation*>& ops)
{
    HandshakeState& self = Handshake::Current();
    self.process_by_self();
    while (!ops.empty()) {
        for (auto it = ops.begin(); it != ops.end();) {
            HandshakeOperation* op = *it;
            HandshakeState* state = Handshake::ForTls(op->target());
            if (state == nullptr || !state->operation_pending(op)) {
                delete op;
                it = ops.erase(it);
                continue;
            }
            if (op->target() == ThreadLocal::GetThreadLocalData()) {
                state->process_by_self();
                continue;
            }
            (void)state->try_process();
            ++it;
        }
        if (!ops.empty()) {
            std::this_thread::yield();
        }
    }
}
} // namespace

void Handshake::execute(HandshakeClosure* cl)
{
    if (cl == nullptr) {
        return;
    }
    std::list<HandshakeOperation*> ops;
    std::vector<MutatorManager::MarkFlushThread*> handle;
    MutatorManager::Instance().EnqueueHandshakeOnAll(cl, ops, handle);
    WaitHandshakeOps(ops);
    MutatorManager::Instance().ReleaseHandshakeHandle(handle);
}

void Handshake::execute(HandshakeClosure* cl, ThreadLocalData* target)
{
    if (cl == nullptr || target == nullptr) {
        return;
    }
    std::list<HandshakeOperation*> ops;
    std::vector<MutatorManager::MarkFlushThread*> handle;
    MutatorManager::Instance().EnqueueHandshakeOn(target, cl, ops, handle);
    WaitHandshakeOps(ops);
    MutatorManager::Instance().ReleaseHandshakeHandle(handle);
}

void Handshake::execute(HandshakeClosure* cl, Mutator* mutator)
{
    if (cl == nullptr || mutator == nullptr) {
        return;
    }
    std::list<HandshakeOperation*> ops;
    std::vector<MutatorManager::MarkFlushThread*> handle;
    MutatorManager::Instance().EnqueueHandshakeOnMutator(mutator, cl, ops, handle);
    WaitHandshakeOps(ops);
    MutatorManager::Instance().ReleaseHandshakeHandle(handle);
}

void ArmThreadPoll(ThreadLocalData* tls)
{
    if (tls != nullptr) {
        tls->safepointState = 1;
    }
}

void ArmAllThreadPolls()
{
    MutatorManager::Instance().ForEachMarkFlushTls([](ThreadLocalData* tls) { ArmThreadPoll(tls); });
}

bool GlobalPoll()
{
    return MutatorManager::Instance().SyncTriggered() || MutatorManager::Instance().EpochHandshakeActive();
}

bool HasPendingSafepoint(ThreadLocalData* tls)
{
    if (GlobalPoll()) {
        return true;
    }
    HandshakeState* state = Handshake::ForTls(tls);
    if (state != nullptr && state->has_operation()) {
        return true;
    }
    return MutatorManager::Instance().TlsHasMarkFlushPending(tls);
}

void UpdatePollValues(ThreadLocalData* tls)
{
    if (tls == nullptr || tls != ThreadLocal::GetThreadLocalData()) {
        return;
    }
    for (;;) {
        const bool armed = HasPendingSafepoint(tls);
        tls->safepointState = armed ? 1 : 0;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!armed && HasPendingSafepoint(tls)) {
            continue;
        }
        break;
    }
}
} // namespace MapleRuntime
