// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <thread>

#include "gc_heap_fixture.hpp"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Collector/RelocationRequestQueue.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(RelocationRequestQueue, RequestedReceiptIsClaimedBeforeOrdinaryAndCompletesOnce)
{
    RelocationRequestQueue queue;
    int owner = 0;
    int ordinary = 0;
    constexpr MAddress kFrom = 0x1000;
    constexpr MAddress kTo = 0x2000;
    RelocationRequestQueue::EnqueueResult added = queue.Add(&owner, kFrom);
    GC_EXPECT_TRUE(added.inserted);

    std::atomic<bool> waiterEntered{ false };
    std::atomic<bool> waiterReturned{ false };
    std::atomic<MAddress> answer{ 0 };
    std::thread waiter([&]() {
        waiterEntered.store(true, std::memory_order_release);
        answer.store(queue.Wait(added.request), std::memory_order_release);
        waiterReturned.store(true, std::memory_order_release);
    });
    JoinGuard waiterGuard(waiter);
    while (!waiterEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    int ordinaryClaims = 0;
    RelocationRequestQueue::Selection selected = queue.SelectBeforeOrdinary([&]() -> void* {
        ++ordinaryClaims;
        return &ordinary;
    });
    const bool requestWasFirst = selected.is_request();
    const bool waiterStayedBlocked = !waiterReturned.load(std::memory_order_acquire);
    RelocationRequestQueue::Handle claimed = selected.request;
    if (claimed == nullptr) {
        // Keep the negative arm finite: if priority is deliberately removed,
        // claim the still-pending request only after recording the bad order.
        claimed = queue.PruneAndClaim();
    }
    GC_EXPECT_TRUE(queue.Publish(kFrom, kTo));
    waiter.join();
    GC_EXPECT_TRUE(requestWasFirst);
    GC_EXPECT_TRUE(claimed == added.request);
    GC_EXPECT_EQ(ordinaryClaims, 0);
    GC_EXPECT_TRUE(waiterStayedBlocked);
    GC_EXPECT_TRUE(waiterReturned.load(std::memory_order_acquire));
    GC_EXPECT_EQ(answer.load(std::memory_order_acquire), kTo);
    GC_EXPECT_EQ(queue.CompletionCount(), static_cast<uint64_t>(1));
    GC_EXPECT_FALSE(queue.Publish(kFrom, kTo + 8));
    GC_EXPECT_EQ(queue.CompletionCount(), static_cast<uint64_t>(1));

    selected = queue.SelectBeforeOrdinary([&]() -> void* {
        ++ordinaryClaims;
        return &ordinary;
    });
    GC_EXPECT_FALSE(selected.is_request());
    GC_EXPECT_TRUE(selected.ordinary == &ordinary);
    GC_EXPECT_EQ(ordinaryClaims, 1);
}

GC_TEST(RelocationRequestQueue, FailedOwnerCompletionReleasesWaiterWithoutReceipt)
{
    RelocationRequestQueue queue;
    queue.BeginCycle();
    queue.BeginWorkers(1);
    int owner = 0;
    constexpr MAddress kFrom = 0x3000;
    const auto added = queue.Add(&owner, kFrom);
    GC_EXPECT_TRUE(added.accepted);
    const auto claimed = queue.PruneAndClaim();
    GC_EXPECT_TRUE(claimed == added.request);

    std::atomic<bool> returned{ false };
    std::atomic<MAddress> answer{ 1 };
    std::thread waiter([&]() {
        answer.store(queue.Wait(added.request), std::memory_order_release);
        returned.store(true, std::memory_order_release);
    });
    JoinGuard waiterGuard(waiter);

    GC_EXPECT_EQ(queue.CompleteOwner(&owner, [](MAddress) { return static_cast<MAddress>(0); }),
                 static_cast<size_t>(1));
    waiter.join();
    GC_EXPECT_TRUE(returned.load(std::memory_order_acquire));
    GC_EXPECT_EQ(answer.load(std::memory_order_acquire), static_cast<MAddress>(0));

    const auto done = queue.SynchronizePoll();
    GC_EXPECT_TRUE(done.workersDone);
}

GC_TEST(RelocationRequestQueue, LastWorkerAndAddHaveOneAtomicProgressDecision)
{
    RelocationRequestQueue queue;
    queue.BeginCycle();
    queue.BeginWorkers(1);
    int owner = 0;
    constexpr MAddress kFrom = 0x4000;
    constexpr MAddress kTo = 0x5000;

    // Add wins the queue lock before the last worker synchronizes, so that
    // worker must observe and complete it rather than terminate.
    const auto added = queue.Add(&owner, kFrom);
    GC_EXPECT_TRUE(added.accepted);
    auto selected = queue.SynchronizePoll();
    GC_EXPECT_FALSE(selected.workersDone);
    GC_EXPECT_TRUE(selected.request == added.request);
    GC_EXPECT_EQ(queue.CompleteOwner(&owner, [=](MAddress from) {
        return from == kFrom ? kTo : static_cast<MAddress>(0);
    }), static_cast<size_t>(1));
    GC_EXPECT_EQ(queue.Wait(added.request), kTo);
    selected = queue.SynchronizePoll();
    GC_EXPECT_TRUE(selected.workersDone);

    // Once the last worker closes the generation, Add owns the opposite arm:
    // it is failed synchronously and cannot become an unconsumed queued item.
    const auto late = queue.Add(&owner, kFrom + 8);
    GC_EXPECT_FALSE(late.accepted);
    GC_EXPECT_EQ(queue.Wait(late.request), static_cast<MAddress>(0));
}

GC_TEST(RelocationRequestQueue, AddWakesAWorkerSynchronizedOnAnEmptyQueue)
{
    RelocationRequestQueue queue;
    queue.BeginCycle();
    queue.BeginWorkers(2);
    int owner = 0;
    constexpr MAddress kFrom = 0x8000;
    constexpr MAddress kTo = 0x9000;
    std::atomic<bool> workerReceived{ false };
    std::atomic<bool> workerDone{ false };

    std::thread worker([&]() {
        auto selected = queue.SynchronizePoll();
        if (selected.is_request()) {
            workerReceived.store(true, std::memory_order_release);
            (void)queue.CompleteOwner(&owner, [](MAddress from) {
                return from == kFrom ? kTo : static_cast<MAddress>(0);
            });
        }
        selected = queue.SynchronizePoll();
        workerDone.store(selected.workersDone, std::memory_order_release);
    });
    JoinGuard workerGuard(worker);

    // This is only an injection barrier: it ensures Add happens after the
    // worker is actually waiting, so the test cannot pass via an early poll.
    while (queue.SynchronizedWorkerCount() == 0) {
        std::this_thread::yield();
    }
    const auto added = queue.Add(&owner, kFrom);
    GC_EXPECT_TRUE(added.accepted);
    GC_EXPECT_EQ(queue.Wait(added.request), kTo);
    GC_EXPECT_TRUE(workerReceived.load(std::memory_order_acquire));
    GC_EXPECT_TRUE(queue.SynchronizePoll().workersDone);
    worker.join();
    GC_EXPECT_TRUE(workerDone.load(std::memory_order_acquire));
}

GC_TEST(RelocationRequestQueue, RegionManagerCompletesRetainedOwnerThroughProductWiring)
{
    GcHeapFixture fx;
    RegionManager manager;
    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginCycle();
    queue.BeginWorkers(1);
    const MAddress from = fx.region0->GetRegionStart();
    const auto added = queue.Add(fx.region0, from);

    GC_EXPECT_EQ(manager.CompleteRelocationRequests(fx.region0), static_cast<size_t>(1));
    const bool productCompleted = added.request->state() == RelocationRequestQueue::State::FAILED;
    GC_EXPECT_TRUE(productCompleted);
    if (!productCompleted) {
        // Keep a deliberately disconnected product arm finite so it reports
        // this test alone instead of hanging the runner.
        (void)queue.Fail(from);
    }
    GC_EXPECT_EQ(queue.Wait(added.request), static_cast<MAddress>(0));
    GC_EXPECT_TRUE(queue.SynchronizePoll().workersDone);
}
