// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Request-port contract, following OpenJDK zDriverPort.cpp:101-184:
// synchronous callers wait for their request's completion index, while
// asynchronous callers return immediately and share one pending bitmap entry.

#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "gc_unittest.hpp"

#include "Cangjie.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Heap.h"
#include "Mutator/ThreadLocal.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

extern "C" void MCC_InvokeGCImpl(bool sync);

class CollectorResourcesTestPeer {
public:
    static Collector* Init(CollectorResources& resources, Collector& collector, bool startNearReceiptWrap)
    {
        MRT_ASSERT(resources.taskQueue == nullptr, "gc_unit requires an uninitialized product task queue");
        Collector* productCollector = resources.collector;
        resources.collector = &collector;
        resources.taskQueue = new TaskQueue<GCExecutor>;
        resources.taskQueue->Init();
        if (startNearReceiptWrap) {
            resources.taskQueue->syncTaskIndex = GCTask::ASYNC_TASK_INDEX - 2;
        }
        resources.finishedGcIndex.store(resources.taskQueue->syncTaskIndex, std::memory_order_release);
        resources.isGCActive.store(true, std::memory_order_release);
        return productCollector;
    }

    static bool ShouldWaitForIgnoredGcRequest(GCReason reason, bool async)
    {
        return CollectorResources::ShouldWaitForIgnoredGcRequest(reason, async);
    }

    static void RequestStop(CollectorResources& resources)
    {
        TaskQueue<GCExecutor>::TaskFilter filter = [](GCExecutor&, GCExecutor&) { return false; };
        GCExecutor task(GCTask::TaskType::TASK_TYPE_TERMINATE_GC);
        (void)resources.taskQueue->EnqueueSync(task, filter);
    }

    static void Destroy(CollectorResources& resources, Collector* productCollector)
    {
        delete resources.taskQueue;
        resources.taskQueue = nullptr;
        resources.collector = productCollector;
    }
};

namespace {

// This is only a harness deadlock guard. Correctness assertions below use
// request-entry, release, and completion events rather than elapsed time.
constexpr auto kHarnessHangLimit = std::chrono::seconds(30);

class BlockingCollector final : public Collector {
public:
    void SetResources(CollectorResources& value) { resources = &value; }

    void Init() override {}
    bool ShouldIgnoreRequest(GCRequest&) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++ignoreChecks;
        ignoreChecked.notify_all();
        return ignoreRequests;
    }
    BaseObject* FindToVersion(BaseObject*) const override { return nullptr; }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return RefField<>(to_zpointer(reinterpret_cast<MAddress>(obj)));
    }

    void RunGarbageCollection(uint64_t gcIndex, GCReason reason) override
    {
        size_t runNumber = 0;
        {
            std::unique_lock<std::mutex> lock(mutex);
            indexes.push_back(gcIndex);
            reasons.push_back(reason);
            runNumber = indexes.size();
            entered.notify_all();
            release.wait(lock, [this, runNumber] { return releasedRuns >= runNumber; });
        }
        resources->NotifyGCFinished(gcIndex);
    }

    bool WaitForRuns(size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return entered.wait_for(lock, kHarnessHangLimit, [this, count] { return indexes.size() >= count; });
    }

    void SetIgnoreRequests(bool value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ignoreRequests = value;
    }

    bool WaitForIgnoreChecks(size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return ignoreChecked.wait_for(lock, kHarnessHangLimit, [this, count] { return ignoreChecks >= count; });
    }

    void ReleaseOne()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++releasedRuns;
        release.notify_all();
    }

    void ReleaseAll()
    {
        std::lock_guard<std::mutex> lock(mutex);
        releasedRuns = std::numeric_limits<size_t>::max();
        release.notify_all();
    }

    size_t RunCount()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return indexes.size();
    }

    GCReason ReasonAt(size_t position)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return reasons.at(position);
    }

private:
    CollectorResources* resources = nullptr;
    std::mutex mutex;
    std::condition_variable entered;
    std::condition_variable ignoreChecked;
    std::condition_variable release;
    bool ignoreRequests = false;
    size_t ignoreChecks = 0;
    size_t releasedRuns = 0;
    std::vector<uint64_t> indexes;
    std::vector<GCReason> reasons;
};

class RequestHarness {
public:
    explicit RequestHarness(bool startNearReceiptWrap = false)
        : resources(Heap::GetHeap().GetCollectorResources()), gcWasEnabled(Heap::GetHeap().IsGCEnabled())
    {
        Heap::GetHeap().EnableGC(true);
        collector.SetResources(resources);
        productCollector = CollectorResourcesTestPeer::Init(resources, collector, startNearReceiptWrap);
        consumer = std::thread([this] { resources.RunTaskLoop(); });
    }

    ~RequestHarness() { Stop(); }

    void Stop()
    {
        if (!consumer.joinable()) {
            return;
        }
        collector.ReleaseAll();
        CollectorResourcesTestPeer::RequestStop(resources);
        consumer.join();
        CollectorResourcesTestPeer::Destroy(resources, productCollector);
        Heap::GetHeap().EnableGC(gcWasEnabled);
    }

    BlockingCollector collector;
    CollectorResources& resources;

private:
    Collector* productCollector = nullptr;
    bool gcWasEnabled;
    std::thread consumer;
};

struct SyncResult {
    bool entered;
    bool returnedBeforeRelease;
    bool returnedAfterRelease;
    size_t runCount;
    GCReason reason;
};

SyncResult RunSynchronousRequest(const std::function<void(RequestHarness&)>& request)
{
    RequestHarness harness;
    std::promise<void> returnedPromise;
    std::future<void> returned = returnedPromise.get_future();
    std::thread requester([&] {
        // The standalone gc_unit process has no registered CJ mutator hooks.
        // An FP runtime thread has no mutator and is a supported synchronous caller.
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        request(harness);
        returnedPromise.set_value();
    });
    JoinGuard requesterGuard(requester);

    bool entered = harness.collector.WaitForRuns(1);
    bool returnedBeforeRelease = returned.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    harness.collector.ReleaseOne();
    bool returnedAfterRelease = returned.wait_for(kHarnessHangLimit) == std::future_status::ready;
    if (!returnedAfterRelease) {
        harness.Stop();
    }
    requester.join();
    size_t runCount = harness.collector.RunCount();
    GCReason observedReason = entered ? harness.collector.ReasonAt(0) : GC_REASON_INVALID;
    harness.Stop();
    return { entered, returnedBeforeRelease, returnedAfterRelease, runCount, observedReason };
}

void ExpectCompletedSync(const SyncResult& result, GCReason reason)
{
    GC_EXPECT_TRUE(result.entered);
    GC_EXPECT_FALSE(result.returnedBeforeRelease);
    GC_EXPECT_TRUE(result.returnedAfterRelease);
    GC_EXPECT_EQ(result.runCount, 1u);
    GC_EXPECT_EQ(result.reason, reason);
}

struct BoundaryResult {
    bool firstEntered;
    bool firstReturnedBeforeCompletion;
    bool firstReturnedAfterCompletion;
    bool secondEntered;
    bool secondReturnedBeforeCompletion;
    bool secondReturnedAfterCompletion;
};

struct RequestCompletionResult {
    bool entered;
    bool returnedBeforeCompletion;
    bool returnedAfterCompletion;
};

BoundaryResult RunSynchronousRequestsAcrossReceiptWrap()
{
    RequestHarness harness(true);

    auto runOne = [&](size_t expectedRuns) {
        std::promise<void> returnedPromise;
        std::future<void> returned = returnedPromise.get_future();
        std::thread requester([&] {
            ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
            MCC_InvokeGCImpl(true);
            returnedPromise.set_value();
        });
        JoinGuard requesterGuard(requester);

        bool entered = harness.collector.WaitForRuns(expectedRuns);
        bool returnedBeforeCompletion = returned.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        harness.collector.ReleaseOne();
        bool returnedAfterCompletion = returned.wait_for(kHarnessHangLimit) == std::future_status::ready;
        if (!returnedAfterCompletion) {
            harness.Stop();
        }
        requester.join();
        return RequestCompletionResult{ entered, returnedBeforeCompletion, returnedAfterCompletion };
    };

    RequestCompletionResult first = runOne(1);
    if (!first.entered || !first.returnedAfterCompletion) {
        harness.Stop();
        return { first.entered, first.returnedBeforeCompletion, first.returnedAfterCompletion,
                 false, false, false };
    }
    RequestCompletionResult second = runOne(2);
    harness.Stop();
    return { first.entered, first.returnedBeforeCompletion, first.returnedAfterCompletion,
             second.entered, second.returnedBeforeCompletion, second.returnedAfterCompletion };
}

int WaitChild(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFSIGNALED(status)) {
        return WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

} // namespace

GC_TEST(GcRequestSync, CompilerSyncEntryWaitsForOwnCompletion)
{
    SyncResult result = RunSynchronousRequest([](RequestHarness&) { MCC_InvokeGCImpl(true); });
    ExpectCompletedSync(result, GC_REASON_USER);
}

GC_TEST(GcRequestSync, ForceFullEntryWaitsForOwnCompletion)
{
    SyncResult result = RunSynchronousRequest([](RequestHarness&) { CJ_MRT_ForceFullGC(); });
    ExpectCompletedSync(result, GC_REASON_USER);
}

GC_TEST(GcRequestSync, OomAndForceRemainSynchronous)
{
    for (GCReason reason : { GC_REASON_OOM, GC_REASON_FORCE }) {
        SyncResult result = RunSynchronousRequest([reason](RequestHarness& harness) {
            harness.resources.RequestGC(reason, false);
        });
        ExpectCompletedSync(result, reason);
    }
}

GC_TEST(GcRequestSync, IgnoredStaticSyncReasonsWaitForCurrentGc)
{
    for (GCReason reason : { GC_REASON_OOM, GC_REASON_FORCE }) {
        RequestHarness harness;
        harness.collector.SetIgnoreRequests(true);
        harness.resources.SetGcStarted(true);

        std::promise<void> returnedPromise;
        std::future<void> returned = returnedPromise.get_future();
        std::atomic<bool> completionPublished{ false };
        std::atomic<bool> returnedBeforeCompletion{ false };
        std::thread requester([&] {
            ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
            harness.resources.RequestGC(reason, true);
            returnedBeforeCompletion.store(!completionPublished.load(std::memory_order_acquire),
                                           std::memory_order_release);
            returnedPromise.set_value();
        });
        JoinGuard requesterGuard(requester);

        bool ignoredPathEntered = harness.collector.WaitForIgnoreChecks(1);
        bool returnedBeforeFinish = returned.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        completionPublished.store(true, std::memory_order_release);
        harness.resources.NotifyGCFinished(GCTask::SYNC_TASK_MIN_INDEX);
        bool returnedAfterFinish = returned.wait_for(kHarnessHangLimit) == std::future_status::ready;
        if (!returnedAfterFinish) {
            harness.Stop();
        }
        requester.join();
        harness.Stop();

        GC_EXPECT_TRUE(ignoredPathEntered);
        GC_EXPECT_FALSE(returnedBeforeFinish);
        GC_EXPECT_FALSE(returnedBeforeCompletion.load(std::memory_order_acquire));
        GC_EXPECT_TRUE(returnedAfterFinish);
        GC_EXPECT_TRUE(CollectorResourcesTestPeer::ShouldWaitForIgnoredGcRequest(reason, true));
    }
}

GC_TEST(GcRequestSync, IgnoredUserSyncWaitsForCurrentGcCompletion)
{
    RequestHarness harness;
    harness.collector.SetIgnoreRequests(true);
    harness.resources.SetGcStarted(true);

    std::promise<void> returnedPromise;
    std::future<void> returned = returnedPromise.get_future();
    std::atomic<bool> completionPublished{ false };
    std::atomic<bool> returnedBeforeCompletion{ false };
    std::thread requester([&] {
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        MCC_InvokeGCImpl(true);
        returnedBeforeCompletion.store(!completionPublished.load(std::memory_order_acquire),
                                       std::memory_order_release);
        returnedPromise.set_value();
    });
    JoinGuard requesterGuard(requester);

    bool ignoredPathEntered = harness.collector.WaitForIgnoreChecks(1);
    bool returnedWhileGcStarted = returned.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    completionPublished.store(true, std::memory_order_release);
    harness.resources.NotifyGCFinished(GCTask::SYNC_TASK_MIN_INDEX);
    bool returnedAfterCompletion = returned.wait_for(kHarnessHangLimit) == std::future_status::ready;
    if (!returnedAfterCompletion) {
        harness.Stop();
    }
    requester.join();
    harness.Stop();

    GC_EXPECT_TRUE(ignoredPathEntered);
    GC_EXPECT_FALSE(returnedWhileGcStarted);
    GC_EXPECT_FALSE(returnedBeforeCompletion.load(std::memory_order_acquire));
    GC_EXPECT_TRUE(returnedAfterCompletion);
}

GC_TEST(GcRequestSync, IgnoredUserAsyncReturnsWhileCurrentGcIsActive)
{
    RequestHarness harness;
    harness.collector.SetIgnoreRequests(true);
    harness.resources.SetGcStarted(true);

    std::promise<void> returnedPromise;
    std::future<void> returned = returnedPromise.get_future();
    std::thread requester([&] {
        MCC_InvokeGCImpl(false);
        returnedPromise.set_value();
    });
    JoinGuard requesterGuard(requester);

    bool ignoredPathEntered = harness.collector.WaitForIgnoreChecks(1);
    bool returnedWhileGcStarted = returned.wait_for(kHarnessHangLimit) == std::future_status::ready &&
        harness.resources.IsGcStarted();
    harness.resources.NotifyGCFinished(GCTask::SYNC_TASK_MIN_INDEX);
    requester.join();
    harness.Stop();

    GC_EXPECT_TRUE(ignoredPathEntered);
    GC_EXPECT_TRUE(returnedWhileGcStarted);
}

GC_TEST(GcRequestSync, SynchronousRequestsWaitForOwnCompletionAcrossReceiptWrap)
{
    BoundaryResult result = RunSynchronousRequestsAcrossReceiptWrap();
    GC_EXPECT_TRUE(result.firstEntered);
    GC_EXPECT_FALSE(result.firstReturnedBeforeCompletion);
    GC_EXPECT_TRUE(result.firstReturnedAfterCompletion);
    GC_EXPECT_TRUE(result.secondEntered);
    GC_EXPECT_FALSE(result.secondReturnedBeforeCompletion);
    GC_EXPECT_TRUE(result.secondReturnedAfterCompletion);
}

GC_TEST(GcRequestSync, AcceptedStaticSyncReasonRejectsAsyncPort)
{
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        RequestHarness harness;
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        harness.resources.RequestGC(GC_REASON_OOM, true);
        _exit(0);
    }
    GC_EXPECT_EQ(WaitChild(pid), SIGABRT);
}

GC_TEST(GcRequestSync, CompilerAsyncEntryReturnsAndMergesPendingRequest)
{
    RequestHarness harness;

    std::promise<void> returnedPromise;
    std::future<void> returned = returnedPromise.get_future();
    std::thread requester([&] {
        MCC_InvokeGCImpl(false);
        returnedPromise.set_value();
    });
    JoinGuard requesterGuard(requester);
    bool firstEntered = harness.collector.WaitForRuns(1);
    bool returnedBeforeRelease = returned.wait_for(kHarnessHangLimit) == std::future_status::ready;

    if (!firstEntered) {
        if (requester.joinable()) {
            requester.join();
        }
        harness.Stop();
        GC_EXPECT_TRUE(firstEntered);
        return;
    }

    constexpr size_t duplicateRequests = 8;
    if (returnedBeforeRelease) {
        requester.join();
        for (size_t i = 0; i < duplicateRequests; ++i) {
            MCC_InvokeGCImpl(false);
        }
    }
    size_t runsBeforeRelease = harness.collector.RunCount();

    harness.collector.ReleaseOne();
    if (requester.joinable()) {
        requester.join();
    }
    bool pendingEntered = harness.collector.WaitForRuns(2);
    harness.Stop();

    GC_EXPECT_TRUE(firstEntered);
    GC_EXPECT_TRUE(returnedBeforeRelease);
    GC_EXPECT_EQ(runsBeforeRelease, 1u);
    GC_EXPECT_TRUE(pendingEntered);
    GC_EXPECT_EQ(harness.collector.RunCount(), 2u);
    GC_EXPECT_EQ(harness.collector.ReasonAt(0), GC_REASON_USER);
    GC_EXPECT_EQ(harness.collector.ReasonAt(1), GC_REASON_USER);
}

} // namespace MapleRuntime
