// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Collector/GcTrigger.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// Source ports of zMetronome.cpp:36-68 and zDirector.cpp:820-928.
// OpenJDK has no dedicated zMetronome/director gtest in the frozen reference.
GC_TEST(GcDirector, FixedOriginSurvivesProcessingTime)
{
    GcMetronome clock(100, 10);
    GC_EXPECT_TRUE(!clock.Poll(109));
    GC_EXPECT_TRUE(clock.Poll(110));
    GC_EXPECT_EQ(clock.DeadlineNs(), 120u);
    GC_EXPECT_TRUE(clock.Poll(123));
    GC_EXPECT_EQ(clock.DeadlineNs(), 130u);
}

GC_TEST(GcDirector, MissedTicksDoNotAccumulateCatchup)
{
    GcMetronome clock(100, 10);
    GC_EXPECT_TRUE(clock.Poll(147));
    GC_EXPECT_EQ(clock.DeadlineNs(), 150u);
    GC_EXPECT_TRUE(!clock.Poll(147));
    GC_EXPECT_TRUE(clock.Poll(150));
}

GC_TEST(GcDirector, CompletionWakeDoesNotMoveDeadline)
{
    GcMetronome clock(100, 10);
    GC_EXPECT_TRUE(clock.Poll(110));
    // Completion causes evaluation while Poll is false; no rebase to 115+10.
    GC_EXPECT_TRUE(!clock.Poll(115));
    GC_EXPECT_EQ(clock.DeadlineNs(), 120u);
    GC_EXPECT_TRUE(clock.Poll(120));
}

GC_TEST(GcDirector, TimerWithoutAllocations)
{
    GcTriggerInputs in;
    in.capacityBytes = 1000;
    in.collectionIntervalSec = 10;
    in.timeSinceLastMajorSec = 9;
    GC_EXPECT_EQ(static_cast<int>(DecideGcTrigger(in).kind), static_cast<int>(GcTriggerKind::NONE));
    in.timeSinceLastMajorSec = 10;
    GC_EXPECT_EQ(static_cast<int>(DecideGcTrigger(in).rule), static_cast<int>(GcTriggerRule::TIMER));
}

GC_TEST(GcDirector, ContinuousRequestsDoNotReplaceTicks)
{
    GcMetronome clock(100, 10);
    GcTriggerInputs in;
    in.capacityBytes = 1000;
    in.collectionIntervalSec = 1;
    in.timeSinceLastMajorSec = 2;
    in.majorBusy = true;
    in.minorBusy = true;
    for (uint64_t now = 101; now <= 149; ++now) {
        (void)DecideGcTrigger(in);
        (void)clock.Poll(now);
    }
    GC_EXPECT_EQ(clock.DeadlineNs(), 150u);
    in.majorBusy = false; // Driver ack, immediately reevaluated before tick.
    GC_EXPECT_EQ(static_cast<int>(DecideGcTrigger(in).rule), static_cast<int>(GcTriggerRule::TIMER));
    GC_EXPECT_EQ(clock.DeadlineNs(), 150u);
}

GC_TEST(GcDirector, HeadroomChangesHighUsageAndDeadline)
{
    GcTriggerInputs in;
    in.capacityBytes = 1000;
    in.usedBytes = 940;
    in.allocRateAvgBps = 10;
    const double before = GcTriggerTimeUntilOomSec(in);
    GC_EXPECT_TRUE(!GcTriggerHighUsage(in));
    in.relocationHeadroomBytes = 20;
    GC_EXPECT_EQ(GcTriggerFreeBytes(in), 40.0);
    GC_EXPECT_TRUE(GcTriggerHighUsage(in));
    GC_EXPECT_TRUE(GcTriggerTimeUntilOomSec(in) < before);
    in.relocationHeadroomBytes = 100;
    GC_EXPECT_EQ(GcTriggerFreeBytes(in), 0.0);
    in.usedBytes = 1100;
    GC_EXPECT_EQ(GcTriggerFreeBytes(in), 0.0);
}

GC_TEST(GcDirector, SmallHeapWorkerBudgetStaysBounded)
{
    GcTriggerInputs in;
    in.capacityBytes = 1000;
    in.usedBytes = 950;
    in.isWarm = false;
    const auto selection = SelectGcWorkers(in, 1, 1);
    GC_EXPECT_EQ(selection.youngWorkers, 1u);
    GC_EXPECT_EQ(selection.oldWorkers, 1u);
}

GC_TEST(GcDirector, ActiveMarkPressureRequestsMoreWorkers)
{
    GcTriggerInputs in;
    in.capacityBytes = 1000;
    in.usedBytes = 990;
    in.relocationHeadroomBytes = 5;
    in.isWarm = true;
    in.isTimeTrustable = true;
    in.allocRateAvgBps = 100;
    in.youngSerialTimeSec = 0.001;
    in.youngParallelTimeSec = 1;
    const auto pressure = RuleDynamicAllocRate(in, 8, 1, false);
    GC_EXPECT_TRUE(pressure.trigger);
    GC_EXPECT_EQ(pressure.workers, 8u);
    in.usedBytes = 0;
    in.allocRateAvgBps = 1;
    const auto relaxed = RuleDynamicAllocRate(in, 8, 1, false);
    GC_EXPECT_TRUE(!relaxed.trigger);
    GC_EXPECT_EQ(relaxed.workers, 1u);
}
