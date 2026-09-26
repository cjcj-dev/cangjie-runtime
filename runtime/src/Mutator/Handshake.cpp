#include "Handshake.h"

#include <mutex>
#include <thread>
#include <unordered_set>

#include "Mutator.h"
#include "Mutator.inline.h"
#include "MutatorManager.h"
#include "ThreadLocal.h"
#include "ThreadSMR.h"
#include "Common/Runtime.h"

namespace MapleRuntime {
namespace {
thread_local HandshakeState nativeHandshakeState{nullptr};
std::mutex g_cpuProfilePendingLock;
std::unordered_set<Mutator*> g_cpuProfilePending;
}

HandshakeState& Handshake::Current()
{
    Mutator* thread = ThreadLocal::GetMutator();
    return thread != nullptr ? thread->GetHandshakeState() : nativeHandshakeState;
}

void HandshakeOperation::do_handshake(Mutator* thread)
{
    cl_->do_thread(thread);
    pending_.fetch_sub(1, std::memory_order_release);
}

void HandshakeState::add_operation(HandshakeOperation* op)
{
    {
        std::lock_guard<std::mutex> lock(lock_);
        queue_.push_back(op);
    }
    // A logical target may migrate between carriers while the poll is armed.
    ArmAllThreadPolls();
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
        remove_op(op);
        op->do_handshake(handshakee_);
    }
    Mutator* mutator = handshakee_;
    if (mutator != nullptr && mutator->HasSuspensionRequest(Mutator::SUSPENSION_FOR_CPU_PROFILE)) {
        (void)mutator->TransitionToCpuProfile(true);
    }
    UpdatePollValues(ThreadLocal::GetThreadLocalData());
}

bool HandshakeState::possibly_can_process()
{
    // Rechecked while holding lock_: leave_safe must acquire the same lock
    // before the owner resumes or migrates into managed execution.
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
    remove_op(op);
    op->do_handshake(handshakee_);
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

void HandshakeState::process_queued_then_detach()
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
        remove_op(op);
        op->do_handshake(handshakee_);
    }
    inSafe_.store(1, std::memory_order_release);
}

namespace {
void WaitHandshakeOperation(HandshakeOperation& op, const ThreadsListHandle& threads)
{
    Mutator* current = ThreadLocal::GetMutator();
    while (!op.is_completed()) {
        for (size_t i = 0; i < threads.length(); ++i) {
            Mutator* target = threads.thread_at(i);
            if (op.target() != nullptr && target != op.target()) { continue; }
            if (target == current) {
                target->GetHandshakeState().process_by_self();
            } else {
                (void)target->GetHandshakeState().try_process();
            }
        }
        if (!op.is_completed()) { std::this_thread::yield(); }
    }
}
} // namespace

void Handshake::execute(HandshakeClosure* cl)
{
    if (cl == nullptr) { return; }
    // HotSpot handshake.cpp:257-260: pin and enqueue every logical thread,
    // including unmounted Cangjie tasks, not only the current carrier owners.
    ThreadsListHandle threads;
    if (threads.length() == 0) { return; }
    HandshakeOperation op(cl, nullptr);
    op.add_target_count(threads.length() - 1);
    for (size_t i = 0; i < threads.length(); ++i) {
        threads.thread_at(i)->GetHandshakeState().add_operation(&op);
    }
    WaitHandshakeOperation(op, threads);
}

void Handshake::execute(HandshakeClosure* cl, Mutator* target)
{
    if (cl == nullptr || target == nullptr) { return; }
    ThreadsListHandle threads;
    if (!threads.includes(target)) { return; }
    HandshakeOperation op(cl, target);
    target->GetHandshakeState().add_operation(&op);
    WaitHandshakeOperation(op, threads);
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

void PublishCpuProfileRequest(Mutator* mutator)
{
    if (mutator == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_cpuProfilePendingLock);
    mutator->SetSuspensionFlag(Mutator::SUSPENSION_FOR_CPU_PROFILE);
    g_cpuProfilePending.insert(mutator);
    ArmAllThreadPolls();
}

void ConsumeCpuProfileRequest(Mutator* mutator)
{
    if (mutator == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_cpuProfilePendingLock);
    g_cpuProfilePending.erase(mutator);
    mutator->ClearSuspensionFlag(Mutator::SUSPENSION_FOR_CPU_PROFILE);
    mutator->SetCpuProfileState(Mutator::FINISH_CPUPROFILE);
}

bool ClaimCpuProfileRequest(Mutator* mutator)
{
    if (mutator == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_cpuProfilePendingLock);
    const bool queued = g_cpuProfilePending.erase(mutator) != 0;
    Mutator::CpuProfileState state = mutator->GetCpuProfileState();
    if (!queued && state != Mutator::NEED_CPUPROFILE) {
        return false;
    }
    mutator->SetCpuProfileState(Mutator::IN_CPUPROFILING);
    return true;
}

void CompleteCpuProfileRequest(Mutator* mutator)
{
    if (mutator == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_cpuProfilePendingLock);
    if (g_cpuProfilePending.find(mutator) == g_cpuProfilePending.end()) {
        mutator->ClearSuspensionFlag(Mutator::SUSPENSION_FOR_CPU_PROFILE);
        mutator->SetCpuProfileState(Mutator::FINISH_CPUPROFILE);
    }
}

bool CpuProfileRequestQueued(const Mutator* mutator)
{
    if (mutator == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_cpuProfilePendingLock);
    return g_cpuProfilePending.find(const_cast<Mutator*>(mutator)) != g_cpuProfilePending.end();
}

bool HasPendingCpuProfileRequest()
{
    std::lock_guard<std::mutex> lock(g_cpuProfilePendingLock);
    return !g_cpuProfilePending.empty();
}

bool GlobalPoll()
{
    if (Runtime::CurrentRef() == nullptr) {
        return HasPendingCpuProfileRequest();
    }
    return HasPendingCpuProfileRequest() || MutatorManager::Instance().SyncTriggered();
}

bool HasPendingSafepoint(ThreadLocalData* tls)
{
    if (GlobalPoll()) {
        return true;
    }
    HandshakeState* state = tls != nullptr && tls->mutator != nullptr
        ? &tls->mutator->GetHandshakeState() : nullptr;
    if (state != nullptr && state->has_operation()) {
        return true;
    }
    if (tls != nullptr && tls->mutator != nullptr &&
        tls->mutator->HasSuspensionRequest(Mutator::SUSPENSION_FOR_CPU_PROFILE)) {
        return true;
    }
    if (Runtime::CurrentRef() == nullptr) {
        return false;
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
