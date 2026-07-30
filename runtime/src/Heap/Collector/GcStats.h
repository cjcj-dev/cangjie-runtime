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

    size_t GetThreshold() const { return heapThreshold; }

    void Dump() const;

    static uint64_t GetPrevGCStartTime() { return prevGcStartTime; }

    static void SetPrevGCStartTime(uint64_t timestamp) { prevGcStartTime = timestamp; }

    static uint64_t GetPrevGCFinishTime() { return prevGcFinishTime; }

    static void SetPrevGCFinishTime(uint64_t timestamp) { prevGcFinishTime = timestamp; }

    static uint64_t GetPrevYoungGCFinishTime() { return prevYoungGcFinishTime; }

    static void SetPrevYoungGCFinishTime(uint64_t timestamp) { prevYoungGcFinishTime = timestamp; }

    static uint64_t prevGcStartTime;
    static uint64_t prevGcFinishTime;
    // Young throttling keeps its own clock. The shared prevGcFinishTime is armed only
    // by collections that actually executed as full (lastCollectionWasYoung, an
    // instance field below — requests and executions differ), so a stream of minors no
    // longer keeps resetting the clock that throttles heuristic/native full
    // collections. Fulls arm both clocks: they cover the young generation as well, so
    // an immediately following minor would mostly re-scan what was just collected.
    static uint64_t prevYoungGcFinishTime;

    GCReason reason;
    bool isConcurrentMark;
    bool async;
    // What the finished collection actually did, as opposed to what the reason asked
    // for: after majorInterval completed minors, the next GC_REASON_YOUNG request is
    // promoted to a full collection. The collector records the executed kind here at
    // the point where it makes that flow decision; the executor reads it afterwards so
    // the throttle clocks can follow it — a promoted full must arm the shared clock
    // like any other full.
    bool lastCollectionWasYoung = false;

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

    double garbageRatio;
    double collectionRate; // bytes per nano-second

    size_t heapThreshold;
};
extern size_t g_gcCount;
extern uint64_t g_gcTotalTimeUs;
extern size_t g_gcCollectedTotalBytes;
} // namespace MapleRuntime
#endif // MRT_STATS_H
