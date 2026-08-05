// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "GcStats.h"

#include <cstdlib>
#include <cstring>

#include "Base/LogFile.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
size_t g_gcCount = 0;
uint64_t g_gcTotalTimeUs = 0;
size_t g_gcCollectedTotalBytes = 0;

uint64_t GCStats::prevGcStartTime = TimeUtil::NanoSeconds() - LONG_MIN_HEU_GC_INTERVAL_NS;
uint64_t GCStats::prevGcFinishTime = TimeUtil::NanoSeconds() - LONG_MIN_HEU_GC_INTERVAL_NS;
std::atomic<uint32_t> GCStats::lastPrevGcFinishReason{GC_REASON_INVALID};
std::atomic<uint64_t> GCStats::setPrevFinishByReason[GC_REASON_MAX]{};
std::atomic<uint64_t> GCStats::throttleHitByReason[GC_REASON_MAX]{};
std::atomic<uint64_t> GCStats::throttleHitTotal{0};

namespace {
bool ThrottleProbeEnabled()
{
    static const bool enabled = []() {
        const char* v = std::getenv("MRT_GCV2_THROTTLE_PROBE");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return enabled;
}

const char* ReasonName(GCReason reason)
{
    if (reason < GC_REASON_MAX) {
        return g_gcRequests[reason].name;
    }
    return "invalid";
}
} // namespace

void GCStats::NoteSetPrevGCFinishTime(GCReason reason)
{
    if (!ThrottleProbeEnabled() || reason >= GC_REASON_MAX) {
        return;
    }
    lastPrevGcFinishReason.store(static_cast<uint32_t>(reason), std::memory_order_relaxed);
    setPrevFinishByReason[reason].fetch_add(1, std::memory_order_relaxed);
}

void GCStats::NoteThrottleHit(GCReason suppressedReason, GCReason lastWriterReason, uint64_t ageNs)
{
    if (!ThrottleProbeEnabled() || suppressedReason >= GC_REASON_MAX) {
        return;
    }
    throttleHitTotal.fetch_add(1, std::memory_order_relaxed);
    throttleHitByReason[suppressedReason].fetch_add(1, std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][throttle-probe] HIT suppressed=%s lastWriter=%s ageNs=%llu minIntervalNs=%llu",
         ReasonName(suppressedReason), ReasonName(lastWriterReason),
         static_cast<unsigned long long>(ageNs),
         static_cast<unsigned long long>(LONG_MIN_HEU_GC_INTERVAL_NS));
}

void GCStats::DumpThrottleProbe(const char* site)
{
    if (!ThrottleProbeEnabled()) {
        return;
    }
    VLOG(REPORT,
         "[GCV2][throttle-probe] dump site=%s totalHits=%llu lastWriter=%s "
         "setFinish[user=%llu oom=%llu backup=%llu heu=%llu native=%llu heu_sync=%llu "
         "native_sync=%llu force=%llu young=%llu] "
         "hits[user=%llu oom=%llu backup=%llu heu=%llu native=%llu heu_sync=%llu "
         "native_sync=%llu force=%llu young=%llu]",
         site,
         static_cast<unsigned long long>(throttleHitTotal.load(std::memory_order_relaxed)),
         ReasonName(static_cast<GCReason>(lastPrevGcFinishReason.load(std::memory_order_relaxed))),
         static_cast<unsigned long long>(setPrevFinishByReason[GC_REASON_USER].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(setPrevFinishByReason[GC_REASON_OOM].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(setPrevFinishByReason[GC_REASON_BACKUP].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(setPrevFinishByReason[GC_REASON_HEU].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(setPrevFinishByReason[GC_REASON_NATIVE].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(setPrevFinishByReason[GC_REASON_HEU_SYNC].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(setPrevFinishByReason[GC_REASON_NATIVE_SYNC].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(setPrevFinishByReason[GC_REASON_FORCE].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(setPrevFinishByReason[GC_REASON_YOUNG].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(throttleHitByReason[GC_REASON_USER].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(throttleHitByReason[GC_REASON_OOM].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(throttleHitByReason[GC_REASON_BACKUP].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(throttleHitByReason[GC_REASON_HEU].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(throttleHitByReason[GC_REASON_NATIVE].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(throttleHitByReason[GC_REASON_HEU_SYNC].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(throttleHitByReason[GC_REASON_NATIVE_SYNC].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(throttleHitByReason[GC_REASON_FORCE].load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(throttleHitByReason[GC_REASON_YOUNG].load(std::memory_order_relaxed)));
}

void GCStats::Init()
{
    isConcurrentMark = false;
    async = false;
    gcStartTime = TimeUtil::NanoSeconds();
    gcEndTime = TimeUtil::NanoSeconds();
    collectedObjects = 0;
    collectedBytes = 0;

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

    const char* jvmIhopEnv = std::getenv("MRT_GCV2_JVM_IHOP");
    const bool useJvmIhop = jvmIhopEnv != nullptr && std::strcmp(jvmIhopEnv, "1") == 0;
    size_t maxCapacity = Heap::GetHeap().GetMaxCapacity();
    if (useJvmIhop) {
        // Modern G1 uses 45% only as its initial IHOP; UpdateGCStats remains the adaptive controller here.
        constexpr size_t initialHeapOccupancyPercent = 45;
        heapThreshold = maxCapacity * initialHeapOccupancyPercent / 100;
    } else {
        // 20 MB:set 20 MB as intial value
        heapThreshold = std::min(CangjieRuntime::GetGCParam().gcThreshold, 20 * MB);
        // 0.2:set 20% heap size as intial value
        heapThreshold = std::min(static_cast<size_t>(maxCapacity * 0.2), heapThreshold);
    }
    VLOG(REPORT, "[GCV2][jvm-ihop] enabled=%d initial-threshold=%zu max-capacity=%zu adaptive-update=1",
         useJvmIhop, heapThreshold, maxCapacity);
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
