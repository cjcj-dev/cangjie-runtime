// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <thread>

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
