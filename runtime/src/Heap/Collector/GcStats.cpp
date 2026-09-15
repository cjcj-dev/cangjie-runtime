// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "GcStats.h"

#include "Base/GcLog.h"
#include "Base/LogFile.h"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zHeap.hpp"

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



void GCStats::RecordMajorGCFinish(uint64_t timestamp, uint64_t, size_t, size_t)
{
    SetPrevGCFinishTime(timestamp);
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
