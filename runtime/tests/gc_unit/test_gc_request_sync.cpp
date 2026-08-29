// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Request-port contract, following OpenJDK zDriverPort.cpp:101-184:
// synchronous callers wait for their request's completion index, while
// asynchronous callers return immediately and share one pending bitmap entry.

#if defined(MRT_GC_UNIT_TESTS)

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
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/DriverPort.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Inspector/ProfilerAgentImpl.h"
#include "Mutator/ThreadLocal.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

extern "C" void MCC_InvokeGCImpl(bool sync);

class CollectorResourcesTestPeer {
public:
    static void Init(CollectorResources& resources, Collector& collector, bool startNearReceiptWrap)
    {
        MRT_ASSERT(resources.taskQueue == nullptr, "gc_unit requires an uninitialized product task queue");
        static bool productCollectorInitialized = false;
        if (!productCollectorInitialized) {
            resources.collectorProxy.Init();
            productCollectorInitialized = true;
        }
        resources.testCollector = &collector;
        resources.GetMinorDriverPort().Reset();
        resources.GetMajorDriverPort().Reset();
        resources.taskQueue = new TaskQueue<GCExecutor>;
        resources.taskQueue->Init();
        if (startNearReceiptWrap) {
            resources.taskQueue->syncTaskIndex = GCTask::ASYNC_TASK_INDEX - 2;
        }
        resources.finishedGcIndex.store(resources.taskQueue->syncTaskIndex, std::memory_order_release);
        resources.isGCActive.store(true, std::memory_order_release);
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

    static bool ExecuteDriverRequest(CollectorResources& resources, const GCDriverRequest& request)
    {
        return resources.ExecuteDriverRequest(request);
    }

    static bool TryAcquireDriverLock(CollectorResources& resources)
    {
        std::unique_lock<std::mutex> lock(resources.driverLock, std::try_to_lock);
        return lock.owns_lock();
    }

    static void RunDriverLoop(CollectorResources& resources, GCDriverKind kind)
    {
        resources.RunDriverLoop(kind);
    }

    static void Destroy(CollectorResources& resources)
    {
        delete resources.taskQueue;
        resources.taskQueue = nullptr;
        resources.testCollector = nullptr;
    }
};

class RegionSpaceTestPeer {
public:
    static bool ShouldRetryAllocation(RegionSpace& space, size_t& tryTimes, size_t size)
    {
        return space.ShouldRetryAllocation(tryTimes, size);
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
        requestProgress.notify_all();
        return ignoreRequests;
    }
    FindToVersionResult FindToVersion(BaseObject*) const override
    {
        return FindToVersionResult::NotForwarded();
    }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return RefField<>(obj, ::g_cjStoreGoodMask);
    }

    void RunGarbageCollection(uint64_t gcIndex, GCReason reason) override
    {
        const size_t activeNow = activeRuns.fetch_add(1, std::memory_order_acq_rel) + 1;
        size_t observed = maxActiveRuns.load(std::memory_order_relaxed);
        while (activeNow > observed &&
               !maxActiveRuns.compare_exchange_weak(observed, activeNow, std::memory_order_relaxed)) {
        }
        if (advanceEpoch) {
            resources->SetGcStarted(true);
        }
        size_t runNumber = 0;
        {
            std::unique_lock<std::mutex> lock(mutex);
            indexes.push_back(gcIndex);
            reasons.push_back(reason);
            runNumber = indexes.size();
            entered.notify_all();
            release.wait(lock, [this, runNumber] { return releasedRuns >= runNumber; });
        }
        if (advanceEpoch) {
            g_gcCount.fetch_add(1, std::memory_order_release);
        }
        activeRuns.fetch_sub(1, std::memory_order_acq_rel);
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

    void SetAdvanceEpoch(bool value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        advanceEpoch = value;
    }

    bool WaitForIgnoreChecks(size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return ignoreChecked.wait_for(lock, kHarnessHangLimit, [this, count] { return ignoreChecks >= count; });
    }

    void NoteRequesterReturned()
    {
        std::lock_guard<std::mutex> lock(mutex);
        requesterReturned = true;
        requestProgress.notify_all();
    }

    bool WaitForRequestDecisionOrReturn()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return requestProgress.wait_for(lock, kHarnessHangLimit,
                                        [this] { return ignoreChecks != 0 || requesterReturned; });
    }

    bool HasRequesterReturned()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return requesterReturned;
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

    size_t MaxConcurrentRuns() const { return maxActiveRuns.load(std::memory_order_acquire); }

private:
    CollectorResources* resources = nullptr;
    std::mutex mutex;
    std::condition_variable entered;
    std::condition_variable ignoreChecked;
    std::condition_variable requestProgress;
    std::condition_variable release;
    bool ignoreRequests = false;
    bool advanceEpoch = false;
    std::atomic<size_t> activeRuns { 0 };
    std::atomic<size_t> maxActiveRuns { 0 };
    bool requesterReturned = false;
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
        CollectorResourcesTestPeer::Init(resources, collector, startNearReceiptWrap);
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
        CollectorResourcesTestPeer::Destroy(resources);
        Heap::GetHeap().EnableGC(gcWasEnabled);
    }

    BlockingCollector collector;
    CollectorResources& resources;

private:
    bool gcWasEnabled;
    std::thread consumer;
};

struct SyncResult {
    bool youngEntered;
    bool oldEntered;
    bool returnedBeforeYoungRelease;
    bool returnedBeforeOldRelease;
    bool returnedAfterRelease;
    size_t runCount;
    GCReason youngReason;
    GCReason oldReason;
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
        harness.collector.NoteRequesterReturned();
        returnedPromise.set_value();
    });
    JoinGuard requesterGuard(requester);

    bool requestMadeProgress = harness.collector.WaitForRequestDecisionOrReturn();
    bool youngEntered = requestMadeProgress && !harness.collector.HasRequesterReturned() &&
        harness.collector.WaitForRuns(1);
    bool returnedBeforeYoungRelease = returned.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    if (youngEntered) {
        harness.collector.ReleaseOne();
    }
    bool oldEntered = youngEntered && harness.collector.WaitForRuns(2);
    bool returnedBeforeOldRelease = returned.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    if (oldEntered) {
        harness.collector.ReleaseOne();
    }
    bool returnedAfterRelease = returned.wait_for(kHarnessHangLimit) == std::future_status::ready;
    if (!returnedAfterRelease) {
        harness.Stop();
    }
    requester.join();
    size_t runCount = harness.collector.RunCount();
    GCReason youngReason = youngEntered ? harness.collector.ReasonAt(0) : GC_REASON_INVALID;
    GCReason oldReason = oldEntered ? harness.collector.ReasonAt(1) : GC_REASON_INVALID;
    harness.Stop();
    return { youngEntered, oldEntered, returnedBeforeYoungRelease, returnedBeforeOldRelease,
             returnedAfterRelease, runCount, youngReason, oldReason };
}

void ExpectCompletedSync(const SyncResult& result, GCReason reason)
{
    GC_EXPECT_TRUE(result.youngEntered);
    GC_EXPECT_TRUE(result.oldEntered);
    GC_EXPECT_FALSE(result.returnedBeforeYoungRelease);
    GC_EXPECT_FALSE(result.returnedBeforeOldRelease);
    GC_EXPECT_TRUE(result.returnedAfterRelease);
    GC_EXPECT_EQ(result.runCount, 2u);
    GC_EXPECT_EQ(result.youngReason, GC_REASON_YOUNG);
    GC_EXPECT_EQ(result.oldReason, reason);
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

    auto runOne = [&](size_t expectedYoungRun, size_t expectedOldRun) {
        std::promise<void> returnedPromise;
        std::future<void> returned = returnedPromise.get_future();
        std::thread requester([&] {
            ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
            MCC_InvokeGCImpl(true);
            returnedPromise.set_value();
        });
        JoinGuard requesterGuard(requester);

        bool entered = harness.collector.WaitForRuns(expectedYoungRun);
        if (entered) {
            harness.collector.ReleaseOne();
            entered = harness.collector.WaitForRuns(expectedOldRun);
        }
        bool returnedBeforeCompletion = returned.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        if (entered) {
            harness.collector.ReleaseOne();
        }
        bool returnedAfterCompletion = returned.wait_for(kHarnessHangLimit) == std::future_status::ready;
        if (!returnedAfterCompletion) {
            harness.Stop();
        }
        requester.join();
        return RequestCompletionResult{ entered, returnedBeforeCompletion, returnedAfterCompletion };
    };

    RequestCompletionResult first = runOne(1, 2);
    if (!first.entered || !first.returnedAfterCompletion) {
        harness.Stop();
        return { first.entered, first.returnedBeforeCompletion, first.returnedAfterCompletion,
                 false, false, false };
    }
    RequestCompletionResult second = runOne(3, 4);
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

GC_TEST(GcRequestSync, GenerationPortsDoNotCoalesceAcrossDrivers)
{
    GCDriverPort minor(GCDriverKind::MINOR);
    GCDriverPort major(GCDriverKind::MAJOR);
    minor.EnqueueAsync(GC_REASON_YOUNG);
    major.EnqueueAsync(GC_REASON_HEU);
    major.EnqueueAsync(GC_REASON_HEU); // same-port async requests are deduplicated

    GCDriverRequest request {};
    GC_EXPECT_TRUE(minor.TryDequeue(request));
    GC_EXPECT_EQ(request.reason, GC_REASON_YOUNG);
    GC_EXPECT_TRUE(major.TryDequeue(request));
    GC_EXPECT_EQ(request.reason, GC_REASON_HEU);
    GC_EXPECT_FALSE(major.TryDequeue(request));
}

GC_TEST(GcRequestSync, DriverPortSyncSequenceAcknowledges)
{
    GCDriverPort port(GCDriverKind::MINOR);
    const uint64_t sequence = port.EnqueueSync(GC_REASON_YOUNG);
    GCDriverRequest request {};
    GC_EXPECT_TRUE(port.TryDequeue(request));
    GC_EXPECT_EQ(request.sequence, sequence);
    std::atomic<bool> completed { false };
    std::thread waiter([&] {
        completed.store(port.WaitForAck(sequence), std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    GC_EXPECT_FALSE(completed.load(std::memory_order_acquire));
    port.Acknowledge(sequence);
    waiter.join();
    GC_EXPECT_TRUE(completed.load(std::memory_order_acquire));
}

GC_TEST(GcRequestSync, DriverAbortIsCooperativeAndResettable)
{
    GCDriverPort port(GCDriverKind::MAJOR);
    GC_EXPECT_FALSE(port.Abort().Poll());
    port.Abort().Request();
    GC_EXPECT_TRUE(port.Abort().Poll());
    port.Abort().Reset();
    GC_EXPECT_FALSE(port.Abort().Poll());
}

GC_TEST(GcRequestSync, StoppedPortWakesSynchronousWaiter)
{
    GCDriverPort port(GCDriverKind::MAJOR);
    const uint64_t sequence = port.EnqueueSync(GC_REASON_FORCE);
    std::promise<bool> resultPromise;
    std::future<bool> result = resultPromise.get_future();
    std::thread waiter([&] { resultPromise.set_value(port.WaitForAck(sequence)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    port.Stop();
    GC_EXPECT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    GC_EXPECT_FALSE(result.get());
    waiter.join();
    GC_EXPECT_TRUE(port.IsStopped());
}

GC_TEST(GcRequestSync, MajorAbortpointSkipsOldAfterYoungPrelude)
{
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    BlockingCollector collector;
    collector.SetResources(resources);
    CollectorResourcesTestPeer::Init(resources, collector, false);
    std::promise<bool> completedPromise;
    std::future<bool> completedFuture = completedPromise.get_future();
    std::thread driver([&] {
        completedPromise.set_value(CollectorResourcesTestPeer::ExecuteDriverRequest(
            resources, GCDriverRequest { 2, GC_REASON_FORCE, false }));
    });
    GC_EXPECT_TRUE(collector.WaitForRuns(1));
    resources.GetMajorDriverPort().Abort().Request();
    collector.ReleaseOne();
    GC_EXPECT_EQ(completedFuture.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    const bool completed = completedFuture.get();
    driver.join();
    GC_EXPECT_FALSE(completed);
    GC_EXPECT_EQ(collector.RunCount(), 1u);
    GC_EXPECT_EQ(collector.ReasonAt(0), GC_REASON_YOUNG);
    resources.GetMajorDriverPort().Abort().Reset();
    CollectorResourcesTestPeer::Destroy(resources);
}

GC_TEST(GcRequestSync, DriverWaitInjectsTimeoutBackupRequest)
{
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    BlockingCollector collector;
    collector.SetResources(resources);
    CollectorResourcesTestPeer::Init(resources, collector, false);
    collector.ReleaseAll();
    Heap::GetHeap().EnableGC(true);
    std::thread driver([&] { CollectorResourcesTestPeer::RunDriverLoop(resources, GCDriverKind::MAJOR); });
    const bool youngObserved = collector.WaitForRuns(1);
    const bool backupObserved = collector.WaitForRuns(2);
    GC_EXPECT_TRUE(youngObserved);
    GC_EXPECT_TRUE(backupObserved);
    GC_EXPECT_EQ(collector.ReasonAt(0), GC_REASON_YOUNG);
    GC_EXPECT_EQ(collector.ReasonAt(1), GC_REASON_BACKUP);
    resources.GetMajorDriverPort().Stop();
    driver.join();
    CollectorResourcesTestPeer::Destroy(resources);
}

GC_TEST(GcRequestSync, MinorAndMajorDriversSerializeCollections)
{
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    BlockingCollector collector;
    collector.SetResources(resources);
    CollectorResourcesTestPeer::Init(resources, collector, false);
    std::thread minor([&] { CollectorResourcesTestPeer::RunDriverLoop(resources, GCDriverKind::MINOR); });
    std::thread major([&] { CollectorResourcesTestPeer::RunDriverLoop(resources, GCDriverKind::MAJOR); });
    resources.GetMinorDriverPort().EnqueueAsync(GC_REASON_YOUNG);
    const bool minorEntered = collector.WaitForRuns(1);
    const bool driverLockHeld = !CollectorResourcesTestPeer::TryAcquireDriverLock(resources);

    resources.GetMajorDriverPort().EnqueueAsync(GC_REASON_FORCE);
    collector.ReleaseOne();
    const bool majorYoungEntered = collector.WaitForRuns(2);
    collector.ReleaseOne();
    const bool majorOldEntered = collector.WaitForRuns(3);
    collector.ReleaseOne();
    const size_t maxConcurrentRuns = collector.MaxConcurrentRuns();
    const GCReason firstReason = collector.ReasonAt(0);
    const GCReason secondReason = collector.ReasonAt(1);
    const GCReason thirdReason = collector.ReasonAt(2);
    resources.GetMinorDriverPort().Stop();
    resources.GetMajorDriverPort().Stop();
    minor.join();
    major.join();
    CollectorResourcesTestPeer::Destroy(resources);

    // Assert only after both product driver loops are joined. A deliberate
    // lock cut must report this one test as red, not terminate the whole suite
    // through a joinable std::thread destructor.
    GC_EXPECT_TRUE(minorEntered);
    GC_EXPECT_TRUE(driverLockHeld);
    GC_EXPECT_TRUE(majorYoungEntered);
    GC_EXPECT_TRUE(majorOldEntered);
    GC_EXPECT_EQ(maxConcurrentRuns, 1u);
    GC_EXPECT_EQ(firstReason, GC_REASON_YOUNG);
    GC_EXPECT_EQ(secondReason, GC_REASON_YOUNG);
    GC_EXPECT_EQ(thirdReason, GC_REASON_FORCE);
}

GC_TEST(GcRequestSync, YoungSyncReturnsAfterEpochAndIdle)
{
    RequestHarness harness;
    harness.collector.SetAdvanceEpoch(true);
    const size_t epochBefore = g_gcCount.load(std::memory_order_acquire);
    std::promise<void> returnedPromise;
    std::future<void> returned = returnedPromise.get_future();
    std::atomic<size_t> epochAfter{ epochBefore };
    std::atomic<bool> startedAfter{ true };
    std::thread requester([&] {
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        harness.resources.RequestGC(GC_REASON_YOUNG, false);
        epochAfter.store(g_gcCount.load(std::memory_order_acquire), std::memory_order_release);
        startedAfter.store(harness.resources.IsGcStarted(), std::memory_order_release);
        returnedPromise.set_value();
    });
    JoinGuard requesterGuard(requester);

    bool entered = harness.collector.WaitForRuns(1);
    bool returnedBeforeRelease = returned.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    if (entered) {
        harness.collector.ReleaseOne();
    }
    bool returnedAfterRelease = returned.wait_for(kHarnessHangLimit) == std::future_status::ready;
    if (!returnedAfterRelease) {
        harness.Stop();
    }
    requester.join();
    harness.Stop();

    GC_EXPECT_TRUE(entered);
    GC_EXPECT_FALSE(returnedBeforeRelease);
    GC_EXPECT_TRUE(returnedAfterRelease);
    GC_EXPECT_TRUE(epochAfter.load(std::memory_order_acquire) > epochBefore);
    GC_EXPECT_FALSE(startedAfter.load(std::memory_order_acquire));
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

GC_TEST(GcRequestSync, AllocationRetryOomEntryWaitsForOwnCompletion)
{
    SyncResult result = RunSynchronousRequest([](RequestHarness&) {
        RegionSpace& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        size_t tryTimes = 5;
        bool shouldRetry = RegionSpaceTestPeer::ShouldRetryAllocation(space, tryTimes, 64);
        GC_EXPECT_TRUE(shouldRetry);
    });
    ExpectCompletedSync(result, GC_REASON_OOM);
}

GC_TEST(GcRequestSync, AllocationRetryHeuEntryWaitsForOwnCompletion)
{
    SyncResult result = RunSynchronousRequest([](RequestHarness&) {
        RegionSpace& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        size_t tryTimes = 4;
        bool shouldRetry = RegionSpaceTestPeer::ShouldRetryAllocation(space, tryTimes, 64);
        GC_EXPECT_TRUE(shouldRetry);
    });
    ExpectCompletedSync(result, GC_REASON_HEU);
}

#if defined(__OHOS__) && (__OHOS__ == 1)
GC_TEST(GcRequestSync, ProfilerCollectGarbageEntryWaitsForOwnCompletion)
{
    SyncResult result = RunSynchronousRequest([](RequestHarness&) {
        ProfilerAgentImpl("{\"id\":1,\"method\":\"collectGarbage\"}", [](const std::string&) {});
    });
    ExpectCompletedSync(result, GC_REASON_HEU);
}
#endif

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
    bool firstOldEntered = harness.collector.WaitForRuns(2);
    harness.collector.ReleaseOne();
    bool pendingYoungEntered = harness.collector.WaitForRuns(3);
    harness.collector.ReleaseOne();
    bool pendingOldEntered = harness.collector.WaitForRuns(4);
    harness.collector.ReleaseOne();
    harness.Stop();

    GC_EXPECT_TRUE(firstEntered);
    GC_EXPECT_TRUE(returnedBeforeRelease);
    GC_EXPECT_EQ(runsBeforeRelease, 1u);
    GC_EXPECT_TRUE(firstOldEntered);
    GC_EXPECT_TRUE(pendingYoungEntered);
    GC_EXPECT_TRUE(pendingOldEntered);
    GC_EXPECT_EQ(harness.collector.RunCount(), 4u);
    GC_EXPECT_EQ(harness.collector.ReasonAt(0), GC_REASON_YOUNG);
    GC_EXPECT_EQ(harness.collector.ReasonAt(1), GC_REASON_USER);
    GC_EXPECT_EQ(harness.collector.ReasonAt(2), GC_REASON_YOUNG);
    GC_EXPECT_EQ(harness.collector.ReasonAt(3), GC_REASON_USER);
}

} // namespace MapleRuntime

#endif // MRT_GC_UNIT_TESTS
