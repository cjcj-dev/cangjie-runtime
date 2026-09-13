// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "GcStats.h"

#include "Base/GcLog.h"
#include "Base/LogFile.h"
#include "Heap/Collector/GcTrigger.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
std::atomic<uint64_t> g_gcTotalTimeUs{ 0 };
std::atomic<size_t> g_gcCollectedTotalBytes{ 0 };

std::atomic<uint64_t> GCStats::prevGcStartTime{ TimeUtil::NanoSeconds() - LONG_MIN_HEU_GC_INTERVAL_NS };
std::atomic<uint64_t> GCStats::prevGcFinishTime{ TimeUtil::NanoSeconds() - LONG_MIN_HEU_GC_INTERVAL_NS };

void GCStats::Init()
{
    isConcurrentMark = false;
    async = false;
    gcStartTime = TimeUtil::NanoSeconds();
    gcEndTime = TimeUtil::NanoSeconds();
    collectedObjects = 0;
    collectedBytes = 0;
    youngCandidateBytes = 0;
    youngPromotedBytes = 0;
    tenuringThreshold = 0;
    for (size_t i = 0; i < 16; ++i) {
        liveByAge[i] = 0;
    }
    youngHeuDeferralUsed = false;

    fromSpaceSize = 0;
    smallGarbageSize = 0;

    pinnedSpaceSize = 0;
    pinnedGarbageSize = 0;

    largeSpaceSize = 0;
    largeGarbageSize = 0;

    liveBytesBeforeGC = 0;
    liveBytesAfterGC = 0;

    garbageRatio = 0.0;
    collectionRate = 0.0;

    size_t maxCapacity = Heap::GetHeap().GetMaxCapacity();
    size_t threshold = std::min(CangjieRuntime::GetGCParam().gcThreshold, 20 * MB);
    threshold = std::min(static_cast<size_t>(maxCapacity * 0.2), threshold);
    heapThreshold.store(threshold, std::memory_order_relaxed);
    VLOG(REPORT, "[GCV2][jvm-ihop] enabled=0 initial-threshold=%zu max-capacity=%zu adaptive-update=1",
         heapThreshold.load(std::memory_order_relaxed), maxCapacity);
}

GCStats::YoungHeuThrottleDecision GCStats::RecordYoungGCFinish(uint64_t timestamp, size_t allocatedAfter,
                                                               size_t promotedBytes, size_t candidateBytes,
                                                               size_t maxCapacity, uint64_t durationNs,
                                                               uint64_t heuMinIntervalNs, bool deferralEnabled)
{
    if (!deferralEnabled) {
        return YoungHeuThrottleDecision::DISABLED;
    }
    if (candidateBytes == 0) {
        return YoungHeuThrottleDecision::NO_COLLECTION_SET;
    }

    // Stay well inside the copying major's half-heap to-space reserve. The
    // quarter-heap limit is the measured safe point for bounded deferral.
    const size_t majorSafetyLimit = maxCapacity / 4;
    const bool oldPressureHigh = allocatedAfter >= majorSafetyLimit ||
        promotedBytes >= majorSafetyLimit - allocatedAfter;
    if (oldPressureHigh) {
        return YoungHeuThrottleDecision::OLD_PRESSURE_HIGH;
    }
    // A short minor completed inside the suppression budget that was already
    // established by the preceding major. Restarting the full window here
    // would add latency without closing the slow-minor hole.
    if (durationNs < heuMinIntervalNs) {
        return YoungHeuThrottleDecision::WITHIN_EXISTING_HEU_WINDOW;
    }
    if (youngHeuDeferralUsed) {
        return YoungHeuThrottleDecision::DEFERRAL_ALREADY_USED;
    }

    youngHeuDeferralUsed = true;
    SetPrevGCFinishTime(timestamp);
    return YoungHeuThrottleDecision::REFRESHED;
}

void GCStats::RecordMajorGCFinish(uint64_t timestamp, uint64_t, size_t, size_t)
{
    youngHeuDeferralUsed = false;
    SetPrevGCFinishTime(timestamp);
}

const char* GCStats::YoungHeuThrottleDecisionName(YoungHeuThrottleDecision decision)
{
    switch (decision) {
        case YoungHeuThrottleDecision::REFRESHED:
            return "refreshed";
        case YoungHeuThrottleDecision::DISABLED:
            return "disabled";
        case YoungHeuThrottleDecision::NO_COLLECTION_SET:
            return "no-collection-set";
        case YoungHeuThrottleDecision::DEFERRAL_ALREADY_USED:
            return "deferral-already-used";
        case YoungHeuThrottleDecision::OLD_PRESSURE_HIGH:
            return "old-pressure-high";
        case YoungHeuThrottleDecision::WITHIN_EXISTING_HEU_WINDOW:
            return "within-existing-window";
        default:
            return "invalid";
    }
}

void GCStats::Dump() const
{
    // Print a summary of the last GC.
    size_t liveSize = Heap::GetHeap().GetAllocatedSize();
    size_t heapSize = Heap::GetHeap().GetUsedPageSize();
    double utilization = (heapSize == 0) ? 0 : (static_cast<double>(liveSize) / heapSize) * 100; // 100 for percentage.

    // Do not change this GC log format.
    // Output one line statistic info after each gc task,
    // include the gc type, collected objects and current heap utilization, etc.
    // display to std-output. take care to modify.
    LOG(RTLOG_INFO,
        "GC for %s: %s collected objects: %zu->%s, %.2f%% utilization (%zu->%s/%zu->%s), "
        "total GC time: %llu->%s",
        g_gcRequests[reason].name, (async ? "async:" : "sync:"),
        collectedBytes, PrettyOrderInfo(collectedBytes, "B").Str(),
        utilization, liveSize, PrettyOrderInfo(liveSize, "B").Str(),
        heapSize, PrettyOrderInfo(heapSize, "B").Str(),
        gcEndTime - gcStartTime, PrettyOrderMathNano(gcEndTime - gcStartTime, "s").Str());

    VLOG(REPORT, "allocated size: %s, heap size: %s, heap utilization: %.2f%%", Pretty(liveSize).Str(),
         Pretty(heapSize).Str(), utilization);
}
} // namespace MapleRuntime
