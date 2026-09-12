// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "Heap/Collector/MarkEngine.h"
#include "Heap/Collector/MarkStripe.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
MarkStackEntry Entry(size_t i)
{
    return MarkStackEntry::PartialArray(i + 1, i + 3, false);
}

void DrainFollow(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, MarkTerminate& terminate,
                 size_t workerId, std::vector<size_t>& seen, bool partial)
{
    (void)MarkEngine::FollowWork(context, smr, stripes, terminate, workerId, partial,
                                 [&seen](const MarkStackEntry& entry) {
                                     seen.push_back(entry.partialArrayOffset());
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
        MarkingSMR smr(workers);
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
    MarkingSMR smr(1);
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
    MarkingSMR smr(1);
    MarkStripeStack* overflow = MarkStripeStack::Create(true);
    overflow->Push(Entry(1));
    MarkStripeStack* published = MarkStripeStack::Create(true);
    published->Push(Entry(2));
    stripes.At(0).PublishStack(overflow, false);
    stripes.At(0).PublishStack(published, true);
    MarkStripeStack* first = stripes.At(0).StealStack(smr, 0);
    GC_EXPECT_EQ(first->Pop().partialArrayOffset(), 2u);
    MarkStripeStack::Destroy(first);
    MarkStripeStack* second = stripes.At(0).StealStack(smr, 0);
    GC_EXPECT_EQ(second->Pop().partialArrayOffset(), 3u);
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
    MarkingSMR smr(1);
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
    MarkingSMR smr(1);
    MarkThreadLocalStacks stacks(1);
    MarkContext context(1, 0, stripes, stacks);
    std::vector<size_t> seen;
    auto result = MarkEngine::FollowWork(context, smr, stripes, terminate, 0, true,
                                         [&seen](const MarkStackEntry& entry) {
                                             seen.push_back(entry.partialArrayOffset());
                                         });
    GC_EXPECT_TRUE(result == MarkEngine::Result::Partial);
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
    MarkingSMR smr(2);
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
