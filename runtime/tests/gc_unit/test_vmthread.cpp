#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "gc_worker_fixture.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Mutator/MutatorManager.h"
#include "Mutator/VMOperation.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
class VMBlockingOperation : public VMOperation {
public:
    std::mutex lock;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    std::atomic<bool> complete{false};
    bool onVMThread = false;
    std::thread::id executor;
    bool evaluate_at_safepoint() const override { return false; }
    const char* name() const override { return "VMThread synchronous completion"; }
    void doit() override
    {
        onVMThread = VMThread::is_VM_thread();
        executor = std::this_thread::get_id();
        std::unique_lock<std::mutex> guard(lock);
        entered = true;
        condition.notify_all();
        condition.wait(guard, [this] { return released; });
        complete.store(true, std::memory_order_release);
    }
};
}

GC_RUNTIME_OTHER_VM_TEST(VMThread1308, PauseAndSynchronousCompletion)
{
    RuntimeParam parameters{};
    parameters.heapParam.heapSize = 128 * 1024;
    parameters.coParam.processorNum = 1;
    parameters.gcParam.concGCThreads = 2;
    parameters.gcParam.concGCThreadsSet = true;
    parameters.gcParam.youngGCThreads = 1;
    parameters.gcParam.youngGCThreadsSet = true;
    parameters.gcParam.oldGCThreads = 1;
    parameters.gcParam.oldGCThreadsSet = true;
    parameters.gcParam.staticGCThreads = true;
    GC_EXPECT_EQ(InitCJRuntime(&parameters), E_OK);
    auto& heap = Heap::GetHeap();
    const auto before = heap.total_collections();
    heap.young().pause_mark_start();
    const auto after = heap.total_collections();
    std::fprintf(stderr, "VM1308_PHASE_TARGET executed=1 before=%u after=%u ready=%d\n",
                 before, after, VMThread::is_running());
    GC_EXPECT_EQ(after, before + 1);
    VMBlockingOperation operation;
    std::atomic<bool> returned{false}, completeAtReturn{false};
    std::thread::id submitter;
    std::thread caller([&] {
        submitter = std::this_thread::get_id();
        VMThread::execute(&operation);
        completeAtReturn.store(operation.complete.load(std::memory_order_acquire), std::memory_order_release);
        returned.store(true, std::memory_order_release);
    });
    bool reached;
    bool prematurelyReturned;
    {
        std::unique_lock<std::mutex> guard(operation.lock);
        reached = operation.condition.wait_for(guard, std::chrono::seconds(5),
                                               [&] { return operation.entered; });
        prematurelyReturned = returned.load(std::memory_order_acquire);
        operation.released = true;
        operation.condition.notify_all();
    }
    caller.join();
    const bool identity = operation.onVMThread && operation.executor != submitter;
    const bool completion = completeAtReturn.load(std::memory_order_acquire) && !prematurelyReturned;
    std::fprintf(stderr, "VM1308_THREAD_TARGET executed=1 vm=%d separate=%d result=%d\n",
                 operation.onVMThread, operation.executor != submitter, identity);
    std::fprintf(stderr, "VM1308_COMPLETION_TARGET executed=1 complete_at_return=%d early=%d result=%d reached=%d\n",
                 completeAtReturn.load(), prematurelyReturned, completion, reached);
    GC_EXPECT_TRUE(identity);
    GC_EXPECT_TRUE(completion);
    GC_EXPECT_TRUE(reached);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
