#include "Handshake.h"

#include <thread>

#include "MutatorManager.h"
#include "ThreadLocal.h"

namespace MapleRuntime {
namespace {
thread_local HandshakeState tlHandshakeState(nullptr);
}

HandshakeState& Handshake::Current()
{
    ThreadLocalData* tls = ThreadLocal::GetThreadLocalData();
    tlHandshakeState.set_handshakee(tls);
    return tlHandshakeState;
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
        op->do_handshake(handshakee_);
        remove_op(op);
    }
    UpdatePollValues(handshakee_);
}

bool HandshakeState::possibly_can_process()
{
    return MutatorManager::Instance().TlsObservedSafe(handshakee_);
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
    op->do_handshake(handshakee_);
    remove_op(op);
    lock_.unlock();
    return true;
}

void Handshake::execute(HandshakeClosure* cl)
{
    if (cl == nullptr) {
        return;
    }
    std::list<HandshakeOperation*> ops;
    MutatorManager::Instance().EnqueueHandshakeOnAll(cl, ops);
    HandshakeState& self = Current();
    self.process_by_self();
    while (!ops.empty()) {
        for (auto it = ops.begin(); it != ops.end();) {
            HandshakeOperation* op = *it;
            HandshakeState* state = ForTls(op->target());
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

void ArmThreadPoll(ThreadLocalData* tls)
{
    if (tls != nullptr) {
        tls->safepointState = 1;
    }
}

bool HasPendingSafepoint(ThreadLocalData* tls)
{
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
