// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_worker_fixture.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zWorkers.hpp"
#include "gc_unittest.hpp"
#include "b09_runtime_fixture.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
MarkStackEntry Entry(size_t i)
{
    return MarkStackEntry(size_t(i + 1), size_t(i + 3), false);
}

void DrainFollow(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, MarkTerminate& terminate,
                 size_t workerId, std::vector<size_t>& seen, bool partial)
{
    MapleRuntime::GcUnit::WorkerFixture workerThread(workerId);
    (void)ZMark::FollowWork(context, smr, stripes, terminate, workerId, partial,
                                 [&seen](const MarkStackEntry& entry) {
                                     seen.push_back(entry.partial_array_offset());
                                 });
}
} // namespace

GC_TEST(MarkPort203Engine, SingleAndTwoWorkersDrainSamePublishedSet)
{
    for (size_t workers : {size_t{1}, size_t{2}}) {
        MarkStripeSet stripes(4);
        MarkTerminate terminate;
        terminate.Reset(workers);
        stripes.SetTerminate(&terminate);
        MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
        MarkThreadLocalStacks seed(4);
        constexpr size_t count = 40;
        for (size_t i = 0; i < count; ++i) {
            seed.Push(stripes, i % 4, Entry(i), true);
        }
        (void)seed.Flush(stripes, true);
        std::vector<std::vector<size_t>> seen(workers);
        std::vector<std::unique_ptr<MarkThreadLocalStacks>> stacks;
        std::vector<std::unique_ptr<MarkContext>> contexts;
        for (size_t w = 0; w < workers; ++w) {
            stacks.emplace_back(std::make_unique<MarkThreadLocalStacks>(4));
            contexts.emplace_back(std::make_unique<MarkContext>(workers, w, stripes, *stacks[w]));
        }
        std::vector<std::thread> threads;
        for (size_t w = 1; w < workers; ++w) {
            threads.emplace_back([&, w]() {
                DrainFollow(*contexts[w], smr, stripes, terminate, w, seen[w], false);
            });
        }
        DrainFollow(*contexts[0], smr, stripes, terminate, 0, seen[0], false);
        for (auto& t : threads) {
            t.join();
        }
        GC_EXPECT_TRUE(terminate.Terminated());
        size_t total = 0;
        for (size_t w = 0; w < workers; ++w) {
            total += seen[w].size();
        }
        GC_EXPECT_EQ(total, count);
        GC_EXPECT_TRUE(stripes.IsEmpty());
    }
}

GC_TEST(MarkPort203Engine, StealLocalBeforeGlobal)
{
    MarkStripeSet stripes(2);
    MarkTerminate terminate;
    terminate.Reset(1);
    stripes.SetTerminate(&terminate);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkThreadLocalStacks stacks(2);
    MarkContext context(1, 0, stripes, stacks);
    context.SetStripeId(0);
    MarkStripeStack* localVictim = MarkStripeStack::Create(true);
    localVictim->Push(Entry(11));
    context.Stacks().Install(1, localVictim);
    MarkStripeStack* global = MarkStripeStack::Create(true);
    global->Push(Entry(22));
    stripes.At(1).PublishStack(global, true, stripes.Terminate());
    std::vector<size_t> seen;
    DrainFollow(context, smr, stripes, terminate, 0, seen, false);
    GC_EXPECT_EQ(seen.size(), 2u);
    GC_EXPECT_EQ(seen[0], 12u);
    GC_EXPECT_EQ(seen[1], 23u);
}

GC_TEST(MarkPort203Engine, OverflowPreferredOverPublished)
{
    MarkStripeSet stripes(1);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkStripeStack* overflow = MarkStripeStack::Create(true);
    overflow->Push(Entry(1));
    MarkStripeStack* published = MarkStripeStack::Create(true);
    published->Push(Entry(2));
    stripes.At(0).PublishStack(overflow, false);
    stripes.At(0).PublishStack(published, true);
    MarkStripeStack* first = stripes.At(0).StealStack(smr, 0);
    GC_EXPECT_EQ(first->Pop().partial_array_offset(), 2u);
    MarkStripeStack::Destroy(first);
    MarkStripeStack* second = stripes.At(0).StealStack(smr, 0);
    GC_EXPECT_EQ(second->Pop().partial_array_offset(), 3u);
    MarkStripeStack::Destroy(second);
}

GC_TEST(MarkPort203Engine, ShrinkingNStripesStillSeesHighSlotWork)
{
    MarkStripeSet stripes(4);
    MarkStripeStack* high = MarkStripeStack::Create(true);
    high->Push(Entry(9));
    stripes.At(3).PublishStack(high, true);
    stripes.SetNStripes(2);
    GC_EXPECT_TRUE(!stripes.IsEmpty());
    GC_EXPECT_EQ(stripes.FirstNonEmptyStripe(), 3u);
    size_t home = 0;
    size_t seenHigh = 0;
    for (size_t victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
        if (victim == 3) {
            ++seenHigh;
        }
    }
    GC_EXPECT_EQ(seenHigh, 1u);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkStripeStack* taken = stripes.At(3).StealStack(smr, 0);
    GC_EXPECT_TRUE(taken != nullptr);
    MarkStripeStack::Destroy(taken);
    GC_EXPECT_TRUE(stripes.IsEmpty());
}

GC_TEST(MarkPort203Engine, PartialReturnsBeforeTerminate)
{
    MarkStripeSet stripes(1);
    MarkTerminate terminate;
    terminate.Reset(1);
    stripes.SetTerminate(&terminate);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkThreadLocalStacks stacks(1);
    MarkContext context(1, 0, stripes, stacks);
    std::vector<size_t> seen;
    auto result = ZMark::FollowWork(context, smr, stripes, terminate, 0, true,
                                         [&seen](const MarkStackEntry& entry) {
                                             seen.push_back(entry.partial_array_offset());
                                         });
    GC_EXPECT_TRUE(result == ZMark::Result::Partial);
    GC_EXPECT_TRUE(!terminate.Terminated());
    GC_EXPECT_TRUE(terminate.Saturated());
    GC_EXPECT_EQ(seen.size(), 0u);
}

GC_TEST(MarkPort203Engine, PublishWakesWaitingWorker)
{
    MarkStripeSet stripes(2);
    MarkTerminate terminate;
    terminate.Reset(2);
    stripes.SetTerminate(&terminate);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkThreadLocalStacks waiterStacks(2);
    MarkThreadLocalStacks producerStacks(2);
    MarkContext waiter(2, 0, stripes, waiterStacks);
    MarkContext producer(2, 1, stripes, producerStacks);
    std::atomic<bool> waiterEntered{ false };
    std::vector<size_t> seen;
    std::thread waitThread([&]() {
        waiterEntered.store(true, std::memory_order_release);
        DrainFollow(waiter, smr, stripes, terminate, 0, seen, false);
    });
    while (!waiterEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MarkStripeStack* stack = MarkStripeStack::Create(true);
    stack->Push(Entry(4));
    stripes.At(0).PublishStack(stack, true, stripes.Terminate());
    std::vector<size_t> producerSeen;
    DrainFollow(producer, smr, stripes, terminate, 1, producerSeen, false);
    waitThread.join();
    GC_EXPECT_TRUE(terminate.Terminated());
    GC_EXPECT_EQ(seen.size() + producerSeen.size(), 1u);
}

GC_TEST(MarkPort203Engine, LeaveUnblocksTryTerminateWaiter)
{
    MarkTerminate terminate;
    terminate.Reset(2);
    MarkStripeSet stripes(2);
    stripes.SetTerminate(&terminate);
    std::atomic<bool> waiting{ false };
    std::atomic<bool> finished{ false };
    std::thread waiter([&]() {
        waiting.store(true, std::memory_order_release);
        GC_EXPECT_TRUE(terminate.TryTerminate(stripes, stripes.NStripes()));
        finished.store(true, std::memory_order_release);
    });
    while (!waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    terminate.Leave();
    waiter.join();
    GC_EXPECT_TRUE(finished.load(std::memory_order_acquire));
}

GC_TEST(MarkPort203Engine, TrySetNStripesIsAtomicSnapshot)
{
    MarkStripeSet stripes(4);
    GC_EXPECT_EQ(stripes.NStripes(), 4u);
    GC_EXPECT_TRUE(stripes.TrySetNStripes(4, 2));
    GC_EXPECT_EQ(stripes.NStripes(), 2u);
    GC_EXPECT_TRUE(!stripes.TrySetNStripes(4, 1));
    GC_EXPECT_EQ(stripes.NStripes(), 2u);
    GC_EXPECT_EQ(stripes.StripeForWorker(2, 0), 0u);
    GC_EXPECT_EQ(stripes.Next(0), 1u);
    GC_EXPECT_EQ(stripes.Next(3), 0u);
}

GC_TEST(MarkPort203Engine, DomainPrepareResizeKeepsCapacity)
{
    MapleRuntime::GcUnit::WorkerFixture domainWorker;
    ZMark domain(8, MarkingStacks::MarkingGeneration::YOUNG);
    domain.PrepareWork(2);
    GC_EXPECT_EQ(domain.Stripes().Count(), 8u);
    GC_EXPECT_TRUE(domain.Stripes().NStripes() <= 8u);
    domain.ResizeWorkers(4);
    GC_EXPECT_EQ(domain.NWorkers(), 4u);
    GC_EXPECT_EQ(domain.Stripes().Count(), 8u);
    MarkThreadLocalStacks* owner = &domain.Stacks();
    domain.ResizeWorkers(2);
    GC_EXPECT_TRUE(owner == &domain.Stacks());
}

GC_TEST(MarkPort203Engine, CrowdedRestoresNStripes)
{
    MarkStripeSet stripes(4);
    stripes.SetNStripes(1);
    for (size_t i = 0; i < 32; ++i) {
        MarkStripeStack* stack = MarkStripeStack::Create(true);
        stack->Push(Entry(i));
        stripes.At(0).PublishStack(stack, true);
    }
    GC_EXPECT_TRUE(stripes.IsCrowded());
    GC_EXPECT_TRUE(stripes.TrySetNStripes(1, 2));
    GC_EXPECT_EQ(stripes.NStripes(), 2u);
}

GC_TEST(MarkPort203Engine, AbortAndResizeRequestsStopFollowWork)
{
    ZAbort abort;
    MapleRuntime::GcUnit::WorkerFixture domainWorker;
    ZMark domain(4, MarkingStacks::MarkingGeneration::YOUNG);
    domain.BindAbort(&abort);
    domain.PrepareWork(1);
    GC_EXPECT_TRUE(!domain.PollStop());
    abort.Request();
    GC_EXPECT_TRUE(domain.PollStop());
    abort.Reset();
    GC_EXPECT_TRUE(!domain.PollStop());

    ZStatWorkers statWorkers;
    ZWorkers workers(GCCycleGeneration::YOUNG, 2, &statWorkers);
    workers.set_active();
    workers.set_active_workers(1);
    domain.BindWorkers(&workers);
    domain.PrepareWork(1);
    GC_EXPECT_TRUE(!domain.PollStop());
    workers.request_resize_workers(2);
    GC_EXPECT_TRUE(domain.PollStop());
}

// ZMark::drain/rebalance_work (zMark.cpp:468-485): stop following while
// retaining unpublished work until the worker flushes and the phase joins.
GC_TEST(MarkPort203Engine, AbortReturnsWithRemainingMarkWorkOwned)
{
    MapleRuntime::GcUnit::B09RuntimeFixture runtime;
    ZAbort abort;
    MapleRuntime::GcUnit::WorkerFixture domainWorker;
    ZMark domain(4, MarkingStacks::MarkingGeneration::MAJOR);
    domain.BindAbort(&abort);
    domain.PrepareWork(1);
    MarkThreadLocalStacks stacks(4);
    MarkContext context(1, 0, domain.Stripes(), stacks);
    constexpr size_t count = 64;
    for (size_t i = 0; i < count; ++i) {
        stacks.Push(domain.Stripes(), 0, Entry(i), true);
    }
    size_t followed = 0;
    const auto result = ZMark::FollowWork(context, domain.Smr(), domain.Stripes(), domain.Terminate(),
        0, false, [&](const MarkStackEntry&) {
            ++followed;
            abort.Request();
        }, nullptr, nullptr, &domain);
    GC_EXPECT_TRUE(result == ZMark::Result::Aborted);
    GC_EXPECT_EQ(followed, 1u);
    (void)stacks.Flush(domain.Stripes(), true);
    context.Cache().Flush();
    // zMarkStack.cpp: ZMarkStackList::length counts segments, not entries.
    // The resume below proves every remaining entry is still owned and consumed.
    GC_EXPECT_TRUE(domain.Stripes().Population() > 0);
    GC_EXPECT_TRUE(domain.PollStop());

    // Explicitly resume only the test's token. A cancelled product request
    // returns to the driver and never resets its token to consume this work.
    abort.Reset();
    domain.PrepareWork(1);
    const auto resumed = ZMark::FollowWork(context, domain.Smr(), domain.Stripes(), domain.Terminate(),
        0, false, [&](const MarkStackEntry&) { ++followed; }, nullptr, nullptr, &domain);
    GC_EXPECT_TRUE(resumed == ZMark::Result::Completed);
    GC_EXPECT_EQ(followed, count);
}
