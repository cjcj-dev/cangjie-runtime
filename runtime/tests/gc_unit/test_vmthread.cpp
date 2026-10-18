// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "Common/Runtime.h"
#include "Cangjie.h"
#include "ObjectModel/MArray.inline.h"
#include "TypeInfoManager.h"
#include "Mutator/Mutator.inline.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zWorkers.inline.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zStat.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_vmthread_fixture.hpp"
#include "gc_worker_fixture.hpp"
#include "Mutator/MutatorManager.h"
#include "Mutator/VMOperation.h"
#include "Inspector/HeapSnapshotJsonSerializer.h"
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" int CJ_ScheduleManagerInit();
extern "C" void CJ_MRT_DumpHeapSnapshot(int fd);

namespace {
// These service entry points require the published VM initialization state.
// Keep the container-only fixture for coordinator tests that do not submit a
// VM_GC_Operation; do not manufacture the service admission bit in a fixture.
class InitializedServiceRuntime {
public:
    InitializedServiceRuntime()
    {
        RuntimeParam params{};
        params.heapParam.heapSize = 128 * 1024;
        params.coParam.processorNum = 1;
        params.gcParam.staticGCThreads = true;
        params.gcParam.concGCThreads = 2;
        params.gcParam.concGCThreadsSet = true;
        params.gcParam.youngGCThreads = 2;
        params.gcParam.youngGCThreadsSet = true;
        params.gcParam.oldGCThreads = 2;
        params.gcParam.oldGCThreadsSet = true;
        GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    }
    ~InitializedServiceRuntime() { CHECK(FiniCJRuntime() == E_OK); }
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

GC_RUNTIME_OTHER_VM_TEST(VMThread1308, PauseRunsOnTheVMThread)
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

GC_RUNTIME_OTHER_VM_TEST(VMThread1308, SubmitterWaitsForCompletion)
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

#include "Common/SuspendibleThreadSet.h"
#include "Mutator/Handshake.h"
#include "Mutator/ThreadSMR.h"
#include "Heap/z/zMarkStack.inline.hpp"
#include "Heap/z/zResurrection.hpp"

namespace {
class RegisteredSafeMutator {
public:
    Mutator target;
    RegisteredSafeMutator() { ThreadsSMRSupport::add_thread(&target); }
    ~RegisteredSafeMutator() { ThreadsSMRSupport::remove_thread(&target); }
};

class VMIdentityClosure final : public HandshakeClosure {
public:
    VMIdentityClosure() : HandshakeClosure("VM1349Identity") {}
    bool vm = false;
    bool stopped = true;
    Mutator* observed = nullptr;
    std::thread::id executor;
    void do_thread(Mutator* target) override
    {
        vm = VMThread::is_VM_thread();
        stopped = MutatorManager::Instance().WorldStopped();
        observed = target;
        executor = std::this_thread::get_id();
    }
};

class VMSeedMarkWork final : public VMOperation {
public:
    explicit VMSeedMarkWork(ZMark& domain) : domain(domain) {}
    bool vm = false;
    bool evaluate_at_safepoint() const override { return false; }
    const char* name() const override { return "VM1349SeedMarkWork"; }
    void doit() override
    {
        vm = VMThread::is_VM_thread();
        ThreadLocal::GetNativeGCData().markStacks[0].Push(
            domain.Stripes(), domain.Stripes().At(0), MarkStackEntry(size_t{17}, size_t{1}, false), false);
    }
private:
    ZMark& domain;
};

std::vector<size_t> PublishedOffsets(ZMark& domain)
{
    std::vector<size_t> values;
    for (size_t i = 0; i < domain.Stripes().NStripes(); ++i) {
        while (auto* stack = domain.Stripes().At(i)->StealStack(domain.Smr(), 0)) {
            while (!stack->IsEmpty()) { values.push_back(stack->Pop().partial_array_offset()); }
            MarkStripeStack::Destroy(stack);
        }
    }
    std::sort(values.begin(), values.end());
    return values;
}
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, GlobalHandshakeUsesVMThread)
{
    VMThreadContainerRuntime container(8);
    RegisteredSafeMutator target;
    VMIdentityClosure closure;
    const auto submitter = std::this_thread::get_id();
    Handshake::execute(&closure);
    std::fprintf(stderr, "VM1349_GLOBAL_TARGET vm=%d stopped=%d observed=%d separate=%d\n",
                 closure.vm, closure.stopped, closure.observed == &target.target, closure.executor != submitter);
    GC_EXPECT_TRUE(closure.vm && closure.observed == &target.target && closure.executor != submitter);
    GC_EXPECT_FALSE(closure.stopped);
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, SingleTargetUsesRequester)
{
    VMThreadContainerRuntime container(8);
    RegisteredSafeMutator target;
    VMIdentityClosure closure;
    const auto submitter = std::this_thread::get_id();
    Handshake::execute(&closure, &target.target);
    std::fprintf(stderr, "VM1349_SINGLE_TARGET vm=%d stopped=%d observed=%d requester=%d\n",
                 closure.vm, closure.stopped, closure.observed == &target.target, closure.executor == submitter);
    GC_EXPECT_TRUE(!closure.vm && closure.observed == &target.target && closure.executor == submitter);
    GC_EXPECT_FALSE(closure.stopped);
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, FlushPublishesVMAndMutatorWork)
{
    VMThreadContainerRuntime container(8);
    WorkerFixture worker;
    ZMark domain(16, MarkingStacks::MarkingGeneration::YOUNG);
    RegisteredSafeMutator target;
    VMSeedMarkWork seed(domain);
    VMThread::execute(&seed);
    target.target.GetGCData().markStacks[0].Push(
        domain.Stripes(), domain.Stripes().At(0), MarkStackEntry(size_t{23}, size_t{1}, false), false);
    ThreadLocal::GetNativeGCData().markStacks[0].Push(
        domain.Stripes(), domain.Stripes().At(0), MarkStackEntry(size_t{31}, size_t{1}, false), false);
    const bool flushed = domain.Flush();
    const auto offsets = PublishedOffsets(domain);
    const bool vmPublished = std::find(offsets.begin(), offsets.end(), 17) != offsets.end();
    const bool mutatorPublished = std::find(offsets.begin(), offsets.end(), 23) != offsets.end();
    const bool submitterRetained = !ThreadLocal::GetNativeGCData().markStacks[0].IsEmpty();
    // Clean the submitter's controlled input without making it part of the result.
    domain.Flush(ThreadLocal::GetNativeGCData());
    (void)PublishedOffsets(domain);
    std::fprintf(stderr, "VM1349_FLUSH_TARGET seeded_vm=%d vm=%d mutator=%d submitter_retained=%d flushed=%d\n",
                 seed.vm, vmPublished, mutatorPublished, submitterRetained, flushed);
    GC_EXPECT_TRUE(mutatorPublished);
    GC_EXPECT_TRUE(seed.vm && flushed && submitterRetained);
    std::fprintf(stderr, "VM1349_MUTATOR_AND_REQUESTER_CONTROL_ASSERT_EXECUTED\n");
    std::fprintf(stderr, "VM1349_VM_FLUSH_TARGET_ASSERT_EXECUTED\n");
    GC_EXPECT_TRUE(vmPublished);
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, EmptyVMFlushHasNoWork)
{
    VMThreadContainerRuntime container(8);
    ZMark domain(16, MarkingStacks::MarkingGeneration::YOUNG);
    const bool flushed = domain.Flush();
    std::fprintf(stderr, "VM1349_EMPTY_FLUSH_TARGET flushed=%d stopped=%d\n",
                 flushed, MutatorManager::Instance().WorldStopped());
    GC_EXPECT_FALSE(flushed);
    GC_EXPECT_FALSE(MutatorManager::Instance().WorldStopped());
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, MarkFollowLeavesSTSForProactiveFlush)
{
    VMThreadContainerRuntime container(8);
    auto& young = Heap::GetHeap().young();
    InitializeGenerationWorkers(young, 1);
    young.Mark().MarkFollow();
    std::fprintf(stderr, "VM1349_PROACTIVE_TARGET completed=1 stopped=%d\n",
                 MutatorManager::Instance().WorldStopped());
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    GC_EXPECT_FALSE(SuspendibleThreadSet::is_suspendible_thread());
#endif
    GC_EXPECT_FALSE(MutatorManager::Instance().WorldStopped());
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, DriverTerminateFlushOutsideSTS)
{
    VMThreadContainerRuntime container(8);
    auto& young = Heap::GetHeap().young();
    InitializeGenerationWorkers(young, 1);
    const bool flushed = young.Mark().TryTerminateFlush();
    std::fprintf(stderr, "VM1349_TERMINATE_TARGET completed=1 flushed=%d stopped=%d\n",
                 flushed, MutatorManager::Instance().WorldStopped());
    GC_EXPECT_FALSE(flushed);
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    GC_EXPECT_FALSE(SuspendibleThreadSet::is_suspendible_thread());
#endif
    GC_EXPECT_FALSE(MutatorManager::Instance().WorldStopped());
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, NonStrongRendezvousBeforeUnblock)
{
    VMThreadContainerRuntime container(8);
    auto& old = Heap::GetHeap().old();
    InitializeGenerationWorkers(old, 1);
    old.set_phase(ZGenerationPhase::MarkComplete);
    RegisteredSafeMutator target;
    const uint64_t beforeEpoch = target.target.GetStackWatermark().GetEpoch();
    RestoreMarkFlips flips;
    // Deterministic stale-owner input: the real rendezvous handshake must
    // process the safe target before GC rendezvous and resurrection unblock.
    ZGlobalsPointers::flip_old_mark_start();
    flips.old = true;
    const uint64_t expectedEpoch = StackWatermark::epoch_id();
    // zResurrection.cpp:31-33: establish the block at a real VM safepoint,
    // then exercise the concurrent non-strong-reference phase after it exits.
    class BlockResurrection final : public VMOperation {
    public:
        const char* name() const override { return "VM1349BlockResurrection"; }
        void doit() override { ZResurrection::block(); }
    } block;
    VMThread::execute(&block);
    std::mutex lock;
    std::condition_variable condition;
    bool joined = false;
    bool blockedAtRendezvous = false;
    bool stoppedAtRendezvous = true;
    std::thread participant([&] {
        SuspendibleThreadSetJoiner joiner;
        {
            std::lock_guard<std::mutex> guard(lock);
            joined = true;
            condition.notify_all();
        }
        while (!SuspendibleThreadSet::should_yield()) { std::this_thread::yield(); }
        blockedAtRendezvous = ZResurrection::is_blocked();
        stoppedAtRendezvous = MutatorManager::Instance().WorldStopped();
        joiner.yield();
    });
    {
        std::unique_lock<std::mutex> guard(lock);
        condition.wait(guard, [&] { return joined; });
    }
    old.process_non_strong_references();
    participant.join();
    const uint64_t afterEpoch = target.target.GetStackWatermark().GetEpoch();
    std::fprintf(stderr, "VM1349_RENDEZVOUS_TARGET blocked_during=%d stopped=%d unblocked_after=%d\n",
                 blockedAtRendezvous, stoppedAtRendezvous, !ZResurrection::is_blocked());
    std::fprintf(stderr, "VM1349_PHASE_HANDSHAKE_STATE_TARGET before=%llu after=%llu expected=%llu\n",
                 static_cast<unsigned long long>(beforeEpoch), static_cast<unsigned long long>(afterEpoch),
                 static_cast<unsigned long long>(expectedEpoch));
    GC_EXPECT_FALSE(stoppedAtRendezvous);
    std::fprintf(stderr, "VM1349_RUNNING_MUTATOR_CONTROL_ASSERT_EXECUTED\n");
    GC_EXPECT_TRUE(blockedAtRendezvous && !ZResurrection::is_blocked());
    GC_EXPECT_TRUE(beforeEpoch != expectedEpoch && afterEpoch == expectedEpoch);
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
namespace {
class IndirectStateTask1349 : public WorkerTask {
public:
    bool expectedSTS;
    bool expectedSafepoint;
    std::atomic<unsigned> matched{0};
    IndirectStateTask1349(bool sts, bool safepoint)
        : WorkerTask("1349 indirect state"), expectedSTS(sts), expectedSafepoint(safepoint) {}
    void work(uint32_t) override
    {
        auto* tls = ThreadLocal::GetThreadLocalData();
        const bool match = tls->isIndirectlySuspendibleThread == expectedSTS &&
                           tls->isIndirectlySafepointThread == expectedSafepoint;
        if (match) { matched.fetch_add(1); }
        std::fprintf(stderr, "INDIRECT1349_STATE sts=%d safepoint=%d match=%d\n",
                     tls->isIndirectlySuspendibleThread, tls->isIndirectlySafepointThread, match);
    }
};
class IndirectSubmitTask1349 : public WorkerTask {
public:
    IndirectSubmitTask1349() : WorkerTask("1349 indirect synchronous submission") {}
    void work(uint32_t) override
    {
        class Operation : public VMOperation {
        public:
            bool evaluate_at_safepoint() const override { return false; }
            const char* name() const override { return "1349 indirect prohibited operation"; }
            void doit() override { std::fprintf(stderr, "INDIRECT1349_UNEXPECTED_SUBMISSION_EXECUTED\n"); }
        } operation;
        std::fprintf(stderr, "INDIRECT1349_TARGET_EXECUTE sts=%d\n",
                     ThreadLocal::GetThreadLocalData()->isIndirectlySuspendibleThread);
        VMThread::execute(&operation);
    }
};
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, IndirectStatesFollowCoordinatorAndClear)
{
    VMThreadContainerRuntime container(8);
    WorkerThreads workers("1349 indirect", 2);
    workers.initialize_workers();
    IndirectStateTask1349 joined(true, false);
    { SuspendibleThreadSetJoiner joiner; workers.run_task(&joined); }
    std::fprintf(stderr, "INDIRECT1349_PRODUCER_TARGET matched=%u expected=2\n", joined.matched.load());
    GC_EXPECT_EQ(joined.matched.load(), 2u);
    IndirectStateTask1349 unjoined(false, false);
    workers.run_task(&unjoined);
    GC_EXPECT_EQ(unjoined.matched.load(), 2u);
    workers.stop();
    workers.initialize_workers();
    IndirectStateTask1349 restarted(false, false);
    workers.run_task(&restarted);
    GC_EXPECT_EQ(restarted.matched.load(), 2u);
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, IndirectSafepointStateFromVMCoordinator)
{
    VMThreadContainerRuntime container(8);
    WorkerThreads workers("1349 safepoint", 2);
    workers.initialize_workers();
    IndirectStateTask1349 safepointed(false, true);
    class Operation : public VMOperation {
        WorkerThreads& workers;
        WorkerTask& task;
    public:
        Operation(WorkerThreads& w, WorkerTask& t) : workers(w), task(t) {}
        const char* name() const override { return "1349 safepointed coordinator"; }
        void doit() override { workers.run_task(&task); }
    } operation(workers, safepointed);
    VMThread::execute(&operation);
    std::fprintf(stderr, "INDIRECT1349_SAFEPOINT_TARGET matched=%u expected=2\n", safepointed.matched.load());
    GC_EXPECT_EQ(safepointed.matched.load(), 2u);
    IndirectStateTask1349 cleared(false, false);
    workers.run_task(&cleared);
    GC_EXPECT_EQ(cleared.matched.load(), 2u);
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, IndirectSTSRejectsSynchronousSubmission)
{
    const char* scene = "GC_UNIT_1349_INDIRECT_SCENE";
    if (std::getenv(scene)) {
        VMThreadContainerRuntime container(8);
        WorkerThreads workers("1349 rejected", 1);
        workers.initialize_workers();
        IndirectSubmitTask1349 task;
        SuspendibleThreadSetJoiner joiner;
        workers.run_task(&task);
        _exit(0); // Expected-abort target must reject a returned submission.
    }
    GC_EXPECT_EQ(setenv(scene, "1", 1), 0);
    try {
        RunInOtherVm("ConcurrentVM1349.IndirectSTSRejectsSynchronousSubmission",
                     "VM operation submitter must not indirectly belong to STS");
    } catch (...) { unsetenv(scene); throw; }
    GC_EXPECT_EQ(unsetenv(scene), 0);
    std::fprintf(stderr, "INDIRECT1349_CONSUMER_TARGET matched_diagnostic=1\n");
}
#endif

// The IDE serializer is an OHOS product component (Inspector/CMakeLists.txt).
#if defined(__OHOS__) && (__OHOS__ == 1) && !defined(MRT_GC_UNIT_OHOS_HOST)
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
    InitializedServiceRuntime runtime;
    ObserveSnapshotTransport();
    CjHeapDataForIDE data;
    const bool result = data.Serialize();
    CheckSnapshotTransport();
    GC_EXPECT_TRUE(result);
}

GC_RUNTIME_OTHER_VM_TEST(VMService1350, OuterOperationAllowsNestedService)
{
    InitializedServiceRuntime runtime;
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
        InitializedServiceRuntime runtime;
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

#endif

GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, VMNestedSubmissionKeepsInternalRoute)
{
    VMThreadContainerRuntime container(8);
    bool executed = false;
    class Inner : public VMOperation {
        bool& executed;
    public:
        explicit Inner(bool& value) : executed(value) {}
        bool evaluate_at_safepoint() const override { return false; }
        const char* name() const override { return "1349 nested inner"; }
        void doit() override { executed = VMThread::is_VM_thread(); }
    } inner(executed);
    class Outer : public VMOperation {
        VMOperation& inner;
    public:
        explicit Outer(VMOperation& operation) : inner(operation) {}
        bool evaluate_at_safepoint() const override { return false; }
        bool allow_nested_vm_operations() const override { return true; }
        const char* name() const override { return "1349 nested outer"; }
        void doit() override { SuspendibleThreadSetJoiner joiner; VMThread::execute(&inner); }
    } outer(inner);
    VMThread::execute(&outer);
    std::fprintf(stderr, "INDIRECT1349_NESTED_CONTROL executed=%d\n", executed);
    GC_EXPECT_TRUE(executed);
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, RestartableDispatchReestablishesIndirectState)
{
    VMThreadContainerRuntime container(8);
    WorkerBudgetFixture budget(2);
    ZStatWorkers stats;
    ZWorkers workers(ZGenerationId::young, &stats);
    workers.set_active();
    class Restart : public ZRestartableTask {
        ZWorkers& workers;
    public:
        std::atomic<unsigned> matched{0};
        unsigned phase = 0;
        explicit Restart(ZWorkers& w) : ZRestartableTask("1349 restart indirect"), workers(w) {}
        void work() override
        {
            auto* tls = ThreadLocal::GetThreadLocalData();
            if (tls->isIndirectlySuspendibleThread && !tls->isIndirectlySafepointThread) { ++matched; }
            if (phase == 0 && WorkerThread::worker_id() == 0) { workers.request_resize_workers(1); }
        }
        void resize_workers(uint32_t) override { ++phase; }
    } task(workers);
    { SuspendibleThreadSetJoiner joiner; workers.run(&task); }
    std::fprintf(stderr, "INDIRECT1349_RESTART_TARGET phase=%u matched=%u expected=3\n",
                 task.phase, task.matched.load());
    GC_EXPECT_EQ(task.phase, 1u);
    GC_EXPECT_EQ(task.matched.load(), 3u);
    class Cleared : public ZTask {
    public:
        bool clean = false;
        Cleared() : ZTask("1349 restart cleared") {}
        void work() override
        {
            auto* tls = ThreadLocal::GetThreadLocalData();
            clean = !tls->isIndirectlySuspendibleThread && !tls->isIndirectlySafepointThread;
        }
    } cleared;
    workers.run(&cleared);
    GC_EXPECT_TRUE(cleared.clean);
}

#endif

#if !defined(MRT_DEBUG) || (MRT_DEBUG != 1)
GC_RUNTIME_OTHER_VM_TEST(ConcurrentVM1349, ReleaseCancelledSubmissionHasNoIdentityFatal)
{
    VMThreadContainerRuntime container(8);
    class Cancelled : public VMOperation {
    public:
        unsigned prologues = 0;
        bool ran = false;
        bool doit_prologue() override { ++prologues; return false; }
        bool evaluate_at_safepoint() const override { return false; }
        const char* name() const override { return "1349 cancelled release control"; }
        void doit() override { ran = true; }
    } operation;
    // HotSpot vmThread.cpp:529-530 has ASSERT preconditions, not product fatal checks.
    { SuspendibleThreadSetJoiner joiner; VMThread::execute(&operation); }
    class Submit : public WorkerTask {
        VMOperation& operation;
    public:
        explicit Submit(VMOperation& op) : WorkerTask("1349 release cancelled worker"), operation(op) {}
        void work(uint32_t) override { VMThread::execute(&operation); }
    } task(operation);
    WorkerThreads workers("1349 release control", 1);
    workers.initialize_workers();
    { SuspendibleThreadSetJoiner joiner; workers.run_task(&task); }
    VMThread::execute(&operation);
    std::fprintf(stderr, "RELEASE1349_IDENTITY_TARGET prologues=%u ran=%d expected=3\n",
                 operation.prologues, operation.ran);
    GC_EXPECT_EQ(operation.prologues, 3u);
    GC_EXPECT_FALSE(operation.ran);
}
#endif

namespace {
void CheckBinaryDump(FILE* file, bool result)
{
    const off_t size = lseek(fileno(file), 0, SEEK_END);
    std::fprintf(stderr, "VM1350_BINARY_TARGET executed=1 result=%d bytes=%lld fd_open=%d\n",
                 result, static_cast<long long>(size), size >= 0);
    GC_EXPECT_TRUE(result && size > 0);
}
class VMNestedBinaryDump final : public VMOperation {
public:
    VMNestedBinaryDump(int output, bool allow) : fd(output), allowNested(allow) {}
    bool allow_nested_vm_operations() const override { return allowNested; }
    const char* name() const override { return "nested binary dump request"; }
    void doit() override
    {
        CjHeapData data;
        result = data.DumpHeap(fd);
    }
    bool result = false;
private:
    int fd;
    bool allowNested;
};
}
GC_RUNTIME_OTHER_VM_TEST(VMService1350, BinaryDescriptorCallerRetainsFile)
{
    InitializedServiceRuntime runtime;
    FILE* file = tmpfile();
    GC_EXPECT_TRUE(file != nullptr);
    CjHeapData data;
    const bool result = data.DumpHeap(fileno(file));
    CheckBinaryDump(file, result);
    fclose(file);
}
GC_RUNTIME_OTHER_VM_TEST(VMService1350, OuterAllowsBinaryDump)
{
    InitializedServiceRuntime runtime;
    FILE* file = tmpfile();
    GC_EXPECT_TRUE(file != nullptr);
    VMNestedBinaryDump operation(fileno(file), true);
    VMThread::execute(&operation);
    CheckBinaryDump(file, operation.result);
    fclose(file);
}
GC_RUNTIME_OTHER_VM_TEST(VMService1350, OuterRejectsBinaryDump)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        InitializedServiceRuntime runtime;
        FILE* file = tmpfile();
        if (file == nullptr) { _exit(2); }
        VMNestedBinaryDump operation(fileno(file), false);
        VMThread::execute(&operation);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fprintf(stderr, "VM1350_BINARY_NESTED_TARGET executed=1 signaled=%d signal=%d\n",
                 WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    GC_EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

namespace {
struct ManagedDumpRequest { int fd; bool hadMutator = false; bool result = false; };
void* RequestBinaryDumpFromManagedThread(void* context)
{
    auto& request = *static_cast<ManagedDumpRequest*>(context);
    auto* mutator = Mutator::GetMutator();
    request.hadMutator = mutator != nullptr;
    // Native C++ task has no compiler-generated stack maps. The mutator
    // remains registered and must enter a saferegion while execute waits.
    mutator->SetManagedContext(false);
    CJ_MRT_DumpHeapSnapshot(request.fd);
    request.result = lseek(request.fd, 0, SEEK_END) > 0;
    mutator->SetManagedContext(true);
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(VMService1350, ManagedRequesterWaitsSafely)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 128 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.staticGCThreads = true;
    params.gcParam.concGCThreads = 2;
    params.gcParam.concGCThreadsSet = true;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.youngGCThreadsSet = true;
    params.gcParam.oldGCThreads = 2;
    params.gcParam.oldGCThreadsSet = true;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    FILE* file = tmpfile();
    GC_EXPECT_TRUE(file != nullptr);
    ManagedDumpRequest request{fileno(file)};
    auto task = RunCJTask(RequestBinaryDumpFromManagedThread, &request);
    GC_EXPECT_TRUE(task != nullptr);
    void* value = nullptr;
    const auto rc = GetTaskRet(task, &value);
    ReleaseHandle(task);
    std::fprintf(stderr, "VM1350_MANAGED_TARGET executed=1 task_rc=%d registered=%d\n",
                 rc, request.hadMutator);
    GC_EXPECT_EQ(rc, E_OK);
    GC_EXPECT_TRUE(request.hadMutator);
    CheckBinaryDump(file, request.result);
    fclose(file);
    FiniCJRuntime();
}

GC_RUNTIME_OTHER_VM_TEST(VMService1350, BinaryPathCallerOwnsFile)
{
    InitializedServiceRuntime runtime;
    const char* root = std::getenv("GC_UNIT_OUT");
    GC_EXPECT_TRUE(root != nullptr);
    const std::string directory = std::string(root) + "/dump-path-" + std::to_string(getpid());
    GC_EXPECT_EQ(mkdir(directory.c_str(), 0700), 0);
    GC_EXPECT_EQ(chdir(directory.c_str()), 0);
    CjHeapData data;
    data.DumpHeap();
    struct stat info{};
    const int rc = stat("item_data.dat", &info);
    std::fprintf(stderr, "VM1350_PATH_TARGET executed=1 stat_rc=%d bytes=%lld\n",
                 rc, static_cast<long long>(info.st_size));
    GC_EXPECT_TRUE(rc == 0 && info.st_size > 0);
    unlink("item_data.dat");
    GC_EXPECT_EQ(chdir(root), 0);
    rmdir(directory.c_str());
}

namespace MapleRuntime {
extern "C" ArrayRef MCC_GetAllThreadSnapshotImpl(const TypeInfo*, const TypeInfo*, const TypeInfo*);
extern "C" ThreadSnapshot MCC_GetCurrentThreadSnapshotImpl(const TypeInfo*, const TypeInfo*);
}
namespace {
struct ThreadDumpObservation {
    size_t count = 0;
    bool callerFound = false;
    uint64_t caller = 0;
};
void* RequestThreadSnapshot(void* context)
{
    auto& result = *static_cast<ThreadDumpObservation*>(context);
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[6][sizeof(TypeInfo)]{};
    auto* snapshot = reinterpret_cast<TypeInfo*>(storage[0]);
    auto* frame = reinterpret_cast<TypeInfo*>(storage[1]);
    auto* byte = reinterpret_cast<TypeInfo*>(storage[2]);
    auto* snapshots = reinterpret_cast<TypeInfo*>(storage[3]);
    auto* frames = reinterpret_cast<TypeInfo*>(storage[4]);
    auto* bytes = reinterpret_cast<TypeInfo*>(storage[5]);
    snapshot->SetType(TypeKind::TYPE_KIND_STRUCT);
    snapshot->SetInstanceSize(sizeof(ThreadSnapshot));
    snapshot->SetFlagHasRefField();
    GCTib snapshotTib{};
    snapshotTib.tag = SIGN_BIT | 5;
    snapshot->SetGCTib(snapshotTib);
    frame->SetType(TypeKind::TYPE_KIND_STRUCT);
    frame->SetInstanceSize(sizeof(StackTraceData));
    frame->SetFlagHasRefField();
    GCTib frameTib{};
    frameTib.tag = SIGN_BIT | 7;
    frame->SetGCTib(frameTib);
    byte->SetType(TypeKind::TYPE_KIND_UINT8);
    byte->SetInstanceSize(1);
    snapshots->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    snapshots->SetComponentTypeInfo(snapshot);
    frames->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    frames->SetComponentTypeInfo(frame);
    bytes->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    bytes->SetComponentTypeInfo(byte);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    result.caller = MCC_GetCurrentThreadSnapshotImpl(frames, bytes).id;
    auto* output = MCC_GetAllThreadSnapshotImpl(snapshots, frames, bytes);
    if (output != nullptr) {
        result.count = output->GetLength();
        for (size_t i = 0; i < result.count; ++i) {
            ThreadSnapshot snapshot{};
            std::memcpy(&snapshot, output->ConvertToCArray() + i * sizeof(snapshot), sizeof(snapshot));
            result.callerFound |= static_cast<uint64_t>(snapshot.id) == result.caller;
        }
    }
    mutator->SetManagedContext(true);
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(VMService1350, ThreadSnapshotCallerConsumesRecords)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 128 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.staticGCThreads = true;
    params.gcParam.concGCThreads = 2;
    params.gcParam.concGCThreadsSet = true;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.youngGCThreadsSet = true;
    params.gcParam.oldGCThreads = 2;
    params.gcParam.oldGCThreadsSet = true;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    ThreadDumpObservation result;
    auto task = RunCJTask(RequestThreadSnapshot, &result);
    GC_EXPECT_TRUE(task != nullptr);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "VM1350_SNAPSHOT_TARGET executed=1 count=%zu caller=%llu caller_found=%d\n",
                 result.count, static_cast<unsigned long long>(result.caller),
                 result.callerFound);
    GC_EXPECT_TRUE(result.callerFound);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
extern "C" void MRT_DumpAllStackTrace();
GC_RUNTIME_OTHER_VM_TEST(VMService1350, DebugStackTraceEntry)
{
    InitializedServiceRuntime runtime;
    MRT_DumpAllStackTrace();
    std::fprintf(stderr, "VM1350_STACKTRACE_TARGET executed=1 returned=1\n");
}
#endif
