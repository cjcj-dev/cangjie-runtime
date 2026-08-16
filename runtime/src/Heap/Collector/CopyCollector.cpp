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
#include "Allocator/RegionSpace.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/HealPairDiag.h"
#include "Heap/Verify/NoTracedDiag.h"
#include "Common/Runtime.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/SatbBuffer.h"
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
    const bool stall = HealPairDiag::Enabled() && HealPairDiag::MidCopyStallNs() > 0 && size > 8;
    // NoteCopy self-gates (healpair OR whozero); always invoke so whozero can see moves.
    HealPairDiag::NoteCopy(reinterpret_cast<const void*>(from), reinterpret_cast<const void*>(to), size, 0);
    if (UNLIKELY(NoTracedDiag::Enabled())) {
        NoTracedDiag::NoteCopy(reinterpret_cast<const void*>(from), reinterpret_cast<const void*>(to), size, 0);
    }
    if (stall) {
        // Tip first so WaitRoutedTipReady / relocate_or_remap can admit the to-address
        // while high offsets are still the reservation zero. Default-off (COPYSTALL_NS).
        CHECK_E(memmove_s(reinterpret_cast<void*>(to), 8, reinterpret_cast<void*>(from), 8) != EOK, "memmove_s fail");
        HealPairDiag::MaybeMidCopyStall(size);
        CHECK_E(memmove_s(reinterpret_cast<void*>(to + 8), size - 8, reinterpret_cast<void*>(from + 8), size - 8) !=
                    EOK,
                "memmove_s fail");
    } else {
        CHECK_E(memmove_s(reinterpret_cast<void*>(to), size, reinterpret_cast<void*>(from), size) != EOK,
                "memmove_s fail");
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanFixShadow(reinterpret_cast<void*>(from), reinterpret_cast<void*>(to), size);
#endif
    HealPairDiag::NoteCopy(reinterpret_cast<const void*>(from), reinterpret_cast<const void*>(to), size, 1);
    if (UNLIKELY(NoTracedDiag::Enabled())) {
        NoTracedDiag::NoteCopy(reinterpret_cast<const void*>(from), reinterpret_cast<const void*>(to), size, 1);
    }
}

void CopyCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    ScopedEntryTrace trace("CJRT_GC_START");
    // prevent other threads stop-the-world during GC.
    // this may be removed in the future.
    ScopedSTWLock stwLock;
    // ScopedStopTheWorld stw;

    gcReason = reason;
    PreGarbageCollection(reason != GC_REASON_YOUNG);
    ScheduleTraceEvent(TRACE_EV_GC_START, -1, nullptr, 0);
    VLOG(REPORT, "[GC] Start %s %s gcIndex= %lu", GetCollectorName(), g_gcRequests[gcReason].name, gcIndex);
    GCStats& gcStats = GetGCStats();
    gcStats.collectedBytes = 0;
    gcStats.youngCandidateBytes = 0;
    gcStats.youngPromotedBytes = 0;
    gcStats.gcStartTime = TimeUtil::NanoSeconds();

    DoGarbageCollection();

    if (reason == GC_REASON_OOM) {
        Heap::GetHeap().GetAllocator().ReclaimGarbageMemory(true);
    }

    PostGarbageCollection(gcIndex);
    gcStats.gcEndTime = TimeUtil::NanoSeconds();
    // Emitted here rather than from GCStats::Dump, because UpdateGCStats below (and so Dump) is
    // skipped for young collections: a minor would produce no cycle record and its phases would
    // be attributed to the next major.
    GcLog::Cycle(GcLog::CompleteCycle(), reason == GC_REASON_YOUNG ? "minor" : "major",
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
    g_gcCount.fetch_add(1, std::memory_order_release);
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
        gcStats.RecordMajorGCFinish(finishTime);
    }
    collectorResources.NotifyGCFinished(gcIndex);
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
    GCThreadPool* copyPool = GetThreadPool();
    const char* poolKind = "shared";
    int32_t previousActiveHelpers = 0;
    bool restoreActiveHelpers = false;
    if (gcReason == GC_REASON_YOUNG) {
        const char* forceSerialEnv = std::getenv("MRT_GCV2_EVACPAR_FORCE_SERIAL");
        const bool forceSerial =
            forceSerialEnv != nullptr && std::strcmp(forceSerialEnv, "1") == 0;
        const char* workGateEnv = std::getenv("MRT_GCV2_EVACPAR_WORK_GATE");
        const bool workGate = workGateEnv != nullptr && std::strcmp(workGateEnv, "1") == 0;
        GCThreadPool* evacuationPool = collectorResources.GetEvacuationThreadPool();
        if (forceSerial) {
            copyPool = nullptr;
            poolKind = "serial";
        } else if (evacuationPool != nullptr) {
            copyPool = evacuationPool;
            poolKind = "dedicated";
        }

        const int32_t maxWorkers = copyPool == nullptr ? 1 : copyPool->GetMaxThreadNum() + 1;
        int32_t workers = maxWorkers;
        constexpr size_t bytesPerWorker = 1 * MB;
        if (!forceSerial && workGate) {
            const size_t workersForBytes = std::max<size_t>(stats.fromSpaceSize / bytesPerWorker, 1);
            workers = static_cast<int32_t>(std::min<size_t>(workersForBytes, static_cast<size_t>(maxWorkers)));
            if (workers == 1) {
                copyPool = nullptr;
                poolKind = "serial";
            } else {
                previousActiveHelpers = copyPool->GetMaxActiveThreadNum();
                const int32_t activeHelpers = workers - 1;
                if (activeHelpers != previousActiveHelpers) {
                    copyPool->SetMaxActiveThreadNum(activeHelpers);
                    restoreActiveHelpers = true;
                }
            }
        }
        VLOG(REPORT,
             "[GCV2][evacpar][copy] parallel=%u workers=%d bytes=%zu bytesPerWorker=%zu maxWorkers=%d "
             "pool=%s forceSerial=%u workGate=%u",
             static_cast<unsigned>(copyPool != nullptr), workers, stats.fromSpaceSize, bytesPerWorker, maxWorkers,
             poolKind, static_cast<unsigned>(forceSerial), static_cast<unsigned>(workGate));
    }
    space.ForwardFromSpace(copyPool);
    if (restoreActiveHelpers) {
        copyPool->SetMaxActiveThreadNum(previousActiveHelpers);
    }
}

void CopyCollector::RefineFromSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    stats.smallGarbageSize = space.RefineFromSpace();
}
} // namespace MapleRuntime
