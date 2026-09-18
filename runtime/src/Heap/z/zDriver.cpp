// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zStringDedup.hpp"
#include "Heap/z/zCollectedHeap.hpp"
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
#include "Heap/z/zMark.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Common/Runtime.h"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Common/RunType.h"
#include "Common/ScopedObjectAccess.h"
#include "LoaderManager.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
std::mutex ZDriver::driverLock;

void ZDriver::lock() { driverLock.lock(); }

void ZDriver::unlock() { driverLock.unlock(); }

extern "C" uintptr_t MRT_StopGCWork()
{
    Heap::GetHeap().StopGCWork();
    return 0;
}

// zDriver.cpp:118-127,319-328: each driver names itself and starts in its
// constructor; run_thread is the request loop (zDriver.cpp:201-225,463-488)
// and terminate closes the port so the loop's receive returns (:227-231).
ZDriver::ZDriver(CollectorResources& resources, GCDriverKind kind, ZDriverPort& port)
    : resources(resources), kind(kind), port(port)
{
    set_name(kind == GCDriverKind::MINOR ? "ZDriverMinor" : "ZDriverMajor");
}

void ZDriver::run_thread()
{
    for (;;) {
        const ZDriverRequest request = port.receive();
        if (request.cause() == GC_REASON_INVALID) {
            return;
        }
        ZCollectedHeap::heap()->director()->set_busy(kind == GCDriverKind::MINOR, true);
        {
            DriverLocker locker;
            const bool major = kind == GCDriverKind::MAJOR;
            ZAbort::reset();
            if (major) ZBreakpoint::AtBeforeGC();
            abortpoint();
            const bool completed = !ZAbort::should_abort() && resources.ExecuteDriverRequest(kind, request);
            port.ack();
#if defined(MRT_GC_UNIT_TESTS)
            if (completed) resources.testCompletionCount.fetch_add(1, std::memory_order_relaxed);
#endif
            if (major) ZBreakpoint::AtAfterGC();
            ZCollectedHeap::heap()->director()->set_busy(!major, false);
            if (completed && !major) ZDirector::evaluate_rules();
        }
        abortpoint();
        auto& regions = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
        regions.SatisfyStalledAllocations();
    }
}

void ZDriver::terminate()
{
    port.send_async(ZDriverRequest(GC_REASON_INVALID, 0, 0));
}

bool ZDriver::is_busy() const
{
    return port.is_busy();
}

void ZDriverMinor::collect(const ZDriverRequest& request)
{
    switch (request.cause()) {
        case GC_REASON_YOUNG:
            _port.send_async(request);
            break;
        case GC_REASON_HEU_SYNC:
        case GC_REASON_NATIVE_SYNC:
            _port.send_sync(request);
            break;
        default:
            CHECK(false);
            break;
    }
}

void ZDriverMajor::collect(const ZDriverRequest& request)
{
    switch (request.cause()) {
        case GC_REASON_USER:
        case GC_REASON_FORCE:
        case GC_REASON_OOM:
            _port.send_sync(request);
            break;
        case GC_REASON_BACKUP:
        case GC_REASON_HEU:
        case GC_REASON_NATIVE:
        case GC_REASON_WARMUP:
            _port.send_async(request);
            break;
        case GC_REASON_WB_BREAKPOINT:
            ZBreakpoint::StartGC();
            _port.send_async(request);
            break;
        default:
            CHECK(false);
            break;
    }
}

void CollectorResources::Init()
{
    ZAbort::reset();
    ZStat::Initialize();
    GetGCStats(ZGenerationId::young).Init();
    GetGCStats(ZGenerationId::old).Init();
    ZStatMutatorAllocRate::initialize();
    const uint64_t now = TimeUtil::NanoSeconds();
    Heap::GetHeap().young().CycleStats().Initialize(now);
    Heap::GetHeap().old().CycleStats().Initialize(now);
    ZCollectedHeap::heap()->_stat = new ZStat();
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
}

// zCollectedHeap.cpp:96-110 ZCollectedHeap::stop. Each ZThread::terminate
// closes its own wait (director monitor, driver port); a driver's port stop
// is also the shutdown acknowledgement for synchronous callers.
void CollectorResources::StopGCWork()
{
    if (finalizerProcessor.IsRunning()) {
        finalizerProcessor.Stop();
    }
    // zCollectedHeap.cpp:314-319 gc_threads_do order: director, major driver,
    // minor driver, stat. StringDedup is not a ZGC thread and stops last.
    StopGCThreads();
    ZCollectedHeap* collected = ZCollectedHeap::heap();
    if (collected->_stat != nullptr) {
        collected->_stat->stop();
        delete collected->_stat;
        collected->_stat = nullptr;
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
    ZCollectedHeap* collected = ZCollectedHeap::heap();
    for (ZThread* thread : { static_cast<ZThread*>(collected->_director), static_cast<ZThread*>(collected->_driver_major),
                             static_cast<ZThread*>(collected->_driver_minor) }) {
        thread->stop();
    }
    delete collected->_director;
    delete collected->_driver_minor;
    delete collected->_driver_major;
    collected->_director = nullptr;
    collected->_driver_minor = nullptr;
    collected->_driver_major = nullptr;
    // Drivers have terminated; no worker task can be submitted any more.
    Heap::GetHeap().young().StopWorkers();
    Heap::GetHeap().old().StopWorkers();
    gcThreadRunning.store(false, std::memory_order_release);
}

void CollectorResources::RunCollection(HeapGcState& collector, uint64_t index, GCReason reason, bool warmup)
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

bool CollectorResources::ExecuteDriverRequest(GCDriverKind kind, const ZDriverRequest& request)
{
    CHECK(request.cause() < GC_REASON_MAX);
    HeapGcState* activeCollector = &Heap::GetHeap().GetCollector();
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
        GCStats& stats = activeCollector->GetGCStats(generation);
        if (firstGeneration) liveBefore = stats.liveBytesBeforeGC;
        firstGeneration = false;
        liveAfter = stats.liveBytesAfterGC;
        collected += stats.collectedBytes;
        threshold = stats.GetThreshold();
    };

    // Set the request's generation budgets before mark-start can consume
    // them, including the old mark domain prepared by the young prelude.
    const uint32_t youngCount = request.young_nworkers() == 0 ? concurrentGcThreadCount : request.young_nworkers();
    const uint32_t oldCount = request.old_nworkers() == 0 ? concurrentGcThreadCount : request.old_nworkers();
    const bool warmup = request.cause() == GC_REASON_WARMUP;
    // zDriver.cpp:166-176 / zGeneration.cpp:154: the request carries the
    // selected worker counts into each generation's ZWorkers.
    activeCollector->GetZGeneration(ZGenerationId::young).Workers()->set_active_workers(youngCount);
    if (request.cause() != GC_REASON_YOUNG) {
        activeCollector->GetZGeneration(ZGenerationId::old).Workers()->set_active_workers(oldCount);
    }

    // zDriver.cpp:416-436: full causes preclean with promote-all, then
    // establish the combined young/old roots cycle. Other causes use partial roots.
    if (request.cause() != GC_REASON_YOUNG) {
        ZGCIdMajor majorId(GCIdMark::Current(), 'Y');
        activeCollector->GetZGeneration(ZGenerationId::old).SelectReason(
            request.cause(), GCTask::ASYNC_TASK_INDEX);
        const bool preclean = ShouldPrecleanYoung(request.cause());
        if (preclean) {
            RunYoungCollection(*activeCollector, GCTask::ASYNC_TASK_INDEX, ZYoungType::major_full_preclean, warmup);
            accumulate(ZGenerationId::young);
            if (ZAbort::should_abort()) {
                CancelDriverRequestLifecycle(kind);
                return false;
            }
        }
        RunYoungCollection(*activeCollector, GCTask::ASYNC_TASK_INDEX,
                           preclean ? ZYoungType::major_full_roots : ZYoungType::major_partial_roots, warmup);
        accumulate(ZGenerationId::young);
#if defined(MRT_GC_UNIT_TESTS)
        if (testAfterYoungPrelude) {
            testAfterYoungPrelude();
        }
#endif
        if (ZAbort::should_abort()) {
            CancelDriverRequestLifecycle(kind);
            return false;
        }
    }
    VLOG(GCPHASE, "[GCV2][driver] kind=%s seq=%llu reason=%u ack=pending",
         request.cause() == GC_REASON_YOUNG ? "minor" : "major",
         0ull, request.cause());
    const uint64_t index = GCTask::ASYNC_TASK_INDEX;
    if (request.cause() == GC_REASON_YOUNG) {
        ZGCIdMinor minorId(GCIdMark::Current());
        RunYoungCollection(*activeCollector, index, ZYoungType::minor, warmup);
        accumulate(ZGenerationId::young);
    } else {
        ZGCIdMajor majorId(GCIdMark::Current(), 'O');
        RunCollection(*activeCollector, index, request.cause(), warmup);
        accumulate(ZGenerationId::old);
    }
    (request.cause() == GC_REASON_YOUNG ? ZStatPhases::MinorCollection : ZStatPhases::MajorCollection)
        .RegisterEnd(TimeUtil::NanoSeconds() - collectionStart);
    // A stop during marking or relocation is cancellation, even though the
    // collection call has returned after joining its work and page cleanup.
    if (ZAbort::should_abort()) {
        CancelDriverRequestLifecycle(kind);
        return false;
    }
    GcLog::Cycle(GCIdMark::Current(), request.cause() == GC_REASON_YOUNG ? "minor" : "major",
                 g_gcRequests[request.cause()].name, collectionStart, TimeUtil::NanoSeconds() - collectionStart,
                 liveBefore, liveAfter, collected, Heap::GetHeap().GetUsedPageSize(), threshold);
    return true;
}

void CollectorResources::CancelDriverRequestLifecycle(GCDriverKind kind)
{
    Heap::GetHeap().GetCollector().GetZGeneration(kind == GCDriverKind::MINOR
        ? ZGenerationId::young : ZGenerationId::old).End();
}

void CollectorResources::StartGCThreads()
{
    bool expected = false;
    if (gcThreadRunning.compare_exchange_strong(expected, true, std::memory_order_acquire) == false) {
        return;
    }
    // Initialize both generation worker sets.
    if (Heap::GetHeap().young().Workers() == nullptr) {
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
        ZYoungGCThreads = ConcGCThreads;
        ZOldGCThreads = ConcGCThreads;
        VLOG(REPORT,
             "concurrent gc thread count %d, active processor count %u, affinity detected %d, region bytes %zu",
             concurrentGcThreadCount, activeProcessorCount, affinityDetected, regionBytes);

        // zArguments.cpp:67-99, zWorkers.cpp:45-64: each generation uses
        // the concurrent budget as its maximum and initial active count.
        // ZWorkers counts participants, excluding the coordinating driver.
        Heap::GetHeap().young().InitializeWorkers(concurrentGcThreadCount);
        Heap::GetHeap().old().InitializeWorkers(concurrentGcThreadCount);
        finalizerProcessor.GetReferenceProcessor().set_workers(
            Heap::GetHeap().old().Workers());
    }

    // zCollectedHeap.cpp:62-70: drivers and director start in ZCollectedHeap().
    // Cangjie Heap lives in ImmortalWrapper constructed at load; threads start
    // here after Heap::Init so capacity/workers exist (ZGC constructs later).
    ZCollectedHeap* collected = ZCollectedHeap::heap();
    collected->_driver_minor = new ZDriverMinor(*this);
    collected->_driver_major = new ZDriverMajor(*this);
    collected->_director = new ZDirector();
    collected->_driver_minor->start();
    collected->_driver_major->start();
}


} // namespace MapleRuntime

namespace MapleRuntime {
ZDriverPort& CollectorResources::GetMinorDriverPort()
{
    return ZCollectedHeap::heap()->driver_minor()->port();
}

ZDriverPort& CollectorResources::GetMajorDriverPort()
{
    return ZCollectedHeap::heap()->driver_major()->port();
}

ZWorkers& CollectorResources::GetWorkers(ZGenerationId generation) const
{
    return *Heap::GetHeap().GetZGeneration(generation).Workers();
}

GCStats& CollectorResources::GetGCStats(ZGenerationId generation)
{
    return Heap::GetHeap().GetZGeneration(generation).Stats();
}
}

namespace MapleRuntime {
bool CollectorResources::IsGcStarted() const
{
    return Heap::GetHeap().GetCycleSnapshot(ZGenerationId::young).active ||
           Heap::GetHeap().GetCycleSnapshot(ZGenerationId::old).active;
}
}

namespace MapleRuntime {
void HeapGcState::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
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
    VLOG(REPORT, "[GC] Start ZGC %s gcIndex= %lu", g_gcRequests[reason].name, gcIndex);
    GCStats& gcStats = GetGCStats(generation);
    gcStats.collectedBytes = 0;
    gcStats.youngCandidateBytes = 0;
    gcStats.youngPromotedBytes = 0;
    gcStats.tenuringThreshold = 0;
    gcStats.gcStartTime = TimeUtil::NanoSeconds();

    // One GC cycle is the roots verification scene: it covers both the minor
    // and major root visitors, including concurrent stack enumeration.  Close
    // after the collector has joined all root work (zVerify.cpp:363-384).
    if (generation == ZGenerationId::young) {
        ZGeneration::young()->collect();
    } else {
        ZGeneration::old()->collect();
    }

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
void CollectorResources::RunYoungCollection(HeapGcState& collector, uint64_t index, ZYoungType type, bool warmup)
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
        case GC_REASON_WARMUP:
            break;
        default:
            CHECK(false);
    }
    // All requests in this existing allocation FIFO wait for a major
    // (zPageAllocator.cpp:StallAllocation requests GC_REASON_OOM).
    const auto& manager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    return manager.IsAllocationStalling();
}


#ifdef COV_SIGNALHANDLE
extern "C" void __gcov_dump(void);
#endif

bool GCExecutor::Execute(void* owner)
{
    MRT_ASSERT(owner != nullptr, "task queue owner ptr should not be null!");
    HeapGcState* collector = reinterpret_cast<HeapGcState*>(owner);

    switch (taskType) {
        case GCTask::TaskType::TASK_TYPE_TERMINATE_GC: {
            return false;
        }
        case GCTask::TaskType::TASK_TYPE_TIMEOUT_GC: {
            uint64_t curTime = TimeUtil::NanoSeconds();
            if ((curTime - GCStats::GetPrevGCStartTime()) > CangjieRuntime::GetGCParam().backupGCInterval) {
                GCStats::SetPrevGCStartTime(curTime);
                collector->RunGarbageCollection(GCTask::ASYNC_TASK_INDEX, GC_REASON_BACKUP);
            }
            break;
        }
        case GCTask::TaskType::TASK_TYPE_INVOKE_GC: {
            GCStats::SetPrevGCStartTime(TimeUtil::NanoSeconds());
            collector->RunGarbageCollection(taskIndex, gcReason);
            break;
        }
        case GCTask::TaskType::TASK_TYPE_DUMP_HEAP: {
            CjHeapData* cjHeapData = new CjHeapData();
            if (cjHeapData != nullptr) {
                cjHeapData->DumpHeap();
                delete cjHeapData;
            } else {
                LOG(RTLOG_ERROR, "cjHeapData Init Failed");
            }
#ifdef COV_SIGNALHANDLE
            __gcov_dump();
#endif
            break;
        }
        case GCTask::TaskType::TASK_TYPE_DUMP_HEAP_IDE: {
#if defined(__OHOS__) && (__OHOS__ == 1)
            CjHeapDataForIDE* heapSnapshotJSONSerializer = new CjHeapDataForIDE();
            if (heapSnapshotJSONSerializer != nullptr) {
                heapSnapshotJSONSerializer->Serialize();
                delete heapSnapshotJSONSerializer;
            } else {
                LOG(RTLOG_ERROR, "heapSnapshotJSONSerializer Init Failed");
            }
            break;
#endif
        }

        case GCTask::TaskType::TASK_TYPE_DUMP_HEAP_OOM: {
            CjHeapData* cjHeapData = new CjHeapData(true);
            if (cjHeapData != nullptr) {
                cjHeapData->DumpHeap();
                delete cjHeapData;
            } else {
                LOG(RTLOG_ERROR, "cjHeapData Init Failed");
            }
#ifdef COV_SIGNALHANDLE
            __gcov_dump();
#endif
            break;
        }
        default:
            LOG(RTLOG_ERROR, "[GC] Error task type: %u ignored!", static_cast<uint32_t>(taskType));
            break;
    }
    return true;
}
}
