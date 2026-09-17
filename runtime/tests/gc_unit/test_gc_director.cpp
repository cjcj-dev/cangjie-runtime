// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/z/zDirector.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Base/TimeUtils.h"
#include "gc_unittest.hpp"

#include <chrono>
#include <cmath>
#include <thread>

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

// zStat.cpp:1242-1267: cycle wall time is split using the worker duration
// and accumulated worker time that ZStatWorkers recorded, never a fixed
// percentage. at_start(n)/at_end() are the ZWorkers::run brackets.
GC_TEST(GcDirector, CycleUsesWorkerAccountingAndControlledClock)
{
    ZStatCycle cycle;
    ZStatWorkers workers;
    cycle.Initialize(0);
    const uint64_t start = TimeUtil::NanoSeconds();
    cycle.AtStart(start);
    workers.at_start(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    workers.at_end();
    const auto recorded = workers.stats();
    GC_EXPECT_TRUE(recorded._accumulated_duration > 0.0);
    GC_EXPECT_TRUE(std::fabs(recorded._accumulated_time - 2.0 * recorded._accumulated_duration) < 0.000001);
    const uint64_t end = TimeUtil::NanoSeconds();
    cycle.AtEnd(end, &workers, true, true);
    const auto first = cycle.Stats(end + 1000000000);
    const double wall = static_cast<double>(end - start) / SECOND_TO_NANO_SECOND;
    GC_EXPECT_TRUE(std::fabs(first.serialTime - (wall - recorded._accumulated_duration)) < 0.000001);
    GC_EXPECT_TRUE(std::fabs(first.parallelTime - recorded._accumulated_time) < 0.000001);
    GC_EXPECT_TRUE(std::fabs(first.lastActiveWorkers - 2.0) < 0.000001);
    GC_EXPECT_EQ(first.timeSinceLast, 1.0);
    GC_EXPECT_EQ(first.warmupCycles, 1u);
    // at_end consumed the accumulation (get_and_reset), so the next cycle
    // starts from zero and an unrecorded cycle still resets it.
    const auto reset = workers.stats();
    GC_EXPECT_EQ(reset._accumulated_duration, 0.0);
    GC_EXPECT_EQ(reset._accumulated_time, 0.0);
    workers.at_start(3);
    workers.at_end();
    cycle.AtStart(end);
    cycle.AtEnd(end + 1, &workers, false, false);
    GC_EXPECT_EQ(workers.stats()._accumulated_duration, 0.0);
    const auto unrecorded = cycle.Stats(end + 2);
    GC_EXPECT_TRUE(std::fabs(unrecorded.parallelTime - recorded._accumulated_time) < 0.000001);
    GC_EXPECT_EQ(unrecorded.warmupCycles, 1u);
}

// zStat.cpp:1356-1377: stats() also counts the batch that is still running,
// weighted by its active worker count.
GC_TEST(GcDirector, WorkerStatsIncludeInFlightBatch)
{
    ZStatWorkers workers;
    GC_EXPECT_EQ(workers.stats()._accumulated_time, 0.0);
    workers.at_start(4);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto inFlight = workers.stats();
    GC_EXPECT_TRUE(inFlight._accumulated_duration > 0.0);
    GC_EXPECT_TRUE(std::fabs(inFlight._accumulated_time - 4.0 * inFlight._accumulated_duration) < 0.000001);
    workers.at_end();
    const auto done = workers.stats();
    GC_EXPECT_TRUE(done._accumulated_duration >= inFlight._accumulated_duration);
    GC_EXPECT_TRUE(std::fabs(done._accumulated_time - 4.0 * done._accumulated_duration) < 0.000001);
    GC_EXPECT_TRUE(std::fabs(workers.get_and_reset_duration() - done._accumulated_duration) < 0.000001);
    GC_EXPECT_TRUE(std::fabs(workers.get_and_reset_time() - done._accumulated_time) < 0.000001);
    GC_EXPECT_EQ(workers.stats()._accumulated_duration, 0.0);
}

GC_TEST(GcDirector, WarmupCountsOnlyWarmupRequests)
{
    ZStatCycle cycle;
    ZStatWorkers workers;
    cycle.Initialize(0);
    cycle.AtStart(1);
    cycle.AtEnd(2, &workers, false, true);
    GC_EXPECT_EQ(cycle.Stats(3).warmupCycles, 0u);
    for (uint64_t i = 0; i < 4; ++i) {
        cycle.AtStart(10 + 2 * i);
        cycle.AtEnd(11 + 2 * i, &workers, true, true);
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

// zGeneration.cpp:600-602,637,1248 and zDirector.cpp:495. These are
// source-level statistics/rule scenarios, not a driver integration test.
GC_TEST(GcDirector, CollectionCountsFollowYoungMarkStarts)
{
    ZStatCollection collections;
    ZStatCycle young;
    ZStatCycle old;
    young.Initialize(0);
    old.Initialize(0);
    collections.AtYoungMarkStart(false);
    const auto prior = collections.Stats();
    collections.AtYoungMarkStart(true);
    const auto combined = collections.Stats();
    GC_EXPECT_EQ(combined.totalCollections - prior.totalCollections, 1u);
    GC_EXPECT_EQ(combined.collectionsAtMajorStart, combined.totalCollections);

    ZStatWorkers youngWorkers;
    ZStatWorkers oldWorkers;
    young.AtStart(1);
    young.AtEnd(2, &youngWorkers, true, true);
    old.AtStart(2);
    old.AtEnd(3, &oldWorkers, true, true);
    const auto completed = collections.Stats();
    GC_EXPECT_EQ(completed.totalCollections, combined.totalCollections);
    GC_EXPECT_EQ(completed.collectionsAtMajorStart, combined.collectionsAtMajorStart);

    // The next young start counts immediately, before its completion.
    collections.AtYoungMarkStart(false);
    const auto next = collections.Stats();
    GC_EXPECT_EQ(next.totalCollections - completed.totalCollections, 1u);
    GC_EXPECT_EQ(next.collectionsAtMajorStart, completed.collectionsAtMajorStart);
    collections.AtYoungMarkStart(false);
    const auto second = collections.Stats();
    GC_EXPECT_EQ(second.totalCollections - next.totalCollections, 1u);
    GC_EXPECT_EQ(second.collectionsAtMajorStart, next.collectionsAtMajorStart);

    collections.AtYoungMarkStart(true);
    const auto nextMajor = collections.Stats();
    GC_EXPECT_EQ(nextMajor.totalCollections - second.totalCollections, 1u);
    GC_EXPECT_EQ(nextMajor.collectionsAtMajorStart, nextMajor.totalCollections);
}

GC_TEST(GcDirector, MajorRateLookaheadStartsAfterCombinedPause)
{
    ZStatCollection collections;
    GcTriggerInputs in;
    in.capacityBytes = 1000;
    in.usedBytes = 800;
    in.youngUsedBytes = 200;
    in.oldUsedBytes = 600;
    in.oldLiveAtMarkEnd = 100;
    in.isTimeTrustable = true;
    in.lastYoungGcDurationSec = 2;
    in.lastOldGcDurationSec = 3;
    in.reclaimedPerYoungAvg = 100;
    in.reclaimedPerOldAvg = 100;
    const auto sampleRule = [&] {
        const auto counts = collections.Stats();
        in.totalCollections = counts.totalCollections;
        in.collectionsAtLastMajor = counts.collectionsAtMajorStart;
        return RuleMajorAllocRate(in);
    };
    GC_EXPECT_TRUE(CalculateExtraYoungGcTime(in) > in.lastOldGcDurationSec);
    collections.AtYoungMarkStart(true);
    GC_EXPECT_TRUE(!sampleRule());
    // Sampling again at old completion must not count the old phase.
    GC_EXPECT_TRUE(!sampleRule());
    collections.AtYoungMarkStart(false);
    GC_EXPECT_TRUE(sampleRule());
    collections.AtYoungMarkStart(true);
    GC_EXPECT_TRUE(!sampleRule());
}

GC_TEST(GcDirector, MajorRateLookaheadUsesUnsignedCollectionDistance)
{
    GcTriggerInputs in;
    in.capacityBytes = 1000;
    in.youngUsedBytes = 200;
    in.oldUsedBytes = 600;
    in.oldLiveAtMarkEnd = 100;
    in.isTimeTrustable = true;
    in.lastYoungGcDurationSec = 2;
    in.lastOldGcDurationSec = 3;
    in.reclaimedPerYoungAvg = 100;
    in.reclaimedPerOldAvg = 100;
    in.collectionsAtLastMajor = std::numeric_limits<uint32_t>::max();
    in.totalCollections = in.collectionsAtLastMajor;
    GC_EXPECT_TRUE(!RuleMajorAllocRate(in));
    ++in.totalCollections;
    GC_EXPECT_TRUE(RuleMajorAllocRate(in));
}

// Source port: zGeneration.cpp:125-170,1212-1237. There is no dedicated
// zGeneration gtest in this reference; these exercise its owner invariant.
GC_TEST(GenerationState, IndependentPhaseSequenceAndWorkers)
{
    class Probe : public ZGeneration {
    public:
        using ZGeneration::ZGeneration;
        bool should_record_stats() override { return false; }
    };
    Probe young(ZGenerationId::young);
    Probe old(ZGenerationId::old);
    young.InitializeWorkers(2);
    old.InitializeWorkers(2);
    young.SelectReason(GC_REASON_YOUNG);
    young.Begin(1);
    young.PublishPhase(ZGenerationPhase::Mark);
    young.Workers()->set_active_workers(1);
    const auto before = young.Snapshot();

    old.SelectReason(GC_REASON_USER);
    old.Begin(2);
    old.PublishPhase(ZGenerationPhase::Relocate);
    old.Workers()->set_active_workers(2);

    const auto after = young.Snapshot();
    GC_EXPECT_EQ(after.sequence, before.sequence);
    GC_EXPECT_TRUE(after.phase == ZGenerationPhase::Mark);
    GC_EXPECT_EQ(after.reason, GC_REASON_YOUNG);
    GC_EXPECT_TRUE(after.active);
    GC_EXPECT_EQ(young.Workers()->active_workers(), 1u);
    GC_EXPECT_EQ(old.Workers()->active_workers(), 2u);
    GC_EXPECT_TRUE(&young.Stats() != &old.Stats());
    GC_EXPECT_TRUE(&young.CycleStats() != &old.CycleStats());

    old.End();
    GC_EXPECT_TRUE(young.Snapshot().active);
    young.End();
}

GC_TEST(GenerationState, FullPrecleanPromotesAllAndRootsComputeThreshold)
{
    class Probe : public ZGeneration {
    public:
        using ZGeneration::ZGeneration;
        bool should_record_stats() override { return false; }
    };
    Probe young(ZGenerationId::young);
    TenuringInputs inputs;
    inputs.softMaxCapacity = 64 * 1024 * 1024;
    inputs.youngAllocated = 4096;
    inputs.youngGarbage = 1024;
    inputs.liveByAge[1] = 1024;
    {
        YoungTypeSetter type(young, ZYoungType::major_full_preclean);
        young.SelectTenuringThreshold(inputs);
        GC_EXPECT_EQ(young.Stats().tenuringThreshold, 0u);
        GC_EXPECT_FALSE(young.IsMajorRoots());
    }
    GC_EXPECT_TRUE(young.YoungType() == ZYoungType::none);
    {
        YoungTypeSetter type(young, ZYoungType::major_full_roots);
        young.SelectTenuringThreshold(inputs);
        GC_EXPECT_TRUE(young.Stats().tenuringThreshold > 0u);
        GC_EXPECT_TRUE(young.IsMajorRoots());
    }
    GC_EXPECT_TRUE(young.YoungType() == ZYoungType::none);
}
