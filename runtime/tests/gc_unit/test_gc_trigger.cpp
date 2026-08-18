// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/GcTrigger.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

static GcTriggerInputs BaseWarmHeap()
{
    GcTriggerInputs in;
    in.capacityBytes = 1000 * 1024 * 1024;
    in.usedBytes = 400 * 1024 * 1024;
    in.youngUsedBytes = 200 * 1024 * 1024;
    in.lastGcDurationSec = 0.050;
    in.timeSinceLastGcSec = 0.2;
    in.isWarm = true;
    in.isTimeTrustable = true;
    in.warmupCyclesDone = 3;
    return in;
}

GC_TEST(GcTrigger, SwitchDefaultOff)
{
    GC_EXPECT_EQ(kGcTriggerAllocRateEnabled, false);
}

GC_TEST(GcTrigger, RateZeroDoesNotFireAllocRate)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.allocRateAvgBps = 0.0;
    in.allocRateSdBps = 0.0;
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::NONE));
    GC_EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(GcTriggerKind::NONE));
}

GC_TEST(GcTrigger, ConstantRateFiresWhenFreeCannotCoverGc)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.usedBytes = 900 * 1024 * 1024;
    in.youngUsedBytes = 200 * 1024 * 1024;
    in.allocRateAvgBps = 4000.0 * 1024 * 1024;
    in.allocRateSdBps = 0.0;
    in.lastGcDurationSec = 0.10;
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::ALLOC_RATE));
    GC_EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(GcTriggerKind::MINOR));
}

GC_TEST(GcTrigger, ConstantRateHoldsWhenFreeCoversGc)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.usedBytes = 200 * 1024 * 1024;
    in.allocRateAvgBps = 10.0 * 1024 * 1024;
    in.allocRateSdBps = 0.0;
    in.lastGcDurationSec = 0.010;
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::NONE));
}

GC_TEST(GcTrigger, RateSpikeFiresViaSigma)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.usedBytes = 850 * 1024 * 1024;
    in.youngUsedBytes = 200 * 1024 * 1024;
    in.allocRateAvgBps = 80.0 * 1024 * 1024;
    in.allocRateSdBps = 200.0 * 1024 * 1024;
    in.lastGcDurationSec = 0.20;
    const double timeUntil = GcTriggerTimeUntilOomSec(in);
    GC_EXPECT_TRUE(timeUntil <= in.lastGcDurationSec);
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::ALLOC_RATE));
}

GC_TEST(GcTrigger, RemainingCannotCoverOneGc)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.usedBytes = 980 * 1024 * 1024;
    in.youngUsedBytes = 100 * 1024 * 1024;
    in.allocRateAvgBps = 200.0 * 1024 * 1024;
    in.allocRateSdBps = 0.0;
    in.lastGcDurationSec = 1.0;
    GC_EXPECT_TRUE(GcTriggerTimeUntilOomSec(in) < in.lastGcDurationSec);
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::ALLOC_RATE));
    GC_EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(GcTriggerKind::MINOR));
}

GC_TEST(GcTrigger, WarmupAtTenPercent)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.isWarm = false;
    in.isTimeTrustable = false;
    in.warmupCyclesDone = 0;
    in.usedBytes = 100 * 1024 * 1024;
    in.allocRateAvgBps = 0.0;
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::WARMUP));
    GC_EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(GcTriggerKind::MAJOR));
}

GC_TEST(GcTrigger, WarmupBelowTenPercentSilent)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.isWarm = false;
    in.isTimeTrustable = false;
    in.warmupCyclesDone = 0;
    in.usedBytes = 50 * 1024 * 1024;
    in.youngUsedBytes = 50 * 1024 * 1024;
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::NONE));
}

GC_TEST(GcTrigger, TwoRulesTimerBeatsAllocRate)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.collectionIntervalSec = 1.0;
    in.timeSinceLastGcSec = 2.0;
    in.usedBytes = 980 * 1024 * 1024;
    in.youngUsedBytes = 200 * 1024 * 1024;
    in.allocRateAvgBps = 4000.0 * 1024 * 1024;
    in.lastGcDurationSec = 1.0;
    GC_EXPECT_TRUE(RuleTimer(in));
    GC_EXPECT_TRUE(RuleAllocRate(in));
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::TIMER));
    GC_EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(GcTriggerKind::MAJOR));
}

GC_TEST(GcTrigger, TwoRulesAllocRateBeatsHighUsage)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.usedBytes = 960 * 1024 * 1024;
    in.youngUsedBytes = 200 * 1024 * 1024;
    in.allocRateAvgBps = 4000.0 * 1024 * 1024;
    in.lastGcDurationSec = 0.50;
    GC_EXPECT_TRUE(RuleAllocRate(in));
    GC_EXPECT_TRUE(RuleHighUsage(in));
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::ALLOC_RATE));
    GC_EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(GcTriggerKind::MINOR));
}

GC_TEST(GcTrigger, HighUsageAlone)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.usedBytes = 960 * 1024 * 1024;
    in.youngUsedBytes = 200 * 1024 * 1024;
    in.allocRateAvgBps = 1.0;
    in.allocRateSdBps = 0.0;
    in.lastGcDurationSec = 0.001;
    GC_EXPECT_TRUE(RuleHighUsage(in));
    GC_EXPECT_TRUE(!RuleAllocRate(in));
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::HIGH_USAGE));
    GC_EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(GcTriggerKind::MINOR));
}

GC_TEST(GcTrigger, NotTrustableDisablesAllocRate)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.isTimeTrustable = false;
    in.usedBytes = 990 * 1024 * 1024;
    in.youngUsedBytes = 200 * 1024 * 1024;
    in.allocRateAvgBps = 8000.0 * 1024 * 1024;
    in.lastGcDurationSec = 1.0;
    GC_EXPECT_TRUE(!RuleAllocRate(in));
}
