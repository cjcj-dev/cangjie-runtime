// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "GcStats.h"

#include <cstdlib>
#include <cstring>

#include "Base/GcLog.h"
#include "Base/LogFile.h"
#include "Heap/Collector/GcTrigger.h"
#include "Heap/Collector/TruncatedSeq.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
std::atomic<size_t> g_gcCount{ 0 };
std::atomic<uint64_t> g_gcTotalTimeUs{ 0 };
std::atomic<size_t> g_gcCollectedTotalBytes{ 0 };

std::atomic<uint64_t> GCStats::lastOldDurationNs{ 0 };
std::atomic<uint64_t> GCStats::lastMajorFinishNs{ 0 };
std::atomic<uint32_t> GCStats::collectionsAtLastMajor{ 0 };
std::atomic<size_t> GCStats::usedAtLastMajorEnd{ 0 };
std::atomic<size_t> GCStats::oldLiveAtMarkEnd{ 0 };
std::atomic<double> GCStats::reclaimedPerYoungAvg{ 0.0 };
std::atomic<double> GCStats::reclaimedPerOldAvg{ 0.0 };
std::atomic<double> GCStats::lastYoungGcDurationAvgSec{ 0.0 };
std::atomic<double> GCStats::lastOldGcDurationAvgSec{ 0.0 };

namespace {
TruncatedSeq g_youngDurationSeq{ 10 };
TruncatedSeq g_oldDurationSeq{ 10 };
TruncatedSeq g_youngReclaimedSeq{ 10 };
TruncatedSeq g_oldReclaimedSeq{ 10 };
} // namespace

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

    lastYoungCandidateBytes.store(0, std::memory_order_relaxed);
    lastYoungPromotedBytes.store(0, std::memory_order_relaxed);
    lastYoungCollectedBytes.store(0, std::memory_order_relaxed);
    lastYoungDurationNs.store(0, std::memory_order_relaxed);
    hasYoungSample.store(false, std::memory_order_relaxed);
    youngTriggerBytes.store(kGcTriggerYoungFixedBytes, std::memory_order_relaxed);
    warmupCyclesDone.store(0, std::memory_order_relaxed);
    isWarm.store(false, std::memory_order_relaxed);
    isTimeTrustable.store(false, std::memory_order_relaxed);
    lastGcDurationNs.store(0, std::memory_order_relaxed);
    lastOldDurationNs.store(0, std::memory_order_relaxed);
    lastMajorFinishNs.store(0, std::memory_order_relaxed);
    collectionsAtLastMajor.store(0, std::memory_order_relaxed);
    usedAtLastMajorEnd.store(0, std::memory_order_relaxed);
    oldLiveAtMarkEnd.store(0, std::memory_order_relaxed);
    reclaimedPerYoungAvg.store(0.0, std::memory_order_relaxed);
    reclaimedPerOldAvg.store(0.0, std::memory_order_relaxed);
    lastYoungGcDurationAvgSec.store(0.0, std::memory_order_relaxed);
    lastOldGcDurationAvgSec.store(0.0, std::memory_order_relaxed);
    g_youngDurationSeq.reset();
    g_oldDurationSeq.reset();
    g_youngReclaimedSeq.reset();
    g_oldReclaimedSeq.reset();

    const char* jvmIhopEnv = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_JVM_IHOP */;
    const bool useJvmIhop = jvmIhopEnv != nullptr && std::strcmp(jvmIhopEnv, "1") == 0;
    size_t maxCapacity = Heap::GetHeap().GetMaxCapacity();
    if (useJvmIhop) {
        // Modern G1 uses 45% only as its initial IHOP; UpdateGCStats remains the adaptive controller here.
        constexpr size_t initialHeapOccupancyPercent = 45;
        heapThreshold.store(maxCapacity * initialHeapOccupancyPercent / 100, std::memory_order_relaxed);
    } else {
        // 20 MB:set 20 MB as intial value
        size_t threshold = std::min(CangjieRuntime::GetGCParam().gcThreshold, 20 * MB);
        // 0.2:set 20% heap size as intial value
        threshold = std::min(static_cast<size_t>(maxCapacity * 0.2), threshold);
        heapThreshold.store(threshold, std::memory_order_relaxed);
    }
    VLOG(REPORT, "[GCV2][jvm-ihop] enabled=%d initial-threshold=%zu max-capacity=%zu adaptive-update=1",
         useJvmIhop, heapThreshold.load(std::memory_order_relaxed), maxCapacity);
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

void GCStats::RecordYoungStats(size_t candidateBytes, size_t promotedBytes, size_t collectedBytes, uint64_t durationNs,
                              size_t maxCapacity)
{
    lastYoungCandidateBytes.store(candidateBytes, std::memory_order_relaxed);
    lastYoungPromotedBytes.store(promotedBytes, std::memory_order_relaxed);
    lastYoungCollectedBytes.store(collectedBytes, std::memory_order_relaxed);
    lastYoungDurationNs.store(durationNs, std::memory_order_relaxed);
    lastGcDurationNs.store(durationNs, std::memory_order_relaxed);
    hasYoungSample.store(true, std::memory_order_relaxed);
    YoungTriggerInputs in;
    in.capacityBytes = maxCapacity;
    in.heapThresholdBytes = GetThreshold();
    in.lastYoungCandidateBytes = candidateBytes;
    in.lastYoungPromotedBytes = promotedBytes;
    in.lastYoungCollectedBytes = collectedBytes;
    in.hasYoungSample = true;
    const size_t trigger = ComputeYoungTriggerBytes(in);
    youngTriggerBytes.store(trigger, std::memory_order_release);
    g_youngDurationSeq.add(static_cast<double>(durationNs) / static_cast<double>(SECOND_TO_NANO_SECOND));
    g_youngReclaimedSeq.add(static_cast<double>(collectedBytes));
    lastYoungGcDurationAvgSec.store(g_youngDurationSeq.avg(), std::memory_order_relaxed);
    reclaimedPerYoungAvg.store(g_youngReclaimedSeq.avg(), std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][gctrigger] young-watermark candidate=%zu promoted=%zu collected=%zu trigger=%zu cap=%zu heu=%zu",
         candidateBytes, promotedBytes, collectedBytes, trigger, maxCapacity, GetThreshold());
}

void GCStats::RecordMajorGCFinish(uint64_t timestamp, uint64_t durationNs, size_t usedAfter,
                                  size_t collectedBytes, uint32_t totalCollections)
{
    youngHeuDeferralUsed = false;
    SetPrevGCFinishTime(timestamp);
    lastMajorFinishNs.store(timestamp, std::memory_order_relaxed);
    lastOldDurationNs.store(durationNs, std::memory_order_relaxed);
    collectionsAtLastMajor.store(totalCollections, std::memory_order_relaxed);
    usedAtLastMajorEnd.store(usedAfter, std::memory_order_relaxed);
    oldLiveAtMarkEnd.store(usedAfter, std::memory_order_relaxed);
    if (durationNs > 0) {
        g_oldDurationSeq.add(static_cast<double>(durationNs) / static_cast<double>(SECOND_TO_NANO_SECOND));
        lastOldGcDurationAvgSec.store(g_oldDurationSeq.avg(), std::memory_order_relaxed);
    }
    if (collectedBytes > 0 || durationNs > 0) {
        g_oldReclaimedSeq.add(static_cast<double>(collectedBytes));
        reclaimedPerOldAvg.store(g_oldReclaimedSeq.avg(), std::memory_order_relaxed);
    }
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
