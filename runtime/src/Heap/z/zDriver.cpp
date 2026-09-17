// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zBreakpoint.hpp"

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
#include "Heap/z/zGlobals.hpp"
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

// zDriver.cpp:118-127,319-328: each driver names itself and starts in its
// constructor; run_thread is the request loop (zDriver.cpp:201-225,463-488)
// and terminate closes the port so the loop's receive returns (:227-231).
ZDriver::ZDriver(CollectorResources& resources, GCDriverKind kind) : resources(resources), kind(kind)
{
    set_name(kind == GCDriverKind::MINOR ? "ZDriverMinor" : "ZDriverMajor");
    create_and_start();
}

void ZDriver::run_thread()
{
    resources.RunDriverLoop(kind);
}

void ZDriver::terminate()
{
    (kind == GCDriverKind::MINOR ? resources.minorDriverPort : resources.majorDriverPort).Stop();
}

void CollectorResources::Init()
{
    ZAbort::reset();
    minorDriverPort.Reset();
    majorDriverPort.Reset();
    ZStat::Initialize();
    GetGCStats(ZGenerationId::young).Init();
    GetGCStats(ZGenerationId::old).Init();
    ZStatMutatorAllocRate::initialize();
    const uint64_t now = TimeUtil::NanoSeconds();
    collectorProxy.GetZGeneration(ZGenerationId::young).CycleStats().Initialize(now);
    collectorProxy.GetZGeneration(ZGenerationId::old).CycleStats().Initialize(now);
    // zHeap.cpp: ZHeap owns _stat; its constructor starts the thread.
    statistics = new ZStat();
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
    minorDriverPort.Stop();
    majorDriverPort.Stop();
}

// zCollectedHeap.cpp:96-110 ZCollectedHeap::stop. Each ZThread::terminate
// closes its own wait (director monitor, driver port); a driver's port stop
// is also the shutdown acknowledgement for synchronous callers.
void CollectorResources::StopGCWork()
{
    finalizerProcessor.Stop();
    // zCollectedHeap.cpp:314-319 gc_threads_do order: director, major driver,
    // minor driver, stat. StringDedup is not a ZGC thread and stops last.
    StopGCThreads();
    if (statistics != nullptr) {
        statistics->stop();
        delete statistics;
        statistics = nullptr;
    }
    StringDedup::Instance().Stop();
}

// zCollectedHeap.cpp:96-110 ZCollectedHeap::stop: every ConcurrentGCThread
// is stopped through ConcurrentGCThread::stop (should_terminate ->
// stop_service -> ZThread::terminate -> wait for termination).
void CollectorResources::StopGCThreads()
{
    if (gcThreadRunning.load(std::memory_order_acquire) == false) {
        return;
    }
    // zCollectedHeap.cpp:106 ZAbort::abort(): cancel in-flight collections
    // before any GC thread is asked to terminate.
    ZAbort::abort();
    ZAbort::abort();
    for (ZThread* thread : { static_cast<ZThread*>(director), static_cast<ZThread*>(majorDriver),
                             static_cast<ZThread*>(minorDriver) }) {
        thread->stop();
    }
    delete director;
    delete minorDriver;
    delete majorDriver;
    director = nullptr;
    minorDriver = nullptr;
    majorDriver = nullptr;
    // Drivers have terminated; no worker task can be submitted any more.
    collectorProxy.GetZGeneration(ZGenerationId::young).StopWorkers();
    collectorProxy.GetZGeneration(ZGenerationId::old).StopWorkers();
    gcThreadRunning.store(false, std::memory_order_release);
}

void CollectorResources::RunDriverLoop(GCDriverKind kind)
{
    GCDriverPort& port = kind == GCDriverKind::MINOR ? minorDriverPort : majorDriverPort;
    GCDriverRequest request {};
    while (TakeDriverRequest(port, request)) {
        (void)ProcessDriverRequest(port, request);
    }
}

bool CollectorResources::TakeDriverRequest(GCDriverPort& port, GCDriverRequest& request)
{
    if (!port.Receive(request)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(directorMutex);
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
    ZGeneration& generation = collector.GetZGeneration(isYoung
        ? ZGenerationId::young : ZGenerationId::old);
    ZStatCycle& cycle = generation.CycleStats();
    const uint64_t start = TimeUtil::NanoSeconds();
    const ZYoungType type = collector.GetZGeneration(ZGenerationId::young).YoungType();
    // zGeneration.cpp:381,388: at_start/at_end(stat_workers, should_record_stats)
    // bracket the collection; the parallel share is read from ZStatWorkers.
    const bool recordStats = !isYoung || type == ZYoungType::minor || type == ZYoungType::major_partial_roots;
    cycle.AtStart(start);
    collector.RunGarbageCollection(index, reason);
    const uint64_t end = TimeUtil::NanoSeconds();
    cycle.AtEnd(end, generation.StatWorkers(), warmup, recordStats);
    (isYoung ? ZStatPhases::YoungGeneration : ZStatPhases::OldGeneration).RegisterEnd(end - start);
}

bool CollectorResources::ExecuteDriverRequest(const GCDriverRequest& request)
{
    CHECK(request.reason < GC_REASON_MAX);
#if defined(MRT_GC_UNIT_TESTS)
    Collector* collector = testCollector != nullptr ? testCollector : static_cast<Collector*>(&collectorProxy);
#else
    Collector* collector = static_cast<Collector*>(&collectorProxy);
#endif
    GCDriverPort& port = request.reason == GC_REASON_YOUNG ? minorDriverPort : majorDriverPort;
    if (ZAbort::should_abort()) {
        return false;
    }
    GCIdMark gcId;
    const uint64_t collectionStart = TimeUtil::NanoSeconds();
    size_t liveBefore = 0;
    size_t liveAfter = 0;
    size_t collected = 0;
    size_t threshold = 0;
    bool firstGeneration = true;
    // ZServiceabilityCycleTracer spans the request, including all young
    // prelude phases of a major. Capture existing generation stats before reuse.
    const auto accumulate = [&](ZGenerationId generation) {
        GCStats& stats = collector->GetGCStats(generation);
        if (firstGeneration) liveBefore = stats.liveBytesBeforeGC;
        firstGeneration = false;
        liveAfter = stats.liveBytesAfterGC;
        collected += stats.collectedBytes;
        threshold = stats.GetThreshold();
    };

    // Set the request's generation budgets before mark-start can consume
    // them, including the old mark domain prepared by the young prelude.
    const uint32_t youngCount = request.youngWorkers == 0 ? concurrentGcThreadCount : request.youngWorkers;
    const uint32_t oldCount = request.oldWorkers == 0 ? concurrentGcThreadCount : request.oldWorkers;
    const bool warmup = request.warmup;
    // zDriver.cpp:166-176 / zGeneration.cpp:154: the request carries the
    // selected worker counts into each generation's ZWorkers.
    collector->GetZGeneration(ZGenerationId::young).Workers()->set_active_workers(youngCount);
    if (request.reason != GC_REASON_YOUNG) {
        collector->GetZGeneration(ZGenerationId::old).Workers()->set_active_workers(oldCount);
    }

    // zDriver.cpp:416-436: full causes preclean with promote-all, then
    // establish the combined young/old roots cycle. Other causes use partial roots.
    if (request.reason != GC_REASON_YOUNG) {
        ZGCIdMajor majorId(GCIdMark::Current(), 'Y');
        collector->GetZGeneration(ZGenerationId::old).SelectReason(
            request.reason, request.asynchronous ? GCTask::ASYNC_TASK_INDEX : request.sequence);
        const bool preclean = ShouldPrecleanYoung(request.reason);
        if (preclean) {
            RunYoungCollection(*collector, GCTask::ASYNC_TASK_INDEX, ZYoungType::major_full_preclean, warmup);
            accumulate(ZGenerationId::young);
            if (ZAbort::should_abort()) {
                CancelDriverRequestLifecycle(port.Kind());
                return false;
            }
        }
        RunYoungCollection(*collector, GCTask::ASYNC_TASK_INDEX,
                           preclean ? ZYoungType::major_full_roots : ZYoungType::major_partial_roots, warmup);
        accumulate(ZGenerationId::young);
#if defined(MRT_GC_UNIT_TESTS)
        if (testAfterYoungPrelude) {
            testAfterYoungPrelude();
        }
#endif
        if (ZAbort::should_abort()) {
            CancelDriverRequestLifecycle(port.Kind());
            return false;
        }
    }
    VLOG(GCPHASE, "[GCV2][driver] kind=%s seq=%llu reason=%u ack=pending",
         request.reason == GC_REASON_YOUNG ? "minor" : "major",
         static_cast<unsigned long long>(request.sequence), request.reason);
    const uint64_t index = request.asynchronous ? GCTask::ASYNC_TASK_INDEX : request.sequence;
    if (request.reason == GC_REASON_YOUNG) {
        ZGCIdMinor minorId(GCIdMark::Current());
        RunYoungCollection(*collector, index, ZYoungType::minor, warmup);
        accumulate(ZGenerationId::young);
    } else {
        ZGCIdMajor majorId(GCIdMark::Current(), 'O');
        RunCollection(*collector, index, request.reason, warmup);
        accumulate(ZGenerationId::old);
    }
    (request.reason == GC_REASON_YOUNG ? ZStatPhases::MinorCollection : ZStatPhases::MajorCollection)
        .RegisterEnd(TimeUtil::NanoSeconds() - collectionStart);
    // A stop during marking or relocation is cancellation, even though the
    // collection call has returned after joining its work and page cleanup.
    if (ZAbort::should_abort()) {
        CancelDriverRequestLifecycle(port.Kind());
        return false;
    }
    GcLog::Cycle(GCIdMark::Current(), request.reason == GC_REASON_YOUNG ? "minor" : "major",
                 g_gcRequests[request.reason].name, collectionStart, TimeUtil::NanoSeconds() - collectionStart,
                 liveBefore, liveAfter, collected, Heap::GetHeap().GetUsedPageSize(), threshold);
    return true;
}

bool CollectorResources::ProcessDriverRequest(GCDriverPort& port, const GCDriverRequest& request)
{
    DriverLocker locker(*this);
    const bool major = port.Kind() == GCDriverKind::MAJOR;
    if (major) ZBreakpoint::AtBeforeGC();
    if (port.IsStopped() || ZAbort::should_abort() || !ExecuteDriverRequest(request)) {
        port.Cancel(request);
        CompleteDriverRequest(port);
        return false;
    }
    port.Acknowledge(request);
#if defined(MRT_GC_UNIT_TESTS)
    testCompletionCount.fetch_add(1, std::memory_order_relaxed);
#endif
    if (major) ZBreakpoint::AtAfterGC();
    CompleteDriverRequest(port);
    return true;
}

void CollectorResources::CancelDriverRequestLifecycle(GCDriverKind kind)
{
    collectorProxy.GetZGeneration(kind == GCDriverKind::MINOR
        ? ZGenerationId::young : ZGenerationId::old).End();
}

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

    if (reason == GC_REASON_WB_BREAKPOINT) {
        ZBreakpoint::StartGC();
        majorDriverPort.EnqueueAsync(reason);
        return;
    }

    // zDriver.cpp:141-160/337-371: every accepted request goes to its port.
    if (async) {
        RequestAsyncGC(reason);
    } else {
        RequestGCAndWait(reason);
    }
}

void CollectorResources::StartGCThreads()
{
    bool expected = false;
    if (gcThreadRunning.compare_exchange_strong(expected, true, std::memory_order_acquire) == false) {
        return;
    }
    // Initialize both generation worker sets.
    if (collectorProxy.GetZGeneration(ZGenerationId::young).Workers() == nullptr) {
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
        concurrentGcThreadCount = static_cast<int32_t>(std::max<size_t>(1,
            std::min<size_t>((cpus + 3) / 4, heapWorkers)));
        // zArguments.cpp:67-81: ConcGCThreads is the per-generation maximum and
        // sizes every ZPerWorker (zValue.inline.hpp:108-110); set before workers.
        ConcGCThreads = static_cast<uint32_t>(concurrentGcThreadCount);
        VLOG(REPORT,
             "concurrent gc thread count %d, active processor count %u, affinity detected %d, region bytes %zu",
             concurrentGcThreadCount, activeProcessorCount, affinityDetected, regionBytes);

        // zArguments.cpp:67-99, zWorkers.cpp:45-64: each generation uses
        // the concurrent budget as its maximum and initial active count.
        // ZWorkers counts participants, excluding the coordinating driver.
        collectorProxy.GetZGeneration(ZGenerationId::young).InitializeWorkers(concurrentGcThreadCount);
        collectorProxy.GetZGeneration(ZGenerationId::old).InitializeWorkers(concurrentGcThreadCount);
        finalizerProcessor.GetReferenceProcessor().set_workers(
            collectorProxy.GetZGeneration(ZGenerationId::old).Workers());
    }

    // zHeap.cpp / zCollectedHeap.cpp:65-71: the two drivers and the director
    // are ZThreads that start in their constructors.
    minorDriver = new ZDriver(*this, GCDriverKind::MINOR);
    majorDriver = new ZDriver(*this, GCDriverKind::MAJOR);
    director = new ZDirector(*this);
}


} // namespace MapleRuntime

namespace MapleRuntime {
CollectorResources::CollectorResources(CollectorProxy& proxy) : collectorProxy(proxy) {}
}

namespace MapleRuntime {
ZWorkers& CollectorResources::GetWorkers(ZGenerationId generation) const
{
    return *collectorProxy.GetZGeneration(generation).Workers();
}

GCStats& CollectorResources::GetGCStats(ZGenerationId generation)
{
    return collectorProxy.GetZGeneration(generation).Stats();
}
}

namespace MapleRuntime {
bool CollectorResources::IsGcStarted() const
{
    return collectorProxy.GetCycleSnapshot(ZGenerationId::young).active ||
           collectorProxy.GetCycleSnapshot(ZGenerationId::old).active;
}
}

namespace MapleRuntime {
void CopyCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    ScopedEntryTrace trace("CJRT_GC_START");

    const ZGenerationId generation = reason == GC_REASON_YOUNG
        ? ZGenerationId::young : ZGenerationId::old;
    ZGeneration& cycle = GetZGeneration(generation);
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
    if (ZAbort::should_abort()) {
        // The phase owner already joined any submitted work. Keep mark and
        // forwarding storage alive for driver shutdown; skip normal reclaim.
        GetWorkers(generation).set_inactive();
        cycle.End();
        return;
    }

    if (reason == GC_REASON_OOM) {
        Heap::GetHeap().GetAllocator().ReclaimGarbageMemory(true);
    }

    PostGarbageCollection(generation, gcIndex);
    gcStats.gcEndTime = TimeUtil::NanoSeconds();
    const char* phaseName = "major.old";
    if (generation == ZGenerationId::young) {
        switch (cycle.YoungType()) {
            case ZYoungType::none: phaseName = "young"; break;
            case ZYoungType::minor: phaseName = "minor.young"; break;
            case ZYoungType::major_full_preclean: phaseName = "major.preclean"; break;
            case ZYoungType::major_full_roots: phaseName = "major.full_roots"; break;
            case ZYoungType::major_partial_roots: phaseName = "major.partial_roots"; break;
        }
    }
    // A generation span includes pauses and concurrent work. Its start and
    // duration distinguish multiple Y spans without inventing another GC ID.
    GcLog::Phase(GCIdMark::Current(), phaseName, "unknown", gcStats.gcStartTime,
                 gcStats.gcEndTime - gcStats.gcStartTime);
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
    if (reason != GC_REASON_YOUNG) {
        gcStats.RecordMajorGCFinish(TimeUtil::NanoSeconds(), gcTimeNs, Heap::GetHeap().GetAllocatedSize(),
                                    gcStats.collectedBytes);
    }
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

namespace MapleRuntime {
void CollectorResources::RunYoungCollection(Collector& collector, uint64_t index, ZYoungType type, bool warmup)
{
    YoungTypeSetter typeSetter(collector.GetZGeneration(ZGenerationId::young), type);
    RunCollection(collector, index, GC_REASON_YOUNG, warmup);
}

bool CollectorResources::ShouldPrecleanYoung(GCReason reason) const
{
    // ZGC zDriver.cpp:270-299: explicit full collections, including breakpoints.
    switch (reason) {
        case GC_REASON_USER:
        case GC_REASON_OOM:
        case GC_REASON_FORCE:
        case GC_REASON_WB_BREAKPOINT:
            return true;
        case GC_REASON_BACKUP:
        case GC_REASON_HEU:
        case GC_REASON_HEU_SYNC:
        case GC_REASON_NATIVE:
        case GC_REASON_NATIVE_SYNC:
            break;
        default:
            CHECK(false);
    }
    // All requests in this existing allocation FIFO wait for a major
    // (zPageAllocator.cpp:StallAllocation requests GC_REASON_OOM).
    const auto& manager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    return manager.IsAllocationStalling();
}

GCDriverPort& CollectorResources::GetYoungDriverPort()
{
    return collectorProxy.GetZGeneration(ZGenerationId::young).YoungType() == ZYoungType::minor
        ? minorDriverPort : majorDriverPort;
}
}
