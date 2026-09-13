// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_STATS_H
#define MRT_STATS_H

#include <algorithm>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>

#include "Base/ImmortalWrapper.h"
#include "Base/Panic.h"
#include "GcRequest.h"

namespace MapleRuntime {
// statistics for previous gc.
class GCStats {
public:
    GCStats() = default;
    ~GCStats() = default;

    void Init();

    size_t GetThreshold() const { return heapThreshold.load(std::memory_order_acquire); }

    void Dump() const;

    static uint64_t GetPrevGCStartTime() { return prevGcStartTime.load(std::memory_order_acquire); }

    static void SetPrevGCStartTime(uint64_t timestamp)
    {
        prevGcStartTime.store(timestamp, std::memory_order_release);
    }

    static uint64_t GetPrevGCFinishTime() { return prevGcFinishTime.load(std::memory_order_acquire); }

    static void SetPrevGCFinishTime(uint64_t timestamp)
    {
        prevGcFinishTime.store(timestamp, std::memory_order_release);
    }

    void RecordMajorGCFinish(uint64_t timestamp)
    {
        RecordMajorGCFinish(timestamp, 0, 0, 0);
    }

    void RecordMajorGCFinish(uint64_t timestamp, uint64_t durationNs, size_t usedAfter,
                             size_t collectedBytes);


    static std::atomic<uint64_t> prevGcStartTime;
    static std::atomic<uint64_t> prevGcFinishTime;

    GCReason reason = GC_REASON_USER;
    bool isConcurrentMark;
    bool async;

    uint64_t gcStartTime;
    uint64_t gcEndTime;

    size_t liveBytesBeforeGC;
    size_t liveBytesAfterGC;

    size_t fromSpaceSize;
    size_t smallGarbageSize;

    size_t pinnedSpaceSize;
    size_t pinnedGarbageSize;

    size_t largeSpaceSize;
    size_t largeGarbageSize;

    size_t collectedBytes;
    size_t collectedObjects;

    // Young collection-set bytes and marked bytes that survive the minor.
    size_t youngCandidateBytes;
    size_t youngPromotedBytes;
    uint32_t tenuringThreshold;
    size_t liveByAge[16];

    double garbageRatio;
    double collectionRate; // bytes per nano-second

    std::atomic<size_t> heapThreshold{ 0 };


private:
    // A minor may extend the HEU finish-time throttle once after a major. A
    // second consecutive minor must leave the clock alone so a major cannot be
    // starved by a stream of young collections.
};
extern std::atomic<uint64_t> g_gcTotalTimeUs;
extern std::atomic<size_t> g_gcCollectedTotalBytes;
} // namespace MapleRuntime
#endif // MRT_STATS_H
