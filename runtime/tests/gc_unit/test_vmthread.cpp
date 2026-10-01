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
#include "Inspector/HeapSnapshotJsonSerializer.h"
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
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
    const auto before = ZCollectedHeap::heap()->total_collections();
    heap.young().pause_mark_start();
    const auto after = ZCollectedHeap::heap()->total_collections();
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
        {
            std::lock_guard<std::mutex> guard(operation.lock);
            completeAtReturn.store(operation.complete.load(std::memory_order_acquire), std::memory_order_release);
            returned.store(true, std::memory_order_release);
        }
        operation.condition.notify_all();
    });
    bool reached = false;
    bool prematurelyReturned = false;
    {
        std::unique_lock<std::mutex> guard(operation.lock);
        reached = operation.condition.wait_for(guard, std::chrono::seconds(5),
                                               [&] { return operation.entered; });
        prematurelyReturned = reached && operation.condition.wait_for(guard, std::chrono::seconds(1),
            [&] { return returned.load(std::memory_order_acquire); });
        operation.released = true;
        operation.condition.notify_all();
    }
    caller.join();
    std::fprintf(stderr, "VM1308_THREAD_TARGET executed=1 vm=%d separate=%d submitter=%d executor=%d\n",
                 operation.onVMThread, operation.executor != submitter,
                 static_cast<int>(submitter == std::thread::id{}),
                 static_cast<int>(operation.executor == std::thread::id{}));
    std::fprintf(stderr, "VM1308_COMPLETION_TARGET executed=1 reached=%d early=%d complete_at_return=%d observe_ms=1000 wait_expired=%d\n",
                 reached, prematurelyReturned, completeAtReturn.load(), reached && !prematurelyReturned);
    // Precondition evidence, deliberately non-fatal so it cannot mask the
    // target assertion below.
    GC_EXPECT_TRUE(reached);
    // Target assertion I1: the operation body ran on the VM thread, not on
    // the submitting thread.
    GC_EXPECT_TRUE(operation.onVMThread && operation.executor != submitter);
    // Target assertion I1: the submitter returned only after the body finished.
    GC_EXPECT_TRUE(!prematurelyReturned && completeAtReturn.load());
}

namespace {
// Observe the existing profiler transport, not a test-only product hook.
struct SnapshotObservation {
    unsigned messages = 0;
    bool onVMThread = true;
    bool stopped = true;
    std::string output;
};
SnapshotObservation snapshotObservation;
void ObserveSnapshotTransport()
{
    snapshotObservation = SnapshotObservation{};
    auto& stream = HeapProfilerStream::GetInstance();
    stream.SetMessageID("{\"id\":1350}");
    stream.SetHandler([](const std::string& message) {
        ++snapshotObservation.messages;
        snapshotObservation.onVMThread &= VMThread::is_VM_thread();
        snapshotObservation.stopped &= MutatorManager::Instance().WorldStopped();
        snapshotObservation.output += message;
    });
}
void CheckSnapshotTransport()
{
    std::fprintf(stderr, "VM1350_IDE_TARGET executed=1 messages=%u vm=%d stopped=%d result=%d\n",
                 snapshotObservation.messages, snapshotObservation.onVMThread,
                 snapshotObservation.stopped,
                 snapshotObservation.output.find("\"id\":1350") != std::string::npos);
    GC_EXPECT_TRUE(snapshotObservation.messages > 0);
    GC_EXPECT_TRUE(snapshotObservation.onVMThread && snapshotObservation.stopped);
    GC_EXPECT_TRUE(snapshotObservation.output.find("\"id\":1350") != std::string::npos);
}
class VMNestedSnapshot final : public VMOperation {
public:
    explicit VMNestedSnapshot(bool allow) : allowNested(allow) {}
    bool allow_nested_vm_operations() const override { return allowNested; }
    const char* name() const override { return "nested profiler request"; }
    void doit() override
    {
        CjHeapDataForIDE data;
        result = data.Serialize();
    }
    bool result = false;
private:
    bool allowNested;
};
}

GC_RUNTIME_OTHER_VM_TEST(VMService1350, IDETransportRunsOnVMThread)
{
    VMThreadContainerRuntime container(8);
    ObserveSnapshotTransport();
    CjHeapDataForIDE data;
    const bool result = data.Serialize();
    CheckSnapshotTransport();
    GC_EXPECT_TRUE(result);
}

GC_RUNTIME_OTHER_VM_TEST(VMService1350, OuterOperationAllowsNestedService)
{
    VMThreadContainerRuntime container(8);
    ObserveSnapshotTransport();
    VMNestedSnapshot operation(true);
    VMThread::execute(&operation);
    CheckSnapshotTransport();
    GC_EXPECT_TRUE(operation.result);
}

GC_RUNTIME_OTHER_VM_TEST(VMService1350, OuterOperationRejectsNestedService)
{
    // fork before creating a VM thread; the child owns its complete runtime.
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        VMThreadContainerRuntime container(8);
        ObserveSnapshotTransport();
        VMNestedSnapshot operation(false);
        VMThread::execute(&operation);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fprintf(stderr, "VM1350_NESTED_TARGET executed=1 signaled=%d signal=%d\n",
                 WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    GC_EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
