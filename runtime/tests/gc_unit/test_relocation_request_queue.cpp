// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
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
    queue.BeginWorkers(1);
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

// Page-level waiters no longer depend on an object Request terminal, but the
// queue generation still owns every accepted Request.  Closing a generation
// must therefore terminate and remove even a request which a worker claimed but
// could not publish; Wait(handle), state observers, and queue reuse all rely on
// that lifecycle contract independently of WaitRoutedTipReady's page predicate.
//
// The other tests in this file all reach their terminal through Publish or
// CompleteOwner, so none of them covers a request that no worker ever touches.
// Assert on the observable state rather than on a waiter, so a lost terminal
// reports here instead of stalling the runner.
GC_TEST(RelocationRequestQueue, GenerationCloseTerminatesARequestNoWorkerEverPublished)
{
    RelocationRequestQueue queue;
    queue.BeginWorkers(1);
    int owner = 0;
    constexpr MAddress kFrom = 0xA000;
    const auto added = queue.Add(&owner, kFrom);
    GC_EXPECT_TRUE(added.accepted);
    GC_EXPECT_TRUE(added.inserted);
    GC_EXPECT_EQ(queue.PendingCount(), static_cast<size_t>(1));
    GC_EXPECT_EQ(queue.CompletionCount(), static_cast<uint64_t>(0));

    // Replay the worker shape of RegionManager.h:1216-1243.  The worker claims
    // the request first (SelectBeforeOrdinary) ...
    const auto claimed = queue.SelectBeforeOrdinary([]() -> void* { return nullptr; });
    GC_EXPECT_TRUE(claimed.is_request());
    GC_EXPECT_TRUE(claimed.request == added.request);

    // ... and then loses the region ownership transition at
    // RegionManager.h:1238-1241, so it continues the loop without completing
    // the request.  The request is now off the deque yet still registered, and
    // no CompleteOwner will ever name it: the deque poll and the ordinary poll
    // are both empty from here on.
    GC_EXPECT_EQ(queue.PendingCount(), static_cast<size_t>(1));
    const auto empty = queue.SelectBeforeOrdinary([]() -> void* { return nullptr; });
    GC_EXPECT_FALSE(static_cast<bool>(empty));

    const auto done = queue.SynchronizePoll();
    GC_EXPECT_TRUE(done.workersDone);
    GC_EXPECT_FALSE(done.is_request());

    // Load bearing: closing the generation must give this request a terminal.
    GC_EXPECT_TRUE(added.request->state() == RelocationRequestQueue::State::FAILED);
    GC_EXPECT_EQ(queue.PendingCount(), static_cast<size_t>(0));
    GC_EXPECT_EQ(queue.CompletionCount(), static_cast<uint64_t>(1));

    // Only reached once the terminal above is proven set, so this cannot block.
    GC_EXPECT_EQ(queue.Wait(added.request), static_cast<MAddress>(0));
    GC_EXPECT_EQ(added.request->receipt(), static_cast<MAddress>(0));
}

GC_TEST(RelocationRequestQueue, PageCompletionTerminatesWaitWithoutObjectReceipt)
{
    RelocationRequestQueue queue;
    queue.BeginWorkers(1);
    int owner = 0;
    constexpr MAddress kFrom = 0xA008;
    const auto added = queue.Add(&owner, kFrom);
    GC_EXPECT_TRUE(added.accepted);

    std::atomic<bool> pageDone{ false };
    std::atomic<bool> cleanup{ false };
    std::atomic<bool> predicateObserved{ false };
    std::mutex returnedMutex;
    std::condition_variable returnedCondition;
    bool returned = false;
    std::thread waiter([&]() {
        queue.WaitUntil(added.request, [&]() {
            predicateObserved.store(true, std::memory_order_release);
            returnedCondition.notify_one();
            return pageDone.load(std::memory_order_acquire) || cleanup.load(std::memory_order_acquire);
        });
        {
            std::lock_guard<std::mutex> lock(returnedMutex);
            returned = true;
        }
        returnedCondition.notify_one();
    });
    JoinGuard waiterGuard(waiter);

    std::unique_lock<std::mutex> returnedLock(returnedMutex);
    const bool waiterReachedPredicate = returnedCondition.wait_for(
        returnedLock, std::chrono::seconds(1),
        [&predicateObserved]() { return predicateObserved.load(std::memory_order_acquire); });
    returnedLock.unlock();
    GC_EXPECT_TRUE(waiterReachedPredicate);

    // Publish page completion only after the waiter has evaluated the predicate
    // false once; otherwise the entry fast path would not exercise the wait.
    pageDone.store(true, std::memory_order_release);
    returnedLock.lock();
    const bool returnedByPage = returnedCondition.wait_for(
        returnedLock, std::chrono::seconds(1), [&returned]() { return returned; });
    returnedLock.unlock();

    // Keep a deliberately object-terminal implementation finite so the red arm
    // reports this item instead of stalling the entire suite.
    if (!returnedByPage) {
        cleanup.store(true, std::memory_order_release);
        (void)queue.Fail(kFrom);
    }
    waiter.join();

    GC_EXPECT_TRUE(returnedByPage);
    if (returnedByPage) {
        GC_EXPECT_TRUE(added.request->state() == RelocationRequestQueue::State::QUEUED);
        GC_EXPECT_EQ(added.request->receipt(), static_cast<MAddress>(0));
        GC_EXPECT_TRUE(queue.Fail(kFrom));
    }
    GC_EXPECT_TRUE(queue.SynchronizePoll().workersDone);
}

GC_TEST(RelocationRequestQueue, FailedOwnerCompletionReleasesWaiterWithoutReceipt)
{
    RelocationRequestQueue queue;
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

GC_TEST(RelocationRequestQueue, ProductPreparationCannotReopenClosedWorkerGeneration)
{
    RegionManager manager;
    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    GC_EXPECT_TRUE(queue.SynchronizePoll().workersDone);

    // EvacuateYoungRegions performs PrepareForwardTable<Young> again after
    // ForwardFromSpace has closed the worker generation. Exercise its product
    // consumer directly: preparation may retire forwarding state, but it must
    // not reactivate the request queue without workers.
    manager.PrepareFromRegionList<Generation::Young>();

    int lateOwner = 0;
    const auto late = queue.Add(&lateOwner, 0x5008);
    GC_EXPECT_FALSE(late.accepted);
    if (late.accepted) {
        // Keep the deliberate reopen arm finite: report this test instead of
        // leaving its accepted request with no worker and stalling the suite.
        (void)queue.Fail(late.request->from());
    }
    GC_EXPECT_TRUE(late.request->state() == RelocationRequestQueue::State::FAILED);
    GC_EXPECT_EQ(queue.Wait(late.request), static_cast<MAddress>(0));
    GC_EXPECT_EQ(queue.PendingCount(), static_cast<size_t>(0));
}

GC_TEST(RelocationRequestQueue, AddWakesAWorkerSynchronizedOnAnEmptyQueue)
{
    RelocationRequestQueue queue;
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
