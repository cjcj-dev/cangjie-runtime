// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zDriver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#if defined(__linux__) || defined(hongmeng)
#include <sched.h>
#endif
#include <thread>

#include "Base/SysCall.h"
#include "CangjieRuntime.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Common/Runtime.h"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Common/RunType.h"
#include "Common/ScopedObjectAccess.h"
#include "LoaderManager.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
std::atomic<uint64_t> g_gcTriggerArmed{ 0 };
std::atomic<uint64_t> g_gcTriggerTurned{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleTimer{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleWarmup{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleAllocRate{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleHighUsage{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleMajorAllocRateArmed{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleMajorAllocRate{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleProactiveArmed{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleProactive{ 0 };
std::atomic<uint32_t> g_gcTriggerYoungWorkers{ 1 };
std::atomic<uint32_t> g_gcTriggerOldWorkers{ 1 };


extern "C" uintptr_t MRT_StopGCWork()
{
    Heap::GetHeap().StopGCWork();
    return 0;
}

void* CollectorResources::MinorDriverThreadEntry(void* arg)
{
    auto* resources = reinterpret_cast<CollectorResources*>(arg);
    MRT_ASSERT(resources != nullptr, "MinorDriverThreadEntry arg=nullptr");
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
    resources->RunDriverLoop(GCDriverKind::MINOR);
    return nullptr;
}

void* CollectorResources::MajorDriverThreadEntry(void* arg)
{
    auto* resources = reinterpret_cast<CollectorResources*>(arg);
    MRT_ASSERT(resources != nullptr, "MajorDriverThreadEntry arg=nullptr");
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
    resources->RunDriverLoop(GCDriverKind::MAJOR);
    return nullptr;
}

void CollectorResources::Init()
{
    minorDriverPort.Reset();
    majorDriverPort.Reset();
    taskQueue = new TaskQueue<GCExecutor>;
    taskQueue->Init();
    finishedGcIndex = GCTask::SYNC_TASK_MIN_INDEX;
    ZStat::Initialize();
    GetGCStats(GCCycleGeneration::YOUNG).Init();
    GetGCStats(GCCycleGeneration::OLD).Init();
    ZStatMutatorAllocRate::initialize();
    const uint64_t now = TimeUtil::NanoSeconds();
    youngCycle.Initialize(now);
    oldCycle.Initialize(now);
    statistics.Start();
    StartGCThreads();
    finalizerProcessor.Start();
    StringDedup::Instance().Start();
    if (Uncommitter::Enabled()) {
        LOG(RTLOG_INFO, "Uncommit: Enabled delay=%zus",
            static_cast<size_t>(Uncommitter::DelayNs() / SECOND_TO_NANO_SECOND));
    } else {
        LOG(RTLOG_INFO, "Uncommit: Disabled");
    }
}

void CollectorResources::Fini()
{
    MRT_ASSERT(!finalizerProcessor.IsRunning(), "Invalid finalizerProcessor status");
    MRT_ASSERT(!gcThreadRunning.load(std::memory_order_relaxed), "Invalid GC thread status");
    taskQueue->Fini();
    delete taskQueue;
    taskQueue = nullptr;
    minorDriverPort.Stop();
    majorDriverPort.Stop();
}

void CollectorResources::StopGCWork()
{
    finalizerProcessor.Stop();
    {
        std::lock_guard<std::mutex> lock(directorMutex);
        directorStopped = true;
        directorCondition.notify_all();
    }
    // Close both ports before joining either driver.  This is the shutdown
    // acknowledgement for synchronous callers: WaitForAck observes stopped
    // and returns immediately instead of enqueueing into an abandoned queue.
    minorDriverPort.Stop();
    majorDriverPort.Stop();
    StopGCThreads();
    StringDedup::Instance().Stop();
    statistics.Stop();
}

// Usually called from main thread, wait for collector thread to exit.
void CollectorResources::StopGCThreads()
{
    if (gcThreadRunning.load(std::memory_order_acquire) == false) {
        return;
    }
    int ret = ::pthread_join(directorThread, nullptr);
    CHECK_E(UNLIKELY(ret != 0), "::pthread_join(director) in StopGCThreads() return %d", ret);
    ret = ::pthread_join(minorDriverThread, nullptr);
    CHECK_E(UNLIKELY(ret != 0), "::pthread_join(minor) in StopGCThreads() return %d", ret);
    ret = ::pthread_join(majorDriverThread, nullptr);
    CHECK_E(UNLIKELY(ret != 0), "::pthread_join(major) in StopGCThreads() return %d", ret);
    // Drivers have joined; no new safepoint work can be submitted.
    delete runtimeWorkers;
    runtimeWorkers = nullptr;
    collectorProxy.GetGenerationCycle(GCCycleGeneration::YOUNG).StopWorkers();
    collectorProxy.GetGenerationCycle(GCCycleGeneration::OLD).StopWorkers();
    gcThreadRunning.store(false, std::memory_order_release);
}

void CollectorResources::RunDriverLoop(GCDriverKind kind)
{
    gcTid.store(MapleRuntime::GetTid(), std::memory_order_release);
    GCDriverPort& port = kind == GCDriverKind::MINOR ? minorDriverPort : majorDriverPort;
    const bool ownsControlQueue = kind == GCDriverKind::MAJOR;
    while (true) {
        GCDriverRequest request {};
        if (TakeDriverRequest(port, request)) {
            (void)ProcessDriverRequest(port, request);
            continue;
        }
        // Stop closes the port and wakes all waiters.  A driver exits once its
        // in-flight request has reached the acknowledgement point.
        if (port.IsStopped()) {
            break;
        }

        if (ownsControlQueue) {
            GCExecutor controlTask;
            if (taskQueue != nullptr && taskQueue->TryDequeue(controlTask)) {
#if defined(MRT_GC_UNIT_TESTS)
                void* owner = testCollector != nullptr ? static_cast<void*>(testCollector)
                                                       : static_cast<void*>(&collectorProxy);
#else
                void* owner = static_cast<void*>(&collectorProxy);
#endif
                // Heap-dump/control work shares the driver lifecycle lock.
                std::lock_guard<std::mutex> lock(driverLock);
                if (!controlTask.Execute(owner)) {
                    break;
                }
                continue;
            }

        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    NotifyGCFinished(GCTask::TASK_INDEX_FOR_EXIT);
}

bool CollectorResources::TakeDriverRequest(GCDriverPort& port, GCDriverRequest& request)
{
    std::lock_guard<std::mutex> lock(directorMutex);
    if (!port.TryDequeue(request)) {
        return false;
    }
    (port.Kind() == GCDriverKind::MINOR ? minorBusy : majorBusy) = true;
    return true;
}

void CollectorResources::CompleteDriverRequest(GCDriverPort& port)
{
    // zDriver.cpp:217-223: publish completion after ack; the director samples
    // again even when the next fixed tick is not due yet.
    std::lock_guard<std::mutex> lock(directorMutex);
    (port.Kind() == GCDriverKind::MINOR ? minorBusy : majorBusy) = false;
    directorReevaluate = true;
    directorCondition.notify_one();
}

void CollectorResources::RunCollection(Collector& collector, uint64_t index, GCReason reason, bool warmup)
{
    const bool isYoung = reason == GC_REASON_YOUNG;
    GCWorkers* workers = collector.GetGenerationCycle(isYoung
        ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD).Workers();
#if defined(MRT_GC_UNIT_TESTS)
    if (workers == nullptr) {
        collector.RunGarbageCollection(index, reason);
        return;
    }
#endif
    ZStatCycle& cycle = isYoung ? youngCycle : oldCycle;
    const auto before = workers->GetSnapshot();
    const uint64_t start = TimeUtil::NanoSeconds();
    cycle.AtStart(start, before.elapsedNanos, before.workerNanos);
    collector.RunGarbageCollection(index, reason);
    const auto after = workers->GetSnapshot();
    const uint64_t end = TimeUtil::NanoSeconds();
    cycle.AtEnd(end, after.elapsedNanos, after.workerNanos, warmup);
    (isYoung ? ZStatPhases::YoungGeneration : ZStatPhases::OldGeneration).RegisterEnd(end - start);
}

bool CollectorResources::ExecuteDriverRequest(const GCDriverRequest& request)
{
    CHECK(request.reason < GC_REASON_MAX);
    // ZDriverLocker: old collection temporarily releases this lock.
    std::lock_guard<std::mutex> lock(driverLock);
#if defined(MRT_GC_UNIT_TESTS)
    Collector* collector = testCollector != nullptr ? testCollector : static_cast<Collector*>(&collectorProxy);
#else
    Collector* collector = static_cast<Collector*>(&collectorProxy);
#endif
    GCDriverPort& port = request.reason == GC_REASON_YOUNG ? minorDriverPort : majorDriverPort;
    if (port.Abort().Poll()) {
        return false;
    }
    StringDedup::GCScope suspendDedup;
    const uint64_t collectionStart = TimeUtil::NanoSeconds();

    // Set the request's generation budgets before mark-start can consume
    // them, including the old mark domain prepared by the young prelude.
    const uint32_t youngCount = request.youngWorkers == 0 ? concurrentGcThreadCount : request.youngWorkers;
    const uint32_t oldCount = request.oldWorkers == 0 ? concurrentGcThreadCount : request.oldWorkers;
    const bool warmup = request.warmup;
    if (collector->GetGenerationCycle(GCCycleGeneration::YOUNG).Workers() != nullptr) {
        GetWorkers(GCCycleGeneration::YOUNG).SetActiveWorkers(youngCount);
        if (request.reason != GC_REASON_YOUNG) {
            GetWorkers(GCCycleGeneration::OLD).SetActiveWorkers(oldCount);
        }
    }

    // A major request owns its young prelude while holding the driver lock,
    // exactly like ZDriverMajor::collect_young followed by collect_old
    // (zDriver.cpp:416-452). Sending a synchronous request back through the
    // minor port would deadlock once both drivers share the ZGC lock.
    if (request.reason != GC_REASON_YOUNG) {
        youngPreludeRequest = &request;
        RunCollection(*collector, GCTask::ASYNC_TASK_INDEX, GC_REASON_YOUNG,
                      warmup);
        youngPreludeRequest = nullptr;
#if defined(MRT_GC_UNIT_TESTS)
        if (testAfterYoungPrelude) {
            testAfterYoungPrelude();
        }
#endif
        if (majorDriverPort.Abort().Poll()) {
            CancelDriverRequestLifecycle();
            return false;
        }
    }
    VLOG(GCPHASE, "[GCV2][driver] kind=%s seq=%llu reason=%u ack=pending",
         request.reason == GC_REASON_YOUNG ? "minor" : "major",
         static_cast<unsigned long long>(request.sequence), request.reason);
    RunCollection(*collector, request.asynchronous ? GCTask::ASYNC_TASK_INDEX : request.sequence,
                  request.reason, warmup);
    (request.reason == GC_REASON_YOUNG ? ZStatPhases::MinorCollection : ZStatPhases::MajorCollection)
        .RegisterEnd(TimeUtil::NanoSeconds() - collectionStart);
    // A stop during marking or relocation is cancellation, even though the
    // collection call has returned after joining its work and page cleanup.
    if (port.Abort().Poll()) {
        CancelDriverRequestLifecycle();
        return false;
    }
    NotifyGCFinished(request.asynchronous ? GCTask::ASYNC_TASK_INDEX : request.sequence);
    return true;
}

bool CollectorResources::ProcessDriverRequest(GCDriverPort& port, const GCDriverRequest& request)
{
    if (port.Abort().Poll() || !ExecuteDriverRequest(request)) {
        port.Cancel(request);
        CompleteDriverRequest(port);
        return false;
    }
    port.Acknowledge(request);
    CompleteDriverRequest(port);
    return true;
}

void CollectorResources::CancelDriverRequestLifecycle()
{
    std::unique_lock<std::mutex> lock(gcFinishedCondMutex);
    gcFinishedCondVar.notify_all();
}

// For the ignored gc request, check whether need to wait for current gc finish
void CollectorResources::PostIgnoredGcRequest(bool shouldWait)
{
    if (shouldWait && IsGcStarted()) {
        ScopedEnterSaferegion safeRegion(false);
        WaitForGCFinish();
    }
}

#if defined(MRT_TESTABLE_INTERNALS)
bool CollectorResources::ShouldWaitForIgnoredGcRequest(GCReason reason, bool async)
{
    return !async || g_gcRequests[reason].IsSyncGC();
}

bool CollectorResources::HasSyncTaskCompleted(uint64_t finishedIndex, uint64_t awaitedIndex)
{
    if (finishedIndex == GCTask::TASK_INDEX_FOR_EXIT) {
        return true;
    }
    MRT_ASSERT(finishedIndex >= GCTask::SYNC_TASK_MIN_INDEX && finishedIndex < GCTask::ASYNC_TASK_INDEX,
               "finished sync task index must not be a sentinel");
    MRT_ASSERT(awaitedIndex >= GCTask::SYNC_TASK_MIN_INDEX && awaitedIndex < GCTask::ASYNC_TASK_INDEX,
               "awaited sync task index must not be a sentinel");
    constexpr uint64_t ringSize = GCTask::ASYNC_TASK_INDEX - GCTask::SYNC_TASK_MIN_INDEX;
    constexpr uint64_t halfRing = ringSize / 2;
    uint64_t finishedOrdinal = finishedIndex - GCTask::SYNC_TASK_MIN_INDEX;
    uint64_t awaitedOrdinal = awaitedIndex - GCTask::SYNC_TASK_MIN_INDEX;
    uint64_t forwardDistance = finishedOrdinal >= awaitedOrdinal
        ? finishedOrdinal - awaitedOrdinal
        : ringSize - awaitedOrdinal + finishedOrdinal;
    return forwardDistance <= halfRing;
}
#endif

void CollectorResources::RequestAsyncGC(GCReason reason)
{
    CHECK(reason < GC_REASON_MAX);
    // Static synchronous reasons have no legal non-blocking completion
    // contract. Keep the pre-driver fail-closed boundary before selecting a
    // generation port; USER remains legal because its mode is per request.
    CHECK(!g_gcRequests[reason].IsSyncGC());
    GCDriverPort& port = reason == GC_REASON_YOUNG ? minorDriverPort : majorDriverPort;
    port.EnqueueAsync(reason);
}

void CollectorResources::RequestGCAndWait(GCReason reason)
{
    CHECK(reason < GC_REASON_MAX);
    // Enter saferegion since current thread may blocked by locks.
    ScopedEnterSaferegion enterSaferegion(false);
    GCDriverPort& port = reason == GC_REASON_YOUNG ? minorDriverPort : majorDriverPort;
    const GCDriverReceipt receipt = port.EnqueueSync(reason);
    (void)port.WaitForAck(receipt);
}

void CollectorResources::RequestGC(GCReason reason, bool async)
{
    CHECK(reason < GC_REASON_MAX);
    if (!IsGCActive()) {
        return;
    }

    GCRequest& request = g_gcRequests[reason];
    uint64_t curTime = TimeUtil::NanoSeconds();
    request.SetPrevRequestTime(curTime);
#if defined(MRT_GC_UNIT_TESTS)
    Collector& requestOwner = testCollector != nullptr ? *testCollector : static_cast<Collector&>(collectorProxy);
#else
    CollectorProxy& requestOwner = collectorProxy;
#endif
    if (requestOwner.ShouldIgnoreRequest(request)) {
        DLOG(ALLOC, "ignore gc request");
        PostIgnoredGcRequest(ShouldWaitForIgnoredGcRequest(reason, async));
    } else if (async) {
        RequestAsyncGC(reason);
    } else {
        RequestGCAndWait(reason);
    }
}

void CollectorResources::NotifyGCFinished(uint64_t gcIndex)
{
#if defined(MRT_GC_UNIT_TESTS)
    testCompletionCount.fetch_add(1, std::memory_order_relaxed);
#endif
    std::unique_lock<std::mutex> lock(gcFinishedCondMutex);
    if (gcIndex != GCTask::ASYNC_TASK_INDEX) { // sync gc, need set taskIndex
        finishedGcIndex.store(gcIndex, std::memory_order_release);
    }
    gcFinishedCondVar.notify_all();
    BroadcastGCCompletion();
}

void CollectorResources::WaitForGCFinish()
{
    uint64_t startTime = TimeUtil::MicroSeconds();
    std::unique_lock<std::mutex> lock(gcFinishedCondMutex);
    uint64_t curWaitGcIndex = finishedGcIndex.load();
    std::function<bool()> pred = [this, curWaitGcIndex] {
        return (!IsGcStarted() || (curWaitGcIndex != finishedGcIndex) ||
                (finishedGcIndex == GCTask::TASK_INDEX_FOR_EXIT));
    };
#ifdef __OHOS__
    std::chrono::seconds waitTime(2); // 2 seconds
    gcFinishedCondVar.wait_for(lock, waitTime, pred);
#else
    gcFinishedCondVar.wait(lock, pred);
#endif
    uint64_t stopTime = TimeUtil::MicroSeconds();
    uint64_t diffTime = stopTime - startTime;
    VLOG(REPORT, "WaitForGCFinish cost %zu us", diffTime);
}

void CollectorResources::StartGCThreads()
{
    bool expected = false;
    if (gcThreadRunning.compare_exchange_strong(expected, true, std::memory_order_acquire) == false) {
        return;
    }
    // Initialize the heap-owned runtime set and both generation sets.
    if (runtimeWorkers == nullptr) {
        unsigned int activeProcessorCount = std::thread::hardware_concurrency();
        bool affinityDetected = false;
#if defined(__linux__) || defined(hongmeng)
        cpu_set_t cpuSet;
        CPU_ZERO(&cpuSet);
        if (sched_getaffinity(0, sizeof(cpuSet), &cpuSet) == 0) {
            int affinityProcessorCount = CPU_COUNT(&cpuSet);
            if (affinityProcessorCount > 0) {
                activeProcessorCount = static_cast<unsigned int>(affinityProcessorCount);
                affinityDetected = true;
            }
        }
#endif
        activeProcessorCount = std::max(activeProcessorCount, 1U);
        // zHeuristics.cpp:77-107: CPU shares are rounded up, while the
        // relocation-buffer budget is capped at 2% of the maximum heap.
        // Dividing before multiplying avoids overflow at the size_t boundary.
        const size_t maxHeap = Heap::GetHeap().GetMaxCapacity();
        const auto& regions = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
        const size_t regionBytes = regions.GetThreadLocalRegionSize();
        CHECK_DETAIL(regionBytes != 0, "worker region budget must be initialized");
        const size_t heapWorkers = maxHeap / 50 / regionBytes;
        const uint64_t cpus = activeProcessorCount;
        gcThreadCount = static_cast<int32_t>(std::max<size_t>(1,
            std::min<size_t>((cpus * 3 + 4) / 5, heapWorkers)));
        concurrentGcThreadCount = static_cast<int32_t>(std::max<size_t>(1,
            std::min<size_t>((cpus + 3) / 4, heapWorkers)));
        VLOG(REPORT,
             "runtime worker count %d, concurrent gc thread count %d, "
             "active processor count %u, affinity detected %d, region bytes %zu",
             gcThreadCount, concurrentGcThreadCount, activeProcessorCount, affinityDetected,
             regionBytes);
        // zRuntimeWorkers.cpp:29-39: use the full parallel budget; the
        // coordinating driver is not one of the task participants.
        runtimeWorkers = new RuntimeWorkers(gcThreadCount);

        // zArguments.cpp:67-99, zWorkers.cpp:45-64: each generation uses
        // the concurrent budget as its maximum and initial active count.
        // GCWorkers counts participants, excluding the coordinating driver.
        collectorProxy.GetGenerationCycle(GCCycleGeneration::YOUNG).InitializeWorkers(concurrentGcThreadCount);
        collectorProxy.GetGenerationCycle(GCCycleGeneration::OLD).InitializeWorkers(concurrentGcThreadCount);

    }

    // ZGC shape: two independent drivers, each consuming only its generation
    // port. The major driver also owns the legacy non-GC control queue.
    if (::pthread_create(&minorDriverThread, nullptr, CollectorResources::MinorDriverThreadEntry, this) != 0) {
        MRT_ASSERT(0, "pthread_create minor driver failed!");
    }
    if (::pthread_create(&majorDriverThread, nullptr, CollectorResources::MajorDriverThreadEntry, this) != 0) {
        minorDriverPort.Stop();
        (void)::pthread_join(minorDriverThread, nullptr);
        MRT_ASSERT(0, "pthread_create major driver failed!");
    }
    // Keep the historical handle as an alias for diagnostics that name the
    // collector's main thread; shutdown joins both concrete driver handles.
    gcMainThread = majorDriverThread;
    if (::pthread_create(&directorThread, nullptr, CollectorResources::DirectorThreadEntry, this) != 0) {
        MRT_ASSERT(0, "pthread_create director failed!");
    }
    // set thread name.
#ifdef __WIN64
    int ret = pthread_setname_np(majorDriverThread, "gc-major-driver");
    CHECK_E(UNLIKELY(ret != 0), "pthread_setname_np() in CollectorResources::StartGCThreads() return %d rather than 0",
            ret);
#endif
}

int32_t CollectorResources::GetGCThreadCount(const bool isConcurrent) const
{
    if (runtimeWorkers == nullptr) {
        return 1;
    }
    return isConcurrent ? concurrentGcThreadCount : gcThreadCount;
}

void CollectorResources::BroadcastGCCompletion()
{
    gcWorking = 0;
#if defined(_WIN64) || defined(__APPLE__)
    WakeWhenGCDone();
#else
    (void)Futex(&gcWorking, FUTEX_WAKE_PRIVATE, INT_MAX);
#endif
}

void CollectorResources::RequestHeapDump(GCTask::TaskType gcTask)
{
    TaskQueue<GCExecutor>::TaskFilter filter = [](GCExecutor&, GCExecutor&) { return false; };
    GCExecutor dumpTask = GCExecutor(gcTask);
    taskQueue->EnqueueSync(dumpTask, filter);
}

} // namespace MapleRuntime

namespace MapleRuntime {
CollectorResources::CollectorResources(CollectorProxy& proxy) : collectorProxy(proxy) {}
}

namespace MapleRuntime {
GCWorkers& CollectorResources::GetWorkers(GCCycleGeneration generation) const
{
    return *collectorProxy.GetGenerationCycle(generation).Workers();
}

GCStats& CollectorResources::GetGCStats(GCCycleGeneration generation)
{
    return collectorProxy.GetGenerationCycle(generation).Stats();
}
}

namespace MapleRuntime {
bool CollectorResources::IsGcStarted() const
{
    return collectorProxy.GetCycleSnapshot(GCCycleGeneration::YOUNG).active ||
           collectorProxy.GetCycleSnapshot(GCCycleGeneration::OLD).active;
}
}

namespace MapleRuntime {
void CopyCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    ScopedEntryTrace trace("CJRT_GC_START");
    const uint64_t cycleSeq = GcLog::BeginCycle();

    const GCCycleGeneration generation = reason == GC_REASON_YOUNG
        ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD;
    GenerationCycle& cycle = GetGenerationCycle(generation);
    if (!cycle.Snapshot().active) {
        cycle.SelectReason(reason);
    }
    PreGarbageCollection(generation, reason != GC_REASON_YOUNG, gcIndex);
    ScheduleTraceEvent(TRACE_EV_GC_START, -1, nullptr, 0);
    VLOG(REPORT, "[GC] Start %s %s gcIndex= %lu", GetCollectorName(), g_gcRequests[reason].name, gcIndex);
    GCStats& gcStats = GetGCStats(generation);
    gcStats.collectedBytes = 0;
    gcStats.youngCandidateBytes = 0;
    gcStats.youngPromotedBytes = 0;
    gcStats.tenuringThreshold = 0;
    gcStats.gcStartTime = TimeUtil::NanoSeconds();

    // One GC cycle is the roots verification scene: it covers both the minor
    // and major root visitors, including concurrent stack enumeration.  Close
    // after the collector has joined all root work (zVerify.cpp:363-384).
    DoGarbageCollection(generation);

    GCDriverPort& port = reason == GC_REASON_YOUNG ? collectorResources.GetYoungDriverPort() :
                                                   collectorResources.GetMajorDriverPort();
    if (port.Abort().Poll()) {
        // The phase owner already joined any submitted work. Keep mark and
        // forwarding storage alive for driver shutdown; skip normal reclaim.
        GetWorkers(generation).SetInactive();
        cycle.End();
        GcLog::CompleteCycle(cycleSeq);
        return;
    }

    if (reason == GC_REASON_OOM) {
        Heap::GetHeap().GetAllocator().ReclaimGarbageMemory(true);
    }

    PostGarbageCollection(generation, gcIndex);
    gcStats.gcEndTime = TimeUtil::NanoSeconds();
    // Emitted here rather than from GCStats::Dump, because UpdateGCStats below (and so Dump) is
    // skipped for young collections: a minor would produce no cycle record and its phases would
    // be attributed to the next major.
    GcLog::CompleteCycle(cycleSeq);
    GcLog::Cycle(cycleSeq, reason == GC_REASON_YOUNG ? "minor" : "major",
                 g_gcRequests[reason].name, gcStats.gcStartTime, gcStats.gcEndTime - gcStats.gcStartTime,
                 gcStats.liveBytesBeforeGC, gcStats.liveBytesAfterGC, gcStats.collectedBytes,
                 Heap::GetHeap().GetUsedPageSize(), gcStats.GetThreshold());
    if (reason != GC_REASON_YOUNG) {
        UpdateGCStats();
    }
    uint64_t gcTimeNs = gcStats.gcEndTime - gcStats.gcStartTime;
    ScheduleTraceEvent(TRACE_EV_GC_DONE, -1, nullptr, 0);
    double rate = (static_cast<double>(gcStats.collectedBytes) / gcTimeNs) * (static_cast<double>(NS_PER_S) / MB);
    VLOG(REPORT, "total gc time: %s us, collection rate %.3lf MB/s\n", Pretty(gcTimeNs / NS_PER_US).Str(), rate);
    g_gcTotalTimeUs.fetch_add(gcTimeNs / NS_PER_US, std::memory_order_release);
    g_gcCollectedTotalBytes.fetch_add(gcStats.collectedBytes, std::memory_order_release);
    gcStats.collectionRate = rate;
    // zStatHeap::at_relocate_end: publish only to the generation being collected.
    const bool young = reason == GC_REASON_YOUNG;
    const size_t usedAfter = Heap::GetHeap().GetAllocatedSize();
    // A12a scope ruling: preserve the old-generation baseline scalar until
    // A07's mark-end livemap aggregation replaces it. Do not infer live bytes
    // from candidate minus reclaimed capacity. Young has an actual mark result.
    const size_t liveBytes = young ? gcStats.youngPromotedBytes : usedAfter;
    (young ? ZStat::YoungHeap() : ZStat::OldHeap()).AtRelocateEnd(
        usedAfter, liveBytes, gcStats.collectedBytes);
    cycle.End();
}
}
