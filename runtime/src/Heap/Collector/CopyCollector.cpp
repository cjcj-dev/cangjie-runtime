// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CopyCollector.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Base/GcLog.h"
#include "Heap/z/zStat.hpp"
#include "Allocator/RegionSpace.h"
#include "Heap/z/zDirector.hpp"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Common/Runtime.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#include "schedule.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
void CopyCollector::PostGarbageCollection(uint64_t gcIndex)
{
    reinterpret_cast<RegionSpace&>(theAllocator).DumpRegionStats("region statistics when gc ends");
    TracingCollector::PostGarbageCollection(gcIndex);
    MutatorManager::Instance().DestroyExpiredMutators();
}

void CopyCollector::CopyObject(const BaseObject& fromObj, BaseObject& toObj, size_t size) const
{
    uintptr_t from = reinterpret_cast<uintptr_t>(&fromObj);
    uintptr_t to = reinterpret_cast<uintptr_t>(&toObj);
    const bool overlap = to < from && to + size > from;
    const bool restoreLocked = overlap && fromObj.GetStateWord().IsLockedWord();

    CHECK_E(memmove_s(reinterpret_cast<void*>(to), size, reinterpret_cast<void*>(from), size) != EOK,
            "memmove_s fail");
    // A conjoint relocation can overwrite the source header while the copier
    // still owns its lock. Restore only the state bits before UnlockObject
    // publishes the forwarding receipt.
    if (restoreLocked) {
        const_cast<BaseObject&>(fromObj).SetStateCode(ObjectState::LOCKED);
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanFixShadow(reinterpret_cast<void*>(from), reinterpret_cast<void*>(to), size);
#endif

}

void CopyCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    ScopedEntryTrace trace("CJRT_GC_START");
    const uint64_t cycleSeq = GcLog::BeginCycle();
    // prevent other threads stop-the-world during GC.
    // this may be removed in the future.
    ScopedSTWLock stwLock;
    // ScopedStopTheWorld stw;

    SelectCycle(reason);
    PreGarbageCollection(reason != GC_REASON_YOUNG, gcIndex);
    ScheduleTraceEvent(TRACE_EV_GC_START, -1, nullptr, 0);
    VLOG(REPORT, "[GC] Start %s %s gcIndex= %lu", GetCollectorName(), g_gcRequests[GetCycleReason()].name, gcIndex);
    GCStats& gcStats = GetGCStats();
    gcStats.collectedBytes = 0;
    gcStats.youngCandidateBytes = 0;
    gcStats.youngPromotedBytes = 0;
    gcStats.tenuringThreshold = 0;
    gcStats.gcStartTime = TimeUtil::NanoSeconds();

    // One GC cycle is the roots verification scene: it covers both the minor
    // and major root visitors, including concurrent stack enumeration.  Close
    // after the collector has joined all root work (zVerify.cpp:363-384).
    DoGarbageCollection();

    GCDriverPort& port = reason == GC_REASON_YOUNG ? collectorResources.GetYoungDriverPort() :
                                                   collectorResources.GetMajorDriverPort();
    if (port.Abort().Poll()) {
        // The phase owner already joined any submitted work. Keep mark and
        // forwarding storage alive for driver shutdown; skip normal reclaim.
        GetWorkers().SetInactive();
        GcLog::CompleteCycle(cycleSeq);
        return;
    }

    if (reason == GC_REASON_OOM) {
        Heap::GetHeap().GetAllocator().ReclaimGarbageMemory(true);
    }

    PostGarbageCollection(gcIndex);
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
    uint64_t finishTime = TimeUtil::NanoSeconds();
    if (reason == GC_REASON_YOUNG) {
        size_t allocatedAfter = Heap::GetHeap().GetAllocatedSize();
        size_t maxCapacity = Heap::GetHeap().GetMaxCapacity();
        uint64_t heuMinInterval = g_gcRequests[GC_REASON_HEU].GetMinInterval();
        // Default on; an exact 0 is the operational rollback for young HEU deferral.
        const char* minorDefersHeuEnv = std::getenv("MRT_GCV2_MINOR_DEFERS_HEU");
        const bool minorDefersHeu =
            minorDefersHeuEnv == nullptr || std::strcmp(minorDefersHeuEnv, "0") != 0;
        GCStats::YoungHeuThrottleDecision decision = gcStats.RecordYoungGCFinish(
            finishTime, allocatedAfter, gcStats.youngPromotedBytes, gcStats.youngCandidateBytes, maxCapacity,
            gcTimeNs, heuMinInterval, minorDefersHeu);
        VLOG(REPORT,
             "[GCV2][heu-loop] minor-finish action=%s enabled=%d allocated-after=%zu promoted=%zu "
             "candidate=%zu duration-ns=%llu HEU-min-interval-ns=%llu major-safety-limit=%zu",
             GCStats::YoungHeuThrottleDecisionName(decision), minorDefersHeu, allocatedAfter,
             gcStats.youngPromotedBytes, gcStats.youngCandidateBytes, static_cast<unsigned long long>(gcTimeNs),
             static_cast<unsigned long long>(heuMinInterval), maxCapacity / 4);
    } else {
        gcStats.RecordMajorGCFinish(finishTime, gcTimeNs, Heap::GetHeap().GetAllocatedSize(),
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
    ActiveCycle().End();
    collectorResources.NotifyGCPhaseFinished(gcIndex);
}

void CopyCollector::ForwardFromSpace()
{
    ScopedEntryTrace trace("CJRT_GC_FORWARD");
    TransitionToGCPhase(GCPhase::GC_PHASE_FORWARD, true);

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    GCStats& stats = GetGCStats();
    stats.liveBytesBeforeGC = space.AllocatedBytes();
    stats.fromSpaceSize = space.FromSpaceSize();
    GarbRegionDiag::CensusBeforeForward("pre-forward");
    if (GetCycleReason() == GC_REASON_YOUNG) {
        space.ForwardFromSpace<Generation::Young>(GetWorkers());
    } else {
        space.ForwardFromSpace<Generation::Old>(GetWorkers());
    }

}

void CopyCollector::RefineFromSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    stats.smallGarbageSize = space.RefineFromSpace();
}
} // namespace MapleRuntime
