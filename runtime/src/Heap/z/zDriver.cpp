// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/shared/stringdedup/stringDedup.hpp"
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

static const ZStatPhaseCollection ZPhaseCollectionMajor("Major Collection", false);
static const ZStatPhaseCollection ZPhaseCollectionMinor("Minor Collection", true);
static const ZStatPhaseGeneration ZPhaseGenerationOld("Old Generation", ZGenerationId::old);
static const ZStatPhaseGeneration ZPhaseGenerationYoung("Young Generation", ZGenerationId::young);
std::mutex ZDriver::driverLock;

static bool ShouldPrecleanYoung(GCReason reason);
static bool ShouldClearAllSoftReferences(GCReason reason);

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
ZDriver::ZDriver(GCDriverKind kind, ZDriverPort& port)
    : kind(kind), port(port)
{
    set_name(kind == GCDriverKind::MINOR ? "ZDriverMinor" : "ZDriverMajor");
}

void ZDriver::run_thread()
{
    // Runtime finalization is a host lifecycle extension. Every driver exit,
    // including an abortpoint, answers queued allocations before terminating.
    struct FinishStalls {
        ~FinishStalls() { Heap::GetHeap().page_allocator().StopStalledAllocations(); }
    } finishStalls;
    for (;;) {
        const ZDriverRequest request = port.receive();
        if (request.cause() == GC_REASON_INVALID) {
            return;
        }
        {
            DriverLocker locker;
            const bool major = kind == GCDriverKind::MAJOR;
            if (major) ZBreakpoint::AtBeforeGC();
            abortpoint();
            const bool completed = !ZAbort::should_abort() && ExecuteDriverRequest(request);
            port.ack();
            if (completed) { HandleAllocStalls(); }
            if (major) ZBreakpoint::AtAfterGC();
            if (completed && !major) ZDirector::evaluate_rules();
        }
        abortpoint();
    }
}

// ZGC zDriver.cpp:193-198,454-460: generation-specific stall ownership.
void ZDriverMinor::HandleAllocStalls() const
{
    Heap::GetHeap().page_allocator().HandleAllocStallingForYoung();
}

void ZDriverMajor::HandleAllocStalls() const
{
    Heap::GetHeap().page_allocator().HandleAllocStallingForOld(Heap::GetHeap().GetFinalizerProcessor()
        .GetReferenceProcessor().uses_clear_all_soft_reference_policy());
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
        case GC_REASON_ALLOCATION_STALL:
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
        case GC_REASON_ALLOCATION_STALL:
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

void ZDriver::RunCollection(uint64_t index, GCReason reason, bool warmup)
{
    const bool isYoung = reason == GC_REASON_YOUNG;
    ZGeneration& generation = Heap::GetHeap().GetZGeneration(isYoung
        ? ZGenerationId::young : ZGenerationId::old);
    ZStatCycle& cycle = generation.CycleStats();
    const uint64_t start = TimeUtil::NanoSeconds();
    const ZYoungType type = Heap::GetHeap().GetZGeneration(ZGenerationId::young).YoungType();
    // zGeneration.cpp:381,388: at_start/at_end(stat_workers, should_record_stats)
    // bracket the collection; the parallel share is read from ZStatWorkers.
    const bool recordStats = !isYoung || type == ZYoungType::minor || type == ZYoungType::major_partial_roots;
    cycle.AtStart(start);
    ZDriver::RunGarbageCollection(index, reason);
    const uint64_t end = TimeUtil::NanoSeconds();
    cycle.AtEnd(end, generation.StatWorkers(), warmup, recordStats);
    (isYoung ? ZPhaseGenerationYoung : ZPhaseGenerationOld).RegisterEnd(start, end);
}

bool ZDriver::ExecuteDriverRequest(const ZDriverRequest& request)
{
    CHECK(request.cause() < GC_REASON_MAX);
    if (ZAbort::should_abort()) {
        return false;
    }
    GCIdMark gcId;
    const uint64_t collectionStart = TimeUtil::NanoSeconds();
    size_t liveBefore = 0;
    size_t liveAfter = 0;
    size_t collected = 0;
    bool firstGeneration = true;
    // ZServiceabilityCycleTracer spans the request, including all young
    // prelude phases of a major. Capture existing generation stats before reuse.
    const auto accumulate = [&](ZGenerationId generation) {
        // rec=cycle fields are read from the generation's ZStatHeap account
        // (zStat.cpp:1703-2036 sampling points).
        ZStatHeap* statHeap = Heap::GetHeap().GetZGeneration(generation).StatHeap();
        if (firstGeneration) liveBefore = statHeap->UsedAtCollectionStart();
        firstGeneration = false;
        liveAfter = statHeap->UsedAtRelocateEnd();
        collected += statHeap->ReclaimedAtRelocateEnd();
    };

    // Set the request's generation budgets before mark-start can consume
    // them, including the old mark domain prepared by the young prelude.
    const uint32_t youngCount = request.young_nworkers() == 0
        ? ZCollectedHeap::heap()->concurrent_gc_threads() : request.young_nworkers();
    const uint32_t oldCount = request.old_nworkers() == 0
        ? ZCollectedHeap::heap()->concurrent_gc_threads() : request.old_nworkers();
    const bool warmup = request.cause() == GC_REASON_WARMUP;
    // zDriver.cpp:166-176 / zGeneration.cpp:154: the request carries the
    // selected worker counts into each generation's ZWorkers.
    Heap::GetHeap().GetZGeneration(ZGenerationId::young).Workers()->set_active_workers(youngCount);
    if (kind == GCDriverKind::MAJOR) {
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).Workers()->set_active_workers(oldCount);
        Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor()
            .set_soft_reference_policy(ShouldClearAllSoftReferences(request.cause()));
    }

    // zDriver.cpp:416-436: full causes preclean with promote-all, then
    // establish the combined young/old roots cycle. Other causes use partial roots.
    if (kind == GCDriverKind::MAJOR) {
        ZGCIdMajor majorId(GCIdMark::Current(), 'Y');
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).SelectReason(
            request.cause(), GCTask::ASYNC_TASK_INDEX);
        const bool preclean = ShouldPrecleanYoung(request.cause());
        if (preclean) {
            ZCollectedHeap::heap()->driver_major()->RunYoungCollection(
                GCTask::ASYNC_TASK_INDEX, ZYoungType::major_full_preclean, warmup);
            accumulate(ZGenerationId::young);
            if (ZAbort::should_abort()) {
                Heap::GetHeap().GetZGeneration(kind == GCDriverKind::MINOR
                    ? ZGenerationId::young : ZGenerationId::old).End();
                return false;
            }
        }
        ZCollectedHeap::heap()->driver_major()->RunYoungCollection(GCTask::ASYNC_TASK_INDEX,
            preclean ? ZYoungType::major_full_roots : ZYoungType::major_partial_roots, warmup);
        accumulate(ZGenerationId::young);
        if (ZAbort::should_abort()) {
            Heap::GetHeap().GetZGeneration(kind == GCDriverKind::MINOR
                ? ZGenerationId::young : ZGenerationId::old).End();
            return false;
        }
    }
    // ZGC zDriver.cpp:434: major young completion also advances stalled requests.
    if (kind == GCDriverKind::MAJOR) {
        Heap::GetHeap().page_allocator().HandleAllocStallingForYoung();
    }
    VLOG(GCPHASE, "[GCV2][driver] kind=%s seq=%llu reason=%u ack=pending",
         kind == GCDriverKind::MINOR ? "minor" : "major",
         0ull, request.cause());
    const uint64_t index = GCTask::ASYNC_TASK_INDEX;
    if (kind == GCDriverKind::MINOR) {
        ZGCIdMinor minorId(GCIdMark::Current());
        ZCollectedHeap::heap()->driver_minor()->RunYoungCollection(index, ZYoungType::minor, warmup);
        accumulate(ZGenerationId::young);
    } else {
        ZGCIdMajor majorId(GCIdMark::Current(), 'O');
        ZCollectedHeap::heap()->driver_major()->RunCollection(index, request.cause(), warmup);
        accumulate(ZGenerationId::old);
    }
    (kind == GCDriverKind::MINOR ? ZPhaseCollectionMinor : ZPhaseCollectionMajor)
        .RegisterEnd(collectionStart, TimeUtil::NanoSeconds());
    // A stop during marking or relocation is cancellation, even though the
    // collection call has returned after joining its work and page cleanup.
    if (ZAbort::should_abort()) {
        Heap::GetHeap().GetZGeneration(kind == GCDriverKind::MINOR
            ? ZGenerationId::young : ZGenerationId::old).End();
        return false;
    }
    GcLog::Cycle(GCIdMark::Current(), kind == GCDriverKind::MINOR ? "minor" : "major",
                 g_gcRequests[request.cause()].name, collectionStart, TimeUtil::NanoSeconds() - collectionStart,
                 liveBefore, liveAfter, collected, Heap::GetHeap().GetUsedPageSize());
    return true;
}


} // namespace MapleRuntime

namespace MapleRuntime {
void ZDriver::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{

    const ZGenerationId generation = reason == GC_REASON_YOUNG
        ? ZGenerationId::young : ZGenerationId::old;
    ZGeneration& cycle = Heap::GetHeap().GetZGeneration(generation);
    if (!cycle.Snapshot().active) {
        cycle.SelectReason(reason);
    }
    cycle.PreGarbageCollection(reason != GC_REASON_YOUNG, gcIndex);
    ScheduleTraceEvent(TRACE_EV_GC_START, -1, nullptr, 0);
    VLOG(REPORT, "[GC] Start ZGC %s gcIndex= %lu", g_gcRequests[reason].name, gcIndex);
    ZStatHeap* statHeap = cycle.StatHeap();
    const uint64_t gcStartTimeNs = TimeUtil::NanoSeconds();

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
        cycle.End();
        return;
    }

    cycle.PostGarbageCollection(gcIndex);
    const uint64_t gcEndTimeNs = TimeUtil::NanoSeconds();
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
    GcLog::Phase(GCIdMark::Current(), phaseName, "unknown", gcStartTimeNs, gcEndTimeNs - gcStartTimeNs);
    uint64_t gcTimeNs = gcEndTimeNs - gcStartTimeNs;
    ScheduleTraceEvent(TRACE_EV_GC_DONE, -1, nullptr, 0);
    const size_t reclaimedThisCollection = statHeap->ReclaimedAtRelocateEnd();
    double rate = (static_cast<double>(reclaimedThisCollection) / gcTimeNs) * (static_cast<double>(NS_PER_S) / MB);
    VLOG(REPORT, "total gc time: %s us, collection rate %.3lf MB/s\n", Pretty(gcTimeNs / NS_PER_US).Str(), rate);
    g_gcTotalTimeUs.fetch_add(gcTimeNs / NS_PER_US, std::memory_order_release);
    g_gcCollectedTotalBytes.fetch_add(reclaimedThisCollection, std::memory_order_release);
    if (reason != GC_REASON_YOUNG) {
        ZStat::SetPrevGCFinishTime(TimeUtil::NanoSeconds());
    }
    // zStatHeap::at_relocate_end (zGeneration.cpp:935-940): publish only to
    // the generation being collected; the account reads the allocator stats.
    const bool young = reason == GC_REASON_YOUNG;
    const size_t usedAfter = Heap::GetHeap().GetAllocatedSize();
    statHeap->AtRelocateEnd(
        static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().Stats(&cycle),
        cycle.should_record_stats());
    if (!young) {
        // RegionManager's allocation pacing reads the post-major baseline.
        static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager()
            .SetLastCollectionStats(usedAfter, rate);
    }
    cycle.End();
}
}

namespace MapleRuntime {
void ZDriver::RunYoungCollection(uint64_t index, ZYoungType type, bool warmup)
{
    YoungTypeSetter typeSetter(Heap::GetHeap().GetZGeneration(ZGenerationId::young), type);
    RunCollection(index, GC_REASON_YOUNG, warmup);
}

static bool ShouldClearAllSoftReferences(GCReason reason)
{
    // ZGC zDriver.cpp:232-268: stall and explicit full collections clear soft refs.
    switch (reason) {
        case GC_REASON_OOM:
        case GC_REASON_FORCE:
        case GC_REASON_ALLOCATION_STALL:
            return true;
        case GC_REASON_USER:
        case GC_REASON_BACKUP:
        case GC_REASON_HEU:
        case GC_REASON_HEU_SYNC:
        case GC_REASON_NATIVE:
        case GC_REASON_NATIVE_SYNC:
        case GC_REASON_WARMUP:
        case GC_REASON_WB_BREAKPOINT:
            break;
        default:
            CHECK(false);
    }
    const auto& manager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    return manager.IsAllocationStallingForOld();
}

static bool ShouldPrecleanYoung(GCReason reason)
{
    // ZGC zDriver.cpp:270-299: explicit full collections, including breakpoints.
    switch (reason) {
        case GC_REASON_USER:
        case GC_REASON_OOM:
        case GC_REASON_FORCE:
        case GC_REASON_WB_BREAKPOINT:
        case GC_REASON_ALLOCATION_STALL:
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
    // ZGC zDriver.cpp:294-299: requests that have seen young but not old.
    const auto& manager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    return manager.IsAllocationStallingForOld();
}


#ifdef COV_SIGNALHANDLE
extern "C" void __gcov_dump(void);
#endif

bool GCExecutor::Execute(void* owner)
{
    MRT_ASSERT(owner != nullptr, "task queue owner ptr should not be null!");

    switch (taskType) {
        case GCTask::TaskType::TASK_TYPE_TERMINATE_GC: {
            return false;
        }
        case GCTask::TaskType::TASK_TYPE_TIMEOUT_GC: {
            uint64_t curTime = TimeUtil::NanoSeconds();
            if ((curTime - ZStat::GetPrevGCStartTime()) > CangjieRuntime::GetGCParam().backupGCInterval) {
                ZStat::SetPrevGCStartTime(curTime);
                ZDriver::RunGarbageCollection(GCTask::ASYNC_TASK_INDEX, GC_REASON_BACKUP);
            }
            break;
        }
        case GCTask::TaskType::TASK_TYPE_INVOKE_GC: {
            ZStat::SetPrevGCStartTime(TimeUtil::NanoSeconds());
            ZDriver::RunGarbageCollection(taskIndex, gcReason);
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
