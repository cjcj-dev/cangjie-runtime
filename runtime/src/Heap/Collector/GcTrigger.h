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

// zMetronome.cpp:36-68. The caller supplies a monotonic clock, so the
// same deadline arithmetic is usable by the director and deterministic tests.
class GcMetronome {
public:
    explicit GcMetronome(uint64_t startNs, uint64_t intervalNs = 10000000)
        : startNs(startNs), intervalNs(intervalNs) {}

    uint64_t DeadlineNs() const { return startNs + intervalNs * ticks; }

    bool Poll(uint64_t nowNs)
    {
        const uint64_t deadline = DeadlineNs();
        if (nowNs < deadline) {
            return false;
        }
        const uint64_t overslept = nowNs - deadline;
        if (overslept > intervalNs) {
            ticks += overslept / intervalNs;
        }
        ++ticks;
        return true;
    }

private:
    const uint64_t startNs;
    const uint64_t intervalNs;
    uint64_t ticks = 1;
};

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
    size_t relocationHeadroomBytes = 0;
    double lastGcDurationSec = 0.0;
    double youngSerialTimeSec = 0.0;
    double youngParallelTimeSec = 0.0;
    double lastYoungWorkers = 1.0;
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
    bool minorBusy = false;
    bool majorBusy = false;
    bool oldWorkersActive = false;
    uint32_t workerCapacity = 1;
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
    const size_t freeIncludingHeadroom = cap - used;
    return static_cast<double>(freeIncludingHeadroom -
                               std::min(freeIncludingHeadroom, in.relocationHeadroomBytes));
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
    // zDirector.cpp:426-434 — ZStat already supplies the sum of measured
    // serial and parallelizable time, each with its own variance margin.
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
    const double currentYoungGcTimePerBytesFreed = youngGcTime / reclaimedPerYoungGc;
    const double potentialYoungGcTimePerBytesFreed = youngGcTime / (reclaimedPerYoungGc + oldGarbage);
    if (currentYoungGcTimePerBytesFreed == std::numeric_limits<double>::infinity()) {
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
    return in.timeSinceLastMajorSec >= in.collectionIntervalSec;
}

inline bool RuleWarmup(const GcTriggerInputs& in)
{
    // zDirector.cpp:401-424: warmup is driven by old-cycle samples.
    if (in.isWarm || GcTriggerSoftMaxBytes(in) == 0) {
        return false;
    }
    const size_t threshold = static_cast<size_t>((in.warmupCyclesDone + 1) * 0.1 * GcTriggerSoftMaxBytes(in));
    return in.usedBytes >= threshold;
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

inline bool RuleMajorAllocRate(const GcTriggerInputs& in)
{
    // zDirector.cpp:470-519. Consumed as a minor-to-major upgrade
    // (zDirector.cpp:830-833), not as a standalone old-exhaustion timer.
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

inline bool RuleMajorProactive(const GcTriggerInputs& in)
{
    // zDirector.cpp:550-605
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

struct GcDynamicRequest {
    bool trigger;
    uint32_t workers;
};

// zDirector.cpp:147-216: soft/semi-hard use the sampled average, hard
// uses the existing prediction plus variance. No second rate estimator.
inline GcDynamicRequest RuleDynamicAllocRate(const GcTriggerInputs& in, uint32_t cap,
                                            double lastWorkers, bool conservative)
{
    if (!in.isTimeTrustable) {
        return {false, cap};
    }
    const double deviation = in.allocRateSdBps / (in.allocRateAvgBps + 1.0);
    const double rate = conservative ?
        std::max(in.allocRatePredictBps, in.allocRateAvgBps) * kGcTriggerSpikeTolerance +
            in.allocRateSdBps * kGcTriggerOneIn1000 + 1.0 : in.allocRateAvgBps;
    const double untilOom = (GcTriggerFreeBytes(in) / rate) / (1.0 + deviation);
    const uint32_t workers = DiscreteGcWorkers(SelectYoungGcWorkers(in, in.youngSerialTimeSec,
        in.youngParallelTimeSec, untilOom, cap, lastWorkers), cap);
    const double duration = in.youngSerialTimeSec + in.youngParallelTimeSec / workers;
    return {untilOom - duration <= untilOom * 0.05, workers};
}

// zDirector.cpp:521-548,682-722: allocate the existing concurrent budget
// according to each generation's reclaimed bytes per unit GC time.
inline GcWorkerSelection SelectWorkerThreads(const GcTriggerInputs& in, uint32_t youngWorkers,
                                              uint32_t cap, bool shareBudget)
{
    double ratio = 1.0;
    if (in.isTimeTrustable) {
        const double youngEfficiency = in.reclaimedPerYoungAvg / in.lastYoungGcDurationSec;
        const double oldEfficiency = in.reclaimedPerOldAvg / in.lastOldGcDurationSec;
        if (youngEfficiency == 0.0) {
            ratio = oldEfficiency == 0.0 ? 1.0 : cap;
        } else {
            ratio = std::min(oldEfficiency / youngEfficiency, static_cast<double>(cap));
        }
    }
    // Zero-time samples cannot order generation costs.
    if (!std::isfinite(ratio)) {
        ratio = 1.0;
    }
    const auto clamp = [cap](double count) {
        return static_cast<uint32_t>(std::max(1.0, std::min(count, static_cast<double>(cap))));
    };
    uint32_t oldWorkers = clamp(youngWorkers * ratio);
    if (shareBudget && oldWorkers + youngWorkers > cap) {
        const uint32_t youngClamped = clamp(cap / (1.0 + ratio));
        oldWorkers = std::max(1u, cap - youngClamped);
        youngWorkers = in.majorBusy ? youngClamped : std::max(oldWorkers, youngWorkers);
    }
    return {youngWorkers, oldWorkers};
}

inline GcWorkerSelection SelectGcWorkers(const GcTriggerInputs& in, uint32_t poolCap, double lastGcWorkers)
{
    const uint32_t cap = std::max(poolCap, 1u);
    GcTriggerInputs hard = in;
    hard.softMaxBytes = in.capacityBytes;
    const auto softRequest = RuleDynamicAllocRate(in, cap, lastGcWorkers, false);
    const auto hardRequest = RuleDynamicAllocRate(hard, cap, lastGcWorkers, true);
    return SelectWorkerThreads(in, std::max(softRequest.workers, hardRequest.workers), cap, true);
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

inline GcTriggerDecision DecideGcTrigger(const GcTriggerInputs& in)
{
    // zDirector.cpp:607-650,820-840: busy checks belong to each generation.
    if (!in.majorBusy) {
        if (RuleTimer(in)) {
            return {GcTriggerKind::MAJOR, GcTriggerRule::TIMER};
        }
        if (RuleWarmup(in)) {
            return {GcTriggerKind::MAJOR, GcTriggerRule::WARMUP};
        }
        if (RuleMajorProactive(in)) {
            return {GcTriggerKind::MAJOR, GcTriggerRule::PROACTIVE};
        }
    }
    if (in.minorBusy || (in.majorBusy && !in.oldWorkersActive)) {
        return {};
    }
    GcTriggerInputs hard = in;
    hard.softMaxBytes = in.capacityBytes;
    const bool allocationRate = !GcTriggerYoungSmall(in) &&
        (RuleDynamicAllocRate(in, in.workerCapacity, in.lastYoungWorkers, false).trigger ||
         RuleDynamicAllocRate(hard, in.workerCapacity, in.lastYoungWorkers, true).trigger);
    const GcTriggerRule minorRule = allocationRate ? GcTriggerRule::ALLOC_RATE :
        RuleHighUsage(in) ? GcTriggerRule::HIGH_USAGE : GcTriggerRule::NONE;
    if (minorRule != GcTriggerRule::NONE) {
        if (!in.majorBusy && RuleMajorAllocRate(in)) {
            return {GcTriggerKind::MAJOR, GcTriggerRule::MAJOR_ALLOC_RATE};
        }
        return {GcTriggerKind::MINOR, minorRule};
    }
    return {};
}

} // namespace MapleRuntime
#endif // MRT_GC_TRIGGER_H
