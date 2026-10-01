#include "Mutator/VMOperation.h"
#include "Base/GcLog.h"
#include "Base/Log.h"
#include "Common/ScopedObjectAccess.h"
#include "Common/SuspendibleThreadSet.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"

namespace MapleRuntime {
namespace {
class VMThreadHaltOperation : public VMOperation {
public:
    void doit() override {}
    const char* name() const override { return "Halt"; }
};
VMThreadHaltOperation haltOperation;
}

VMThread::VMThread() : nextOperation(&haltOperation) {}

VMThread& VMThread::instance()
{
    static VMThread* vmThread = new VMThread();
    return *vmThread;
}

bool VMThread::is_VM_thread()
{
    return ThreadLocal::GetThreadType() == ThreadType::VM_THREAD;
}

bool VMThread::is_running()
{
    auto& vmThread = instance();
    std::lock_guard<std::mutex> guard(vmThread.lock);
    return vmThread.running;
}

bool VMThread::is_terminated()
{
    auto& vmThread = instance();
    std::lock_guard<std::mutex> guard(vmThread.lock);
    return vmThread.terminated;
}

VMOperation* VMThread::vm_operation()
{
    CHECK_DETAIL(is_VM_thread(), "VM operation is private to the VM thread");
    return instance().currentOperation;
}

void VMThread::create()
{
    auto& vmThread = instance();
    std::unique_lock<std::mutex> guard(vmThread.lock);
    // vmThread.cpp:113-115 asserts one VMThread at a time, and :148-150
    // destroy() clears the pointer so the next runtime may create another.
    CHECK_DETAIL(!vmThread.running, "one VM thread at a time");
    if (vmThread.terminated) {
        vmThread.terminated = false;
        vmThread.shouldTerminate = false;
        vmThread.currentOperation = nullptr;
    }
    vmThread.thread = std::thread([&vmThread] { vmThread.run(); });
    vmThread.condition.wait(guard, [&vmThread] { return vmThread.running; });
}

bool VMThread::set_next_operation(VMOperation* operation)
{
    if (nextOperation != nullptr) { return false; }
    nextOperation = operation;
    return true;
}

void VMThread::wait_until_executed(VMOperation* operation)
{
    ScopedEnterSaferegion saferegion(false);
    std::unique_lock<std::mutex> guard(lock);
    while (!set_next_operation(operation)) {
        condition.wait(guard);
    }
    condition.notify_all();
    while (nextOperation == operation) {
        condition.wait(guard);
    }
}

void VMThread::execute(VMOperation* operation)
{
    auto& vmThread = instance();
    if (is_VM_thread()) {
        operation->set_calling_thread(ThreadLocal::GetThreadLocalData());
        vmThread.inner_execute(operation);
        return;
    }
    // HotSpot vmThread.cpp:525-530: synchronous wait must not pin STS.
    CHECK_DETAIL(!SuspendibleThreadSet::is_suspendible_thread(), "VM operation submitter must not belong to STS");
    CHECK_DETAIL(!ThreadLocal::GetThreadLocalData()->isIndirectlySuspendibleThread,
                 "VM operation submitter must not indirectly belong to STS");
    CHECK_DETAIL(is_running(), "VM thread must be ready before operation submission");
    if (!operation->doit_prologue()) { return; }
    operation->set_calling_thread(ThreadLocal::GetThreadLocalData());
    vmThread.wait_until_executed(operation);
    operation->doit_epilogue();
}

void VMThread::inner_execute(VMOperation* operation)
{
    CHECK_DETAIL(is_VM_thread(), "VM thread execution required");
    VMOperation* previous = currentOperation;
    if (previous != nullptr) {
        CHECK_DETAIL(previous->allow_nested_vm_operations(), "Unexpected nested VM operation");
        operation->set_calling_thread(previous->calling_thread());
    }
    currentOperation = operation;
    auto& manager = MutatorManager::Instance();
    const bool endSafepoint = operation->evaluate_at_safepoint() && !manager.WorldStopped();
    // The VM thread owns the safepoint now, so the pause ledger the collector
    // phase guards read moves here with it (zStat.cpp:711-759 reports the same
    // scope at the collection exit). Non-GC callers keep their own scope.
    uint64_t startTime = 0;
    uint64_t stoppedTime = 0;
    if (endSafepoint) {
        startTime = TimeUtil::NanoSeconds();
        manager.StopTheWorld();
        stoppedTime = TimeUtil::NanoSeconds();
    }
    evaluate_operation(operation);
    if (endSafepoint) {
        const uint64_t endTime = TimeUtil::NanoSeconds();
        GcLog::Stw(operation->name(), startTime, stoppedTime - startTime, endTime - stoppedTime);
        manager.StartTheWorld();
    }
    currentOperation = previous;
}

void VMThread::evaluate_operation(VMOperation* operation)
{
    operation->evaluate();
}

VMOperation* VMThread::wait_for_operation()
{
    std::unique_lock<std::mutex> guard(lock);
    condition.wait(guard, [this] { return nextOperation != nullptr || shouldTerminate; });
    return shouldTerminate ? nullptr : nextOperation;
}

void VMThread::loop()
{
    for (;;) {
        VMOperation* operation = wait_for_operation();
        if (operation == nullptr) { return; }
        inner_execute(operation);
        std::lock_guard<std::mutex> guard(lock);
        nextOperation = nullptr;
        condition.notify_all();
    }
}

void VMThread::run()
{
    ThreadLocal::SetThreadType(ThreadType::VM_THREAD);
    {
        std::lock_guard<std::mutex> guard(lock);
        nextOperation = nullptr;
        running = true;
        condition.notify_all();
    }
    loop();
    currentOperation = &haltOperation;
    // vmThread.cpp:189-190: the VM thread leaves at a safepoint, so it never
    // observes a resumed world.
    MutatorManager::Instance().StopTheWorld();
    {
        std::lock_guard<std::mutex> guard(lock);
        running = false;
        terminated = true;
        condition.notify_all();
    }
}

void VMThread::wait_for_vm_thread_exit()
{
    auto& vmThread = instance();
    // threads.cpp:961 exits the calling thread from the thread list before
    // :987 calls this entry, and vmThread.cpp:236-237 asserts the same
    // precondition. A caller still in the mutator set would be counted
    // against the world stop the VM thread performs at :189-190.
    CHECK_DETAIL(ThreadLocal::GetMutator() == nullptr,
        "VM thread exit waits on a caller that has left the mutator set");
    // vmThread.cpp:244-247: the caller is no longer in the thread list, so the
    // wait must not run anything the final safepoint can block on, and
    // :254-257 waits with a no-safepoint-check lock for the same reason.
    {
        std::unique_lock<std::mutex> guard(vmThread.lock);
        CHECK_DETAIL(vmThread.running && vmThread.nextOperation == nullptr, "VM thread must drain before shutdown");
        vmThread.shouldTerminate = true;
        vmThread.condition.notify_all();
        vmThread.condition.wait(guard, [&vmThread] { return vmThread.terminated; });
    }
    vmThread.thread.join();
}
}
