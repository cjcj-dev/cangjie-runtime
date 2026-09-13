// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Collector/GcTrigger.h"
#include "Base/ZStat.h"
#include "Heap/Allocator/AllocationStallQueue.h"
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

// zStat.cpp:1242-1267: cycle wall time is split using actual worker
// duration and accumulated worker time, never a fixed percentage.
GC_TEST(GcDirector, CycleUsesWorkerAccountingAndControlledClock)
{
    ZStatCycle cycle;
    cycle.Initialize(0);
    cycle.AtStart(1000000000, 0, 0);
    cycle.AtEnd(5000000000, 2000000000, 4000000000, true);
    const auto first = cycle.Stats(6000000000);
    GC_EXPECT_EQ(first.serialTime, 2.0);
    GC_EXPECT_EQ(first.parallelTime, 4.0);
    GC_EXPECT_EQ(first.lastActiveWorkers, 2.0);
    GC_EXPECT_EQ(first.timeSinceLast, 1.0);
    GC_EXPECT_EQ(first.warmupCycles, 1u);
    cycle.AtStart(6000000000, 2000000000, 4000000000);
    cycle.AtEnd(11000000000, 6000000000, 12000000000, false);
    const auto next = cycle.Stats(12000000000);
    GC_EXPECT_TRUE(std::fabs(next.serialTime - 1.3) < 0.000001);
    GC_EXPECT_TRUE(std::fabs(next.parallelTime - 6.8) < 0.000001);
    GC_EXPECT_TRUE(next.serialTimeSd > 0.0);
    GC_EXPECT_TRUE(next.parallelTimeSd > 0.0);
    GC_EXPECT_EQ(next.warmupCycles, 1u);
}

GC_TEST(GcDirector, WarmupCountsOnlyWarmupRequests)
{
    ZStatCycle cycle;
    cycle.Initialize(0);
    cycle.AtStart(1, 0, 0);
    cycle.AtEnd(2, 0, 0, false);
    GC_EXPECT_EQ(cycle.Stats(3).warmupCycles, 0u);
    for (uint64_t i = 0; i < 4; ++i) {
        cycle.AtStart(10 + 2 * i, 0, 0);
        cycle.AtEnd(11 + 2 * i, 0, 0, true);
    }
    GC_EXPECT_EQ(cycle.Stats(20).warmupCycles, 3u);
}

GC_TEST(GcDirector, StandaloneMinorDoesNotReserveInactiveOldBudget)
{
    GcTriggerInputs in;
    in.isTimeTrustable = true;
    in.lastYoungGcDurationSec = 1;
    in.lastOldGcDurationSec = 1;
    in.reclaimedPerYoungAvg = 1;
    in.reclaimedPerOldAvg = 8;
    const auto normal = SelectWorkerThreads(in, 1, 8, false);
    GC_EXPECT_EQ(normal.youngWorkers, 1u);
    in.majorBusy = true;
    const auto duringOld = SelectWorkerThreads(in, 1, 8, true);
    GC_EXPECT_EQ(duringOld.youngWorkers, 1u);
    GC_EXPECT_EQ(duringOld.oldWorkers, 7u);
}

GC_TEST(GcDirector, AllocationStallSnapshotUsesOutstandingRequests)
{
    std::mutex mutex;
    AllocationStallQueue queue(mutex);
    AllocationStallRequest request(4096, 0, true, true);
    GC_EXPECT_TRUE(!queue.IsStalling());
    {
        std::lock_guard<std::mutex> lock(mutex);
        queue.EnqueueLocked(request);
    }
    GC_EXPECT_TRUE(queue.IsStalling());
    queue.CompleteWave(queue.CaptureWaveBoundary());
    GC_EXPECT_TRUE(!queue.IsStalling());
}

GC_TEST(GcDirector, StallingBoostsAndRetainsActiveWorkerBudgets)
{
    GcTriggerInputs in;
    in.allocationStalling = true;
    const auto boost = SelectWorkerThreads(in, 1, 8, true);
    GC_EXPECT_EQ(boost.youngWorkers, 8u);
    GC_EXPECT_EQ(boost.oldWorkers, 8u);
    in.allocationStalling = false;
    in.activeYoungWorkers = 8;
    in.activeOldWorkers = 8;
    const auto retained = SelectWorkerThreads(in, 1, 8, true);
    GC_EXPECT_EQ(retained.youngWorkers, 8u);
    GC_EXPECT_EQ(retained.oldWorkers, 8u);
    in.activeYoungWorkers = 0;
    in.activeOldWorkers = 0;
    const auto idle = SelectWorkerThreads(in, 1, 8, false);
    GC_EXPECT_EQ(idle.youngWorkers, 1u);
    GC_EXPECT_EQ(idle.oldWorkers, 1u);
}
