#include "Mutator/VMOperation.h"
#include "Base/Log.h"
#include "Common/ScopedObjectAccess.h"
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
    CHECK_DETAIL(!vmThread.running && !vmThread.terminated, "VM thread lifecycle");
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
    if (endSafepoint) { manager.StopTheWorld(); }
    operation->evaluate();
    if (endSafepoint) { manager.StartTheWorld(); }
    currentOperation = previous;
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
    for (;;) {
        VMOperation* operation;
        {
            std::unique_lock<std::mutex> guard(lock);
            condition.wait(guard, [this] { return nextOperation != nullptr || shouldTerminate; });
            if (shouldTerminate) { break; }
            operation = nextOperation;
        }
        inner_execute(operation);
        {
            std::lock_guard<std::mutex> guard(lock);
            nextOperation = nullptr;
            condition.notify_all();
        }
    }
    currentOperation = &haltOperation;
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
