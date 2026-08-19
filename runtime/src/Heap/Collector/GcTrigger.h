// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_TRIGGER_H
#define MRT_GC_TRIGGER_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "Base/Globals.h"
#include "Heap/Collector/GcTriggerFlags.h"

namespace MapleRuntime {

// L3: product consumes DecideGcTrigger. Flip false to restore occupancy+interval.
constexpr bool kGcTriggerAllocRateEnabled = true;
// L1 perturbation: pin the young watermark back to 32MB (zDirector still runs).
constexpr bool kGcTriggerPinYoung32MB = false;
// zDirector.cpp:331-363 / :365-381 — alloc-rate and high-usage re-evaluate
// every tick. Product keeps the occupancy watermark gate (gctrigger2): a
// high-survival young set still latches occupancy young off so HEU takes over.
// Flip true to let director MINOR ignore that latch (survival wall regresses).
constexpr bool kGcTriggerDirectorMinorIgnoresWatermark = false;
// gctrigger2 latched occupancy to cap when last collected ≤5% of heap.
// That treated a fully-dead 32MB young set on a 1GB heap as "not worth it".
// Product requires the young set itself to still be live. Flip true to restore
// the 1GB RSS regression.
constexpr bool kGcTriggerLatchOnSmallCollect = false;
// zDirector.cpp:401-424 warmup is a ZGC *old* collection (duration sample).
// Our MAJOR is a copying full-heap GC. Firing it at 10/20/30% used on the
// 12-wave NW shape OOMs, then ReclaimGarbageMemory walks a mixed garbage list
// and SEGV in DeleteRegionLocked. Product does not consume the warmup rule.
// Flip true only to restore the three early full GCs.
constexpr bool kGcTriggerWarmupRequestsGc = false;
constexpr size_t kGcTriggerYoungFixedBytes = 32 * MB;

// zDirector.cpp:39 — P(sample outside CI) ≈ 1/1000 for a normal.
constexpr double kGcTriggerOneIn1000 = 3.290527;
// z_globals.hpp:37 — unforeseen phase-change guard on moving-average rate.
constexpr double kGcTriggerSpikeTolerance = 2.0;
// zDirector.cpp:306 / :324 — young too small / free too low.
constexpr double kGcTriggerYoungSmallPercent = 5.0;
constexpr double kGcTriggerHighUsageFreePercent = 5.0;
// zDirector.cpp:417 — warmup at 10/20/30% of capacity.
constexpr double kGcTriggerWarmupStepPercent = 10.0;
constexpr uint32_t kGcTriggerWarmupCycles = 3;
// zDirector.cpp:576 / :580 — proactive enable gate.
constexpr double kGcTriggerProactiveUsedIncreasePercent = 10.0;
constexpr double kGcTriggerProactiveTimeThresholdSec = 5.0 * 60.0;
// zDirector.cpp:589-590
constexpr double kGcTriggerProactiveAssumedThroughputDrop = 0.50;
constexpr double kGcTriggerProactiveAcceptableThroughputDrop = 0.01;
// zDirector.cpp:132 — friction against lowering worker count too eagerly.
constexpr double kGcTriggerWorkerLoweringFriction = 0.5;

enum class GcTriggerKind : uint8_t {
    NONE = 0,
    MINOR,
    MAJOR,
};

enum class GcTriggerRule : uint8_t {
    NONE = 0,
    TIMER,
    WARMUP,
    ALLOC_RATE,
    HIGH_USAGE,
    MAJOR_ALLOC_RATE,
    PROACTIVE,
};

struct GcTriggerInputs {
    double allocRateAvgBps = 0.0;
    double allocRatePredictBps = 0.0;
    double allocRateSdBps = 0.0;
    size_t usedBytes = 0;
    size_t youngUsedBytes = 0;
    size_t oldUsedBytes = 0;
    size_t capacityBytes = 0;
    size_t softMaxBytes = 0;
    double lastGcDurationSec = 0.0;
    double lastYoungGcDurationSec = 0.0;
    double lastOldGcDurationSec = 0.0;
    double timeSinceLastGcSec = 0.0;
    double timeSinceLastMajorSec = 0.0;
    double collectionIntervalSec = 0.0;
    uint32_t warmupCyclesDone = 0;
    uint32_t totalCollections = 0;
    uint32_t collectionsAtLastMajor = 0;
    size_t usedAtLastMajorEnd = 0;
    size_t oldLiveAtMarkEnd = 0;
    double reclaimedPerYoungAvg = 0.0;
    double reclaimedPerOldAvg = 0.0;
    bool isWarm = false;
    bool isTimeTrustable = false;
};

struct GcWorkerSelection {
    uint32_t youngWorkers = 1;
    uint32_t oldWorkers = 1;
};

struct GcTriggerDecision {
    GcTriggerKind kind = GcTriggerKind::NONE;
    GcTriggerRule rule = GcTriggerRule::NONE;
};

struct YoungTriggerInputs {
    size_t capacityBytes = 0;
    size_t heapThresholdBytes = 0;
    size_t lastYoungCandidateBytes = 0;
    size_t lastYoungPromotedBytes = 0;
    size_t lastYoungCollectedBytes = 0;
    bool hasYoungSample = false;
};

// zDirector.cpp:331-363 — convert soft/hard minor rules into a young watermark.
// Soft: keep collecting while last minor reclaimed enough. Hard: if survival is
// high, raise the line to the HEU budget so young/HEU stop interleaving.
inline size_t ComputeYoungTriggerBytes(const YoungTriggerInputs& in, bool pin32 = kGcTriggerPinYoung32MB)
{
    if (pin32) {
        return kGcTriggerYoungFixedBytes;
    }
    if (in.capacityBytes == 0) {
        return kGcTriggerYoungFixedBytes;
    }
    const size_t youngSmall =
        static_cast<size_t>(static_cast<double>(in.capacityBytes) * kGcTriggerYoungSmallPercent / 100.0);
    // Occupancy floor stays at 32MB. zDirector.cpp:296-306 uses 5% as a now-gate
    // on the current young set, not as the next occupancy line. max(5% heap, 32MB)
    // raised the 1GB line to 51MB and left RSS at 1.23× after the latch fix.
    size_t floorBytes = kGcTriggerYoungFixedBytes;
    size_t ceilingBytes = in.heapThresholdBytes == 0 ? in.capacityBytes : in.heapThresholdBytes;
    if (ceilingBytes < floorBytes) {
        ceilingBytes = floorBytes;
    }
    if (!in.hasYoungSample || in.lastYoungCandidateBytes == 0) {
        return floorBytes;
    }
    const double survival = static_cast<double>(in.lastYoungPromotedBytes) /
        static_cast<double>(in.lastYoungCandidateBytes);
    // zDirector.cpp:296-306 is a NOW gate (young_used <= 5% of heap → skip this
    // minor), not a latch. Raising the occupancy watermark to cap when last
    // collected <= 5% of heap latched young off on allocation/1GB: a fully-dead
    // 32MB young set is 3% of 1GB but ~100% of young. Only latch that occupancy
    // line when the young set itself came back at least 5% live — then another
    // floor-sized minor cannot be expected to free 5% of the heap
    // (zDirector.cpp:303-306). Fully-dead young stays on the floor / 5% line.
    constexpr double highSurvival = 1.0 - (kGcTriggerYoungSmallPercent / 100.0);
    constexpr double stillLive = kGcTriggerYoungSmallPercent / 100.0;
    const bool smallCollect = in.lastYoungCollectedBytes <= youngSmall;
    if (survival >= highSurvival ||
        (smallCollect && (kGcTriggerLatchOnSmallCollect || survival >= stillLive))) {
        return in.capacityBytes;
    }
    const double reclaim = 1.0 - survival;
    size_t adaptive = in.lastYoungCollectedBytes;
    if (reclaim > 0.0) {
        adaptive = static_cast<size_t>(static_cast<double>(in.lastYoungCollectedBytes) / reclaim);
    }
    adaptive = std::max(adaptive, floorBytes);
    return std::min(adaptive, ceilingBytes);
}

inline double GcTriggerMaxAllocRateBps(const GcTriggerInputs& in)
{
    const double avg = in.allocRateAvgBps;
    const double sd = in.allocRateSdBps;
    return (avg * kGcTriggerSpikeTolerance) + (sd * kGcTriggerOneIn1000);
}

inline size_t GcTriggerSoftMaxBytes(const GcTriggerInputs& in)
{
    // zDirector.cpp:262 / :312 — soft_max_heap_size is the free-denominator.
    // Missing/zero env falls back to hard capacity (cjSoftMaxHeapSize default).
    if (in.softMaxBytes == 0) {
        return in.capacityBytes;
    }
    return std::min(in.softMaxBytes, in.capacityBytes);
}

inline double GcTriggerFreeBytes(const GcTriggerInputs& in)
{
    const size_t cap = GcTriggerSoftMaxBytes(in);
    const size_t used = std::min(in.usedBytes, cap);
    return static_cast<double>(cap - used);
}

inline double GcTriggerTimeUntilOomSec(const GcTriggerInputs& in)
{
    // zDirector.cpp:274-275 — +1 B/s avoids div-by-zero when rate is 0.
    return GcTriggerFreeBytes(in) / (GcTriggerMaxAllocRateBps(in) + 1.0);
}

inline bool GcTriggerYoungSmall(const GcTriggerInputs& in)
{
    // zDirector.cpp:296-306 — percent_of(young_used, soft_max_capacity).
    const size_t cap = GcTriggerSoftMaxBytes(in);
    if (cap == 0) {
        return true;
    }
    const double pct = 100.0 * static_cast<double>(in.youngUsedBytes) / static_cast<double>(cap);
    return pct <= kGcTriggerYoungSmallPercent;
}

inline bool GcTriggerHighUsage(const GcTriggerInputs& in)
{
    // zDirector.cpp:309-324 — free vs soft_max_capacity.
    const size_t cap = GcTriggerSoftMaxBytes(in);
    if (cap == 0) {
        return true;
    }
    const double freePct = 100.0 * GcTriggerFreeBytes(in) / static_cast<double>(cap);
    return freePct <= kGcTriggerHighUsageFreePercent;
}

inline bool GcTriggerMajorUrgent(const GcTriggerInputs& in)
{
    // zDirector.cpp:327-329
    return GcTriggerYoungSmall(in) && GcTriggerHighUsage(in);
}

inline double GcTriggerGcTimeSec(double durationSec)
{
    // zDirector.cpp:426-434 — serial + parallelizable, both + 3.3 sigma.
    // We only have a single wall sample; treat it as the serial+parallel sum.
    return durationSec;
}

inline double CalculateExtraYoungGcTime(const GcTriggerInputs& in)
{
    // zDirector.cpp:436-468
    if (!in.isTimeTrustable) {
        return 0.0;
    }
    const size_t oldUsed = in.oldUsedBytes;
    const size_t oldLive = std::min(in.oldLiveAtMarkEnd, oldUsed);
    const double oldGarbage = static_cast<double>(oldUsed - oldLive);
    const double youngGcTime = GcTriggerGcTimeSec(in.lastYoungGcDurationSec);
    const double reclaimedPerYoungGc = in.reclaimedPerYoungAvg;
    if (reclaimedPerYoungGc <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double currentYoungGcTimePerBytesFreed = youngGcTime / reclaimedPerYoungGc;
    const double potentialYoungGcTimePerBytesFreed = youngGcTime / (reclaimedPerYoungGc + oldGarbage);
    if (!std::isfinite(currentYoungGcTimePerBytesFreed)) {
        return std::numeric_limits<double>::infinity();
    }
    const double extraYoungGcTimePerBytesFreed =
        currentYoungGcTimePerBytesFreed - potentialYoungGcTimePerBytesFreed;
    return extraYoungGcTimePerBytesFreed * (reclaimedPerYoungGc + oldGarbage);
}

inline bool RuleTimer(const GcTriggerInputs& in)
{
    if (in.collectionIntervalSec <= 0.0) {
        return false;
    }
    return in.timeSinceLastGcSec >= in.collectionIntervalSec;
}

inline bool RuleWarmup(const GcTriggerInputs& in, bool requestGc = kGcTriggerWarmupRequestsGc)
{
    // zDirector.cpp:401-424. Product does not consume this as a copying full GC.
    if (!requestGc) {
        return false;
    }
    if (in.isWarm || in.capacityBytes == 0) {
        return false;
    }
    if (in.warmupCyclesDone >= kGcTriggerWarmupCycles) {
        return false;
    }
    const double usedThresholdPct = (static_cast<double>(in.warmupCyclesDone) + 1.0) * kGcTriggerWarmupStepPercent;
    const double usedPct = 100.0 * static_cast<double>(in.usedBytes) / static_cast<double>(in.capacityBytes);
    return usedPct >= usedThresholdPct;
}

inline bool RuleAllocRate(const GcTriggerInputs& in)
{
    if (!in.isTimeTrustable) {
        return false;
    }
    if (GcTriggerYoungSmall(in)) {
        return false;
    }
    const double timeUntilGc = GcTriggerTimeUntilOomSec(in) - in.lastGcDurationSec;
    return timeUntilGc <= 0.0;
}

inline bool RuleHighUsage(const GcTriggerInputs& in)
{
    if (GcTriggerYoungSmall(in)) {
        return false;
    }
    return GcTriggerHighUsage(in);
}

inline bool RuleMajorAllocRate(const GcTriggerInputs& in, bool enabled = kGcTriggerMajorAllocRateEnabled)
{
    // zDirector.cpp:470-519. Consumed as a minor-to-major upgrade
    // (zDirector.cpp:830-833), not as a standalone old-exhaustion timer.
    if (!enabled) {
        return false;
    }
    if (!in.isTimeTrustable) {
        return false;
    }
    const double oldGcTime = GcTriggerGcTimeSec(in.lastOldGcDurationSec);
    const double youngGcTime = GcTriggerGcTimeSec(in.lastYoungGcDurationSec);
    const double reclaimedPerYoungGc = in.reclaimedPerYoungAvg;
    const double reclaimedPerOldGc = in.reclaimedPerOldAvg;
    const double extraYoungGcTime = CalculateExtraYoungGcTime(in);
    const uint32_t lookahead = in.totalCollections >= in.collectionsAtLastMajor ?
        in.totalCollections - in.collectionsAtLastMajor : 0;
    const double extraYoungGcTimeForLookahead = extraYoungGcTime * static_cast<double>(lookahead);
    const bool canAmortizeTimeCost = extraYoungGcTimeForLookahead > oldGcTime;
    // zDirector.cpp:485-516 — 0/0 is NaN (false); young 0 / old >0 is +inf (true).
    const double currentYoungGcTimePerBytesFreed = youngGcTime / reclaimedPerYoungGc;
    const double currentOldGcTimePerBytesFreed = oldGcTime / reclaimedPerOldGc;
    const bool oldGarbageIsCheaper = currentOldGcTimePerBytesFreed < currentYoungGcTimePerBytesFreed;
    return canAmortizeTimeCost || oldGarbageIsCheaper || GcTriggerMajorUrgent(in);
}

inline bool RuleMajorProactive(const GcTriggerInputs& in, bool enabled = kGcTriggerProactiveEnabled)
{
    // zDirector.cpp:550-605
    if (!enabled) {
        return false;
    }
    if (!in.isWarm) {
        return false;
    }
    const size_t usedIncreaseThreshold =
        static_cast<size_t>(static_cast<double>(GcTriggerSoftMaxBytes(in)) *
                            (kGcTriggerProactiveUsedIncreasePercent / 100.0));
    const size_t usedThreshold = in.usedAtLastMajorEnd + usedIncreaseThreshold;
    if (in.usedBytes < usedThreshold && in.timeSinceLastMajorSec < kGcTriggerProactiveTimeThresholdSec) {
        return false;
    }
    const double serialGcTime = GcTriggerGcTimeSec(in.lastOldGcDurationSec) +
        GcTriggerGcTimeSec(in.lastYoungGcDurationSec);
    const double gcDuration = serialGcTime;
    if (gcDuration <= 0.0) {
        // ZGC is warm only after sampled cycles; a zero duration makes the
        // acceptable interval 0 and would fire every tick.
        return false;
    }
    const double acceptableGcInterval =
        gcDuration * ((kGcTriggerProactiveAssumedThroughputDrop / kGcTriggerProactiveAcceptableThroughputDrop) - 1.0);
    const double timeUntilGc = acceptableGcInterval - in.timeSinceLastMajorSec;
    return timeUntilGc <= 0.0;
}

inline double EstimatedGcWorkers(double serialGcTime, double parallelizableGcTime, double timeUntilDeadline)
{
    // zDirector.cpp:100-103
    const double parallelizableTimeUntilDeadline = std::max(timeUntilDeadline - serialGcTime, 0.001);
    return parallelizableGcTime / parallelizableTimeUntilDeadline;
}

inline uint32_t DiscreteGcWorkers(double gcWorkers, uint32_t poolCap)
{
    // zDirector.cpp:105-107 — clamp(ceil(gc_workers), 1, ZYoungGCThreads)
    const uint32_t cap = poolCap == 0 ? 1 : poolCap;
    if (!std::isfinite(gcWorkers) || gcWorkers >= static_cast<double>(cap)) {
        return cap;
    }
    const uint32_t want = static_cast<uint32_t>(std::ceil(std::max(gcWorkers, 1.0)));
    if (want < 1) {
        return 1;
    }
    return std::min(want, cap);
}

inline double SelectYoungGcWorkers(const GcTriggerInputs& in, double serialGcTime, double parallelizableGcTime,
                                   double timeUntilOom, uint32_t poolCap, double lastGcWorkers)
{
    // zDirector.cpp:109-145
    if (!in.isWarm) {
        return static_cast<double>(poolCap == 0 ? 1 : poolCap);
    }
    const double gcWorkers = EstimatedGcWorkers(serialGcTime, parallelizableGcTime, timeUntilOom);
    const uint32_t actualGcWorkers = DiscreteGcWorkers(gcWorkers, poolCap);
    if (static_cast<double>(actualGcWorkers) < lastGcWorkers && lastGcWorkers > 0.0) {
        const double gcDurationDelta =
            (parallelizableGcTime / static_cast<double>(actualGcWorkers)) - (parallelizableGcTime / lastGcWorkers);
        const double additionalTimeForAllocations = in.timeSinceLastGcSec - gcDurationDelta;
        const double nextTimeUntilOom = timeUntilOom + additionalTimeForAllocations;
        const double nextAvoidOomGcWorkers =
            EstimatedGcWorkers(serialGcTime, parallelizableGcTime, nextTimeUntilOom);
        const double nextGcWorkers = nextAvoidOomGcWorkers + kGcTriggerWorkerLoweringFriction;
        const double lo = static_cast<double>(actualGcWorkers);
        const double hi = lastGcWorkers;
        if (nextGcWorkers < lo) {
            return lo;
        }
        if (nextGcWorkers > hi) {
            return hi;
        }
        return nextGcWorkers;
    }
    return gcWorkers;
}

inline GcWorkerSelection SelectGcWorkers(const GcTriggerInputs& in, uint32_t poolCap, double lastGcWorkers,
                                         bool enabled = kGcTriggerDynamicWorkersEnabled)
{
    // zDirector.cpp:783-793 initial_workers — soft/hard minor-alloc-rate workers.
    GcWorkerSelection out;
    const uint32_t cap = poolCap == 0 ? 1 : poolCap;
    out.youngWorkers = cap;
    out.oldWorkers = cap;
    if (!enabled) {
        return out;
    }
    const double serialGcTime = in.lastYoungGcDurationSec * 0.25;
    const double parallelizableGcTime = in.lastYoungGcDurationSec * 0.75;
    const double timeUntilOom = GcTriggerTimeUntilOomSec(in);
    const double workers =
        SelectYoungGcWorkers(in, serialGcTime, parallelizableGcTime, timeUntilOom, cap, lastGcWorkers);
    out.youngWorkers = DiscreteGcWorkers(workers, cap);
    out.oldWorkers = out.youngWorkers;
    return out;
}

// zDirector.cpp:820-840 — major rules first (timer/warmup), then minor
// (alloc-rate before high-usage). Two hits: first match in that order wins.
extern std::atomic<uint64_t> g_gcTriggerArmed;
extern std::atomic<uint64_t> g_gcTriggerTurned;
extern std::atomic<uint64_t> g_gcTriggerRuleTimer;
extern std::atomic<uint64_t> g_gcTriggerRuleWarmup;
extern std::atomic<uint64_t> g_gcTriggerRuleAllocRate;
extern std::atomic<uint64_t> g_gcTriggerRuleHighUsage;
extern std::atomic<uint64_t> g_gcTriggerRuleMajorAllocRateArmed;
extern std::atomic<uint64_t> g_gcTriggerRuleMajorAllocRate;
extern std::atomic<uint64_t> g_gcTriggerRuleProactiveArmed;
extern std::atomic<uint64_t> g_gcTriggerRuleProactive;

inline void NoteGcTriggerRule(GcTriggerRule rule)
{
    switch (rule) {
        case GcTriggerRule::TIMER:
            g_gcTriggerRuleTimer.fetch_add(1, std::memory_order_relaxed);
            break;
        case GcTriggerRule::WARMUP:
            g_gcTriggerRuleWarmup.fetch_add(1, std::memory_order_relaxed);
            break;
        case GcTriggerRule::ALLOC_RATE:
            g_gcTriggerRuleAllocRate.fetch_add(1, std::memory_order_relaxed);
            break;
        case GcTriggerRule::HIGH_USAGE:
            g_gcTriggerRuleHighUsage.fetch_add(1, std::memory_order_relaxed);
            break;
        case GcTriggerRule::MAJOR_ALLOC_RATE:
            g_gcTriggerRuleMajorAllocRate.fetch_add(1, std::memory_order_relaxed);
            break;
        case GcTriggerRule::PROACTIVE:
            g_gcTriggerRuleProactive.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

inline GcTriggerDecision MaybeUpgradeMinorToMajor(const GcTriggerInputs& in, GcTriggerRule minorRule)
{
    // zDirector.cpp:830-833 — merge minor into major when rule_major_allocation_rate.
    if constexpr (kGcTriggerMajorAllocRateEnabled) {
        g_gcTriggerRuleMajorAllocRateArmed.fetch_add(1, std::memory_order_relaxed);
        if (RuleMajorAllocRate(in)) {
            return { GcTriggerKind::MAJOR, GcTriggerRule::MAJOR_ALLOC_RATE };
        }
    }
    return { GcTriggerKind::MINOR, minorRule };
}

inline GcTriggerDecision DecideGcTrigger(const GcTriggerInputs& in)
{
    // zDirector.cpp:820-840 — major (timer/warmup/proactive) first, then minor,
    // then maybe upgrade the minor via rule_major_allocation_rate.
    if (RuleTimer(in)) {
        return { GcTriggerKind::MAJOR, GcTriggerRule::TIMER };
    }
    if (RuleWarmup(in)) {
        return { GcTriggerKind::MAJOR, GcTriggerRule::WARMUP };
    }
    if constexpr (kGcTriggerProactiveEnabled) {
        g_gcTriggerRuleProactiveArmed.fetch_add(1, std::memory_order_relaxed);
        if (RuleMajorProactive(in)) {
            return { GcTriggerKind::MAJOR, GcTriggerRule::PROACTIVE };
        }
    }
    if (RuleAllocRate(in)) {
        return MaybeUpgradeMinorToMajor(in, GcTriggerRule::ALLOC_RATE);
    }
    if (RuleHighUsage(in)) {
        return MaybeUpgradeMinorToMajor(in, GcTriggerRule::HIGH_USAGE);
    }
    if (GcTriggerYoungSmall(in) && GcTriggerHighUsage(in) && in.isTimeTrustable) {
        return { GcTriggerKind::MAJOR, GcTriggerRule::HIGH_USAGE };
    }
    return { GcTriggerKind::NONE, GcTriggerRule::NONE };
}

// zDirector.cpp:331-381 — director minor is not gated by a raised watermark.
// The occupancy young path in TakeRegion still uses the watermark.
inline bool ShouldRequestDirectorMinor(GcTriggerKind kind, size_t youngAllocated, size_t watermark,
                                       bool ignoreWatermark = kGcTriggerDirectorMinorIgnoresWatermark)
{
    if (kind != GcTriggerKind::MINOR) {
        return false;
    }
    if (ignoreWatermark) {
        return true;
    }
    return youngAllocated >= watermark;
}

} // namespace MapleRuntime
#endif // MRT_GC_TRIGGER_H
