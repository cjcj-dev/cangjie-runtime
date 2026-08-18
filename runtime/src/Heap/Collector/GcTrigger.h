// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_TRIGGER_H
#define MRT_GC_TRIGGER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Product stays on occupancy + interval triggers until mutrelo closes.
// Flip to true only after wiring a sampler; do not add MRT_GCV2_* env.
constexpr bool kGcTriggerAllocRateEnabled = false;

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
};

struct GcTriggerInputs {
    double allocRateAvgBps = 0.0;
    double allocRatePredictBps = 0.0;
    double allocRateSdBps = 0.0;
    size_t usedBytes = 0;
    size_t youngUsedBytes = 0;
    size_t capacityBytes = 0;
    double lastGcDurationSec = 0.0;
    double timeSinceLastGcSec = 0.0;
    double collectionIntervalSec = 0.0;
    uint32_t warmupCyclesDone = 0;
    bool isWarm = false;
    bool isTimeTrustable = false;
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

inline double GcTriggerFreeBytes(const GcTriggerInputs& in)
{
    const size_t cap = in.capacityBytes;
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
    if (in.capacityBytes == 0) {
        return true;
    }
    const double pct = 100.0 * static_cast<double>(in.youngUsedBytes) / static_cast<double>(in.capacityBytes);
    return pct <= kGcTriggerYoungSmallPercent;
}

inline bool GcTriggerHighUsage(const GcTriggerInputs& in)
{
    if (in.capacityBytes == 0) {
        return true;
    }
    const double freePct = 100.0 * GcTriggerFreeBytes(in) / static_cast<double>(in.capacityBytes);
    return freePct <= kGcTriggerHighUsageFreePercent;
}

inline bool RuleTimer(const GcTriggerInputs& in)
{
    if (in.collectionIntervalSec <= 0.0) {
        return false;
    }
    return in.timeSinceLastGcSec >= in.collectionIntervalSec;
}

inline bool RuleWarmup(const GcTriggerInputs& in)
{
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

// zDirector.cpp:820-840 — major rules first (timer/warmup), then minor
// (alloc-rate before high-usage). Two hits: first match in that order wins.
inline GcTriggerDecision DecideGcTrigger(const GcTriggerInputs& in)
{
    if (RuleTimer(in)) {
        return { GcTriggerKind::MAJOR, GcTriggerRule::TIMER };
    }
    if (RuleWarmup(in)) {
        return { GcTriggerKind::MAJOR, GcTriggerRule::WARMUP };
    }
    if (RuleAllocRate(in)) {
        return { GcTriggerKind::MINOR, GcTriggerRule::ALLOC_RATE };
    }
    if (RuleHighUsage(in)) {
        return { GcTriggerKind::MINOR, GcTriggerRule::HIGH_USAGE };
    }
    if (GcTriggerYoungSmall(in) && GcTriggerHighUsage(in) && in.isTimeTrustable) {
        return { GcTriggerKind::MAJOR, GcTriggerRule::HIGH_USAGE };
    }
    return { GcTriggerKind::NONE, GcTriggerRule::NONE };
}

} // namespace MapleRuntime
#endif // MRT_GC_TRIGGER_H
