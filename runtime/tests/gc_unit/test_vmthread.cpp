// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_worker_fixture.hpp"
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

extern "C" int CJ_ScheduleManagerInit();

namespace {
// Container only. The operation itself is the product VMThread::execute path,
// and the phase call below is the product ZGeneration pause entry.
class VMThreadContainerRuntime final : public Runtime {
public:
    explicit VMThreadContainerRuntime(size_t heapUnits)
    {
        CreateStandaloneHeap(heapUnits);
        GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
        runtime = this;
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        manager.Init();
        concurrency.Init(ConcurrencyParam{1024, 64, 1});
        VMThread::create();
    }
    ~VMThreadContainerRuntime() override
    {
        VMThread::wait_for_vm_thread_exit();
        runtime = nullptr;
    }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam{}; }
    void SetGCThreshold(uint64_t) override {}

private:
    MutatorManager manager;
    Concurrency concurrency;
};

// Deterministic interleaving device, shaped after HotSpot
// threadHelper.inline.hpp:53-95: the submitting thread parks until the operation
// has published that it is executing, so the interleaving is not random.
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
} // namespace

GC_RUNTIME_TEST(VMThread1308, PauseRunsOnTheVMThread)
{
    VMThreadContainerRuntime container(8);
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    InitializeGenerationWorkers(heap.young(), 1);
    YoungTypeSetter type(heap.young(), ZYoungType::minor);
    const auto before = heap.total_collections();
    heap.young().pause_mark_start();
    const auto after = heap.total_collections();
    std::fprintf(stderr, "VM1308_PHASE_TARGET executed=1 before=%u after=%u vm_running=%d\n",
                 before, after, VMThread::is_running());
    // Target assertion: the product pause entered the VM thread and completed.
    GC_EXPECT_EQ(after, before + 1);
}

GC_RUNTIME_TEST(VMThread1308, SubmitterWaitsForCompletion)
{
    VMThreadContainerRuntime container(8);
    VMBlockingOperation operation;
    std::atomic<bool> returned{false};
    std::atomic<bool> completeAtReturn{false};
    std::thread::id submitter;
    std::thread caller([&] {
        submitter = std::this_thread::get_id();
        VMThread::execute(&operation);
        completeAtReturn.store(operation.complete.load(std::memory_order_acquire), std::memory_order_release);
        returned.store(true, std::memory_order_release);
    });
    bool reached = false;
    bool prematurelyReturned = false;
    {
        std::unique_lock<std::mutex> guard(operation.lock);
        reached = operation.condition.wait_for(guard, std::chrono::seconds(5),
                                               [&] { return operation.entered; });
        prematurelyReturned = returned.load(std::memory_order_acquire);
        operation.released = true;
        operation.condition.notify_all();
    }
    caller.join();
    std::fprintf(stderr, "VM1308_THREAD_TARGET executed=1 vm=%d separate=%d submitter=%d executor=%d\n",
                 operation.onVMThread, operation.executor != submitter,
                 static_cast<int>(submitter == std::thread::id{}),
                 static_cast<int>(operation.executor == std::thread::id{}));
    std::fprintf(stderr, "VM1308_COMPLETION_TARGET executed=1 reached=%d early=%d complete_at_return=%d\n",
                 reached, prematurelyReturned, completeAtReturn.load());
    // Precondition evidence, deliberately non-fatal so it cannot mask the
    // target assertion below.
    GC_EXPECT_TRUE(reached);
    // Target assertion I1: the operation body ran on the VM thread, not on
    // the submitting thread.
    GC_EXPECT_TRUE(operation.onVMThread && operation.executor != submitter);
    // Target assertion I1: the submitter returned only after the body finished.
    GC_EXPECT_TRUE(!prematurelyReturned && completeAtReturn.load());
}
