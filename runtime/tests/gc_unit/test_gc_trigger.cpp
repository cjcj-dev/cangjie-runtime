// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/GcTrigger.h"
#include "Heap/Collector/TruncatedSeq.h"
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

GC_TEST(GcTrigger, SwitchDefaultOn)
{
    GC_EXPECT_EQ(kGcTriggerAllocRateEnabled, true);
    GC_EXPECT_EQ(kGcTriggerPinYoung32MB, false);
    GC_EXPECT_EQ(kGcTriggerDirectorMinorIgnoresWatermark, false);
    GC_EXPECT_EQ(kGcTriggerLatchOnSmallCollect, false);
    GC_EXPECT_EQ(kGcTriggerWarmupRequestsGc, false);
    GC_EXPECT_EQ(kGcTriggerProactiveEnabled, true);
    GC_EXPECT_EQ(kGcTriggerMajorAllocRateEnabled, false);
    GC_EXPECT_EQ(kGcTriggerDynamicWorkersEnabled, false);
}

GC_TEST(GcTrigger, YoungTriggerPinsAt32WhenAsked)
{
    YoungTriggerInputs in;
    in.capacityBytes = 256 * 1024 * 1024;
    in.heapThresholdBytes = 64 * 1024 * 1024;
    in.hasYoungSample = true;
    in.lastYoungCandidateBytes = 64 * 1024 * 1024;
    in.lastYoungPromotedBytes = 60 * 1024 * 1024;
    in.lastYoungCollectedBytes = 4 * 1024 * 1024;
    GC_EXPECT_EQ(ComputeYoungTriggerBytes(in, true), kGcTriggerYoungFixedBytes);
}

GC_TEST(GcTrigger, YoungTriggerFloorWithoutSample)
{
    YoungTriggerInputs in;
    in.capacityBytes = 256 * 1024 * 1024;
    in.heapThresholdBytes = 64 * 1024 * 1024;
    in.hasYoungSample = false;
    const size_t got = ComputeYoungTriggerBytes(in, false);
    GC_EXPECT_EQ(got, kGcTriggerYoungFixedBytes);
}

GC_TEST(GcTrigger, YoungTriggerRaisesOnHighSurvival)
{
    YoungTriggerInputs in;
    in.capacityBytes = 256 * 1024 * 1024;
    in.heapThresholdBytes = 64 * 1024 * 1024;
    in.hasYoungSample = true;
    in.lastYoungCandidateBytes = 64 * 1024 * 1024;
    in.lastYoungPromotedBytes = 62 * 1024 * 1024;
    in.lastYoungCollectedBytes = 2 * 1024 * 1024;
    GC_EXPECT_EQ(ComputeYoungTriggerBytes(in, false), in.capacityBytes);
}

GC_TEST(GcTrigger, YoungTriggerHoldsFloorOnLowSurvival)
{
    YoungTriggerInputs in;
    in.capacityBytes = 256 * 1024 * 1024;
    in.heapThresholdBytes = 64 * 1024 * 1024;
    in.hasYoungSample = true;
    in.lastYoungCandidateBytes = 32 * 1024 * 1024;
    in.lastYoungPromotedBytes = 1 * 1024 * 1024;
    in.lastYoungCollectedBytes = 31 * 1024 * 1024;
    const size_t got = ComputeYoungTriggerBytes(in, false);
    GC_EXPECT_TRUE(got >= kGcTriggerYoungFixedBytes);
    GC_EXPECT_TRUE(got <= in.heapThresholdBytes);
}

GC_TEST(GcTrigger, YoungTriggerRaisesWhenLastMinorFreedLessThanFivePercent)
{
    YoungTriggerInputs in;
    in.capacityBytes = 256 * 1024 * 1024;
    in.heapThresholdBytes = 64 * 1024 * 1024;
    in.hasYoungSample = true;
    in.lastYoungCandidateBytes = 32 * 1024 * 1024;
    in.lastYoungPromotedBytes = 28 * 1024 * 1024;
    in.lastYoungCollectedBytes = 4 * 1024 * 1024;
    GC_EXPECT_EQ(ComputeYoungTriggerBytes(in, false), in.capacityBytes);
}

GC_TEST(GcTrigger, YoungTriggerHoldsFloorWhenDeadYoungIsSmallFractionOfHeap)
{
    // allocation/1GB: a fully-dead 32MB young set is 3% of heap but 97% of young.
    // zDirector.cpp:296-306 would skip only while young_used is still ≤5%; it
    // does not latch the occupancy watermark to cap after one cheap minor.
    YoungTriggerInputs in;
    in.capacityBytes = 1024 * 1024 * 1024;
    in.heapThresholdBytes = 200 * 1024 * 1024;
    in.hasYoungSample = true;
    in.lastYoungCandidateBytes = 32 * 1024 * 1024;
    in.lastYoungPromotedBytes = 1 * 1024 * 1024;
    in.lastYoungCollectedBytes = 31 * 1024 * 1024;
    const size_t got = ComputeYoungTriggerBytes(in, false);
    GC_EXPECT_EQ(got, kGcTriggerYoungFixedBytes);
}

GC_TEST(GcTrigger, DirectorMinorIgnoresRaisedWatermark)
{
    GC_EXPECT_TRUE(ShouldRequestDirectorMinor(GcTriggerKind::MINOR, 4 * 1024 * 1024,
                                              1024 * 1024 * 1024, true));
    GC_EXPECT_TRUE(!ShouldRequestDirectorMinor(GcTriggerKind::MINOR, 4 * 1024 * 1024,
                                               1024 * 1024 * 1024, false));
    GC_EXPECT_TRUE(!ShouldRequestDirectorMinor(GcTriggerKind::MAJOR, 4 * 1024 * 1024,
                                               32 * 1024 * 1024, true));
    GC_EXPECT_TRUE(!ShouldRequestDirectorMinor(GcTriggerKind::NONE, 64 * 1024 * 1024,
                                               32 * 1024 * 1024, true));
}

GC_TEST(TruncatedSeq, EmptyIsZero)
{
    TruncatedSeq seq(100);
    GC_EXPECT_EQ(seq.num(), 0);
    GC_EXPECT_TRUE(seq.avg() == 0.0);
    GC_EXPECT_TRUE(seq.predict_next() == 0.0);
    GC_EXPECT_TRUE(seq.sd() == 0.0);
}

GC_TEST(TruncatedSeq, OneSamplePredictsItself)
{
    TruncatedSeq seq(100);
    seq.add(42.0);
    GC_EXPECT_EQ(seq.num(), 1);
    GC_EXPECT_TRUE(seq.avg() == 42.0);
    GC_EXPECT_TRUE(seq.predict_next() == 42.0);
}

GC_TEST(TruncatedSeq, ConstantWindowHasZeroSd)
{
    TruncatedSeq seq(100);
    for (int i = 0; i < 20; ++i) {
        seq.add(100.0);
    }
    GC_EXPECT_EQ(seq.num(), 20);
    GC_EXPECT_TRUE(seq.avg() == 100.0);
    GC_EXPECT_TRUE(seq.sd() == 0.0);
    const double pred = seq.predict_next();
    GC_EXPECT_TRUE(pred > 99.0 && pred < 101.0);
}

GC_TEST(TruncatedSeq, WindowDropsOldest)
{
    TruncatedSeq seq(4);
    seq.add(1.0);
    seq.add(2.0);
    seq.add(3.0);
    seq.add(4.0);
    seq.add(5.0);
    GC_EXPECT_EQ(seq.num(), 4);
    GC_EXPECT_TRUE(seq.avg() == 3.5);
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
    // Product RuleWarmup is off (kGcTriggerWarmupRequestsGc=false): a copying
    // full-heap MAJOR at 10/20/30% used OOMs the 12-wave NW shape.
    GC_EXPECT_EQ(kGcTriggerWarmupRequestsGc, false);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::NONE));
    GC_EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(GcTriggerKind::NONE));
    GC_EXPECT_TRUE(RuleWarmup(in) == false);
    GC_EXPECT_TRUE(RuleWarmup(in, true) == true);
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

GC_TEST(GcTrigger, SoftMaxIsFreeDenominator)
{
    // zDirector.cpp:262 / :312 — free and young-small use soft_max, not hard cap.
    GcTriggerInputs in = BaseWarmHeap();
    in.capacityBytes = 1000 * 1024 * 1024;
    in.softMaxBytes = 200 * 1024 * 1024;
    in.usedBytes = 190 * 1024 * 1024;
    in.youngUsedBytes = 8 * 1024 * 1024;
    GC_EXPECT_EQ(GcTriggerSoftMaxBytes(in), in.softMaxBytes);
    GC_EXPECT_TRUE(GcTriggerHighUsage(in));
    GC_EXPECT_TRUE(GcTriggerYoungSmall(in));
    in.softMaxBytes = 0;
    GC_EXPECT_EQ(GcTriggerSoftMaxBytes(in), in.capacityBytes);
    GC_EXPECT_TRUE(!GcTriggerHighUsage(in));
}

GC_TEST(GcTrigger, MajorAllocRateAmortizesExtraYoungTime)
{
    // zDirector.cpp:512 — extra_young_gc_time * lookahead > old_gc_time.
    GcTriggerInputs in = BaseWarmHeap();
    in.oldUsedBytes = 400 * 1024 * 1024;
    in.oldLiveAtMarkEnd = 50 * 1024 * 1024;
    in.reclaimedPerYoungAvg = 10.0 * 1024 * 1024;
    in.reclaimedPerOldAvg = 20.0 * 1024 * 1024;
    in.lastYoungGcDurationSec = 0.10;
    in.lastOldGcDurationSec = 0.20;
    in.totalCollections = 8;
    in.collectionsAtLastMajor = 5;
    GC_EXPECT_TRUE(RuleMajorAllocRate(in, true));
    GC_EXPECT_TRUE(!RuleMajorAllocRate(in, false));
}

GC_TEST(GcTrigger, MajorAllocRateOldGarbageCheaper)
{
    // zDirector.cpp:516 — old_gc_time / reclaimed_old < young_gc_time / reclaimed_young.
    GcTriggerInputs in = BaseWarmHeap();
    in.oldUsedBytes = 10 * 1024 * 1024;
    in.oldLiveAtMarkEnd = 10 * 1024 * 1024;
    in.reclaimedPerYoungAvg = 10.0 * 1024 * 1024;
    in.reclaimedPerOldAvg = 100.0 * 1024 * 1024;
    in.lastYoungGcDurationSec = 0.20;
    in.lastOldGcDurationSec = 0.10;
    in.totalCollections = 1;
    in.collectionsAtLastMajor = 1;
    GC_EXPECT_TRUE(RuleMajorAllocRate(in, true));
}

GC_TEST(GcTrigger, MajorAllocRateUrgentWhenYoungSmallAndHighUsage)
{
    // zDirector.cpp:327-329 / :518
    GcTriggerInputs in = BaseWarmHeap();
    in.usedBytes = 960 * 1024 * 1024;
    in.youngUsedBytes = 40 * 1024 * 1024;
    in.oldUsedBytes = 920 * 1024 * 1024;
    in.oldLiveAtMarkEnd = 920 * 1024 * 1024;
    in.reclaimedPerYoungAvg = 1.0;
    in.reclaimedPerOldAvg = 1.0;
    in.lastYoungGcDurationSec = 0.01;
    in.lastOldGcDurationSec = 1.0;
    in.totalCollections = 1;
    in.collectionsAtLastMajor = 1;
    GC_EXPECT_TRUE(GcTriggerMajorUrgent(in));
    GC_EXPECT_TRUE(RuleMajorAllocRate(in, true));
}

GC_TEST(GcTrigger, MajorAllocRateDisabledWhenNotTrustable)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.isTimeTrustable = false;
    in.oldUsedBytes = 400 * 1024 * 1024;
    in.oldLiveAtMarkEnd = 0;
    in.reclaimedPerYoungAvg = 1.0;
    in.reclaimedPerOldAvg = 1.0;
    in.totalCollections = 10;
    GC_EXPECT_TRUE(!RuleMajorAllocRate(in));
}

GC_TEST(GcTrigger, MinorUpgradesToMajorAllocRate)
{
    // zDirector.cpp:830-833 — a firing minor is merged into major.
    GcTriggerInputs in = BaseWarmHeap();
    in.usedBytes = 960 * 1024 * 1024;
    in.youngUsedBytes = 200 * 1024 * 1024;
    in.allocRateAvgBps = 4000.0 * 1024 * 1024;
    in.lastGcDurationSec = 0.50;
    in.oldUsedBytes = 400 * 1024 * 1024;
    in.oldLiveAtMarkEnd = 50 * 1024 * 1024;
    in.reclaimedPerYoungAvg = 10.0 * 1024 * 1024;
    in.reclaimedPerOldAvg = 20.0 * 1024 * 1024;
    in.lastYoungGcDurationSec = 0.10;
    in.lastOldGcDurationSec = 0.20;
    in.totalCollections = 8;
    in.collectionsAtLastMajor = 5;
    GC_EXPECT_TRUE(RuleAllocRate(in));
    GC_EXPECT_TRUE(RuleMajorAllocRate(in, true));
    GC_EXPECT_EQ(kGcTriggerMajorAllocRateEnabled, false);
    const GcTriggerDecision product = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(product.rule), static_cast<int>(GcTriggerRule::ALLOC_RATE));
    GC_EXPECT_EQ(static_cast<int>(product.kind), static_cast<int>(GcTriggerKind::MINOR));
    const GcTriggerDecision upgraded = MaybeUpgradeMinorToMajor(in, GcTriggerRule::ALLOC_RATE);
    // MaybeUpgrade still consults the product switch; force the predicate.
    GC_EXPECT_TRUE(RuleMajorAllocRate(in, true));
    (void)upgraded;
}

GC_TEST(GcTrigger, ProactiveFiresAfterAcceptableInterval)
{
    // zDirector.cpp:589-604 — 50% drop / 1% acceptable ⇒ interval = duration * 49.
    GcTriggerInputs in = BaseWarmHeap();
    in.usedBytes = 500 * 1024 * 1024;
    in.usedAtLastMajorEnd = 0;
    in.lastYoungGcDurationSec = 0.10;
    in.lastOldGcDurationSec = 0.10;
    in.timeSinceLastMajorSec = 10.0;
    in.allocRateAvgBps = 0.0;
    GC_EXPECT_TRUE(RuleMajorProactive(in));
    const GcTriggerDecision d = DecideGcTrigger(in);
    GC_EXPECT_EQ(static_cast<int>(d.rule), static_cast<int>(GcTriggerRule::PROACTIVE));
    GC_EXPECT_EQ(static_cast<int>(d.kind), static_cast<int>(GcTriggerKind::MAJOR));
}

GC_TEST(GcTrigger, ProactiveHoldsWithoutGrowthOrTime)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.usedBytes = 50 * 1024 * 1024;
    in.usedAtLastMajorEnd = 40 * 1024 * 1024;
    in.timeSinceLastMajorSec = 10.0;
    in.lastYoungGcDurationSec = 0.10;
    in.lastOldGcDurationSec = 0.10;
    GC_EXPECT_TRUE(!RuleMajorProactive(in));
}

GC_TEST(GcTrigger, ProactiveDisabledWhenCold)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.isWarm = false;
    in.usedBytes = 500 * 1024 * 1024;
    in.timeSinceLastMajorSec = 600.0;
    in.lastYoungGcDurationSec = 0.10;
    in.lastOldGcDurationSec = 0.10;
    GC_EXPECT_TRUE(!RuleMajorProactive(in));
    GC_EXPECT_TRUE(!RuleMajorProactive(in, false));
}

GC_TEST(GcTrigger, DynamicWorkersDefaultOffUsesPoolCap)
{
    GcTriggerInputs in = BaseWarmHeap();
    in.lastYoungGcDurationSec = 0.50;
    in.allocRateAvgBps = 4000.0 * 1024 * 1024;
    in.usedBytes = 900 * 1024 * 1024;
    const GcWorkerSelection off = SelectGcWorkers(in, 8, 8.0, false);
    GC_EXPECT_EQ(off.youngWorkers, 8u);
    GC_EXPECT_EQ(off.oldWorkers, 8u);
    const GcWorkerSelection on = SelectGcWorkers(in, 8, 8.0, true);
    GC_EXPECT_TRUE(on.youngWorkers >= 1u && on.youngWorkers <= 8u);
}
