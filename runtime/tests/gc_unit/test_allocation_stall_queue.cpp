// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#if defined(MRT_GC_UNIT_TESTS)

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "gc_unittest.hpp"
#include "Heap/Allocator/AllocationStallQueue.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(AllocationStallQueue, FirstWaiterOwnsGcRequest)
{
    AllocationStallQueue queue;
    AllocationStallRequest first(64);
    AllocationStallRequest second(64);
    GC_EXPECT_TRUE(queue.Enqueue(first));
    GC_EXPECT_FALSE(queue.Enqueue(second));
    GC_EXPECT_EQ(queue.EnqueuedCount(), static_cast<size_t>(2));
    GC_EXPECT_EQ(queue.Pending(), static_cast<size_t>(2));

    // This test isolates enqueue->GC ownership; completion is covered by the
    // following tests, so a cut in the notification path turns only this item red.
}

GC_TEST(AllocationStallQueue, FifoWaitersAreSatisfiedByCapacity)
{
    AllocationStallQueue queue;
    AllocationStallRequest small(32);
    AllocationStallRequest large(128);
    queue.Enqueue(small);
    queue.Enqueue(large);

    // A capacity wave that cannot satisfy the head must not skip it or satisfy
    // a later request out of order.
    GC_EXPECT_EQ(queue.SatisfyAvailable([](size_t bytes) { return bytes <= 64; }), static_cast<size_t>(1));
    GC_EXPECT_TRUE(small.Wait());
    GC_EXPECT_EQ(queue.Pending(), static_cast<size_t>(1));
    GC_EXPECT_EQ(queue.SatisfyAvailable([](size_t) { return true; }), static_cast<size_t>(1));
    GC_EXPECT_TRUE(large.Wait());
    GC_EXPECT_EQ(queue.DequeuedCount(), static_cast<size_t>(2));
}

GC_TEST(AllocationStallQueue, FailedWaveAnswersEveryRequestExactlyOnce)
{
    AllocationStallQueue queue;
    constexpr size_t waiterCount = 4;
    std::vector<std::unique_ptr<AllocationStallRequest>> requests;
    requests.reserve(waiterCount);
    for (size_t i = 0; i < waiterCount; ++i) {
        requests.emplace_back(std::make_unique<AllocationStallRequest>(64 + i));
        queue.Enqueue(*requests.back());
    }
    GC_EXPECT_EQ(queue.FailAll(), waiterCount);
    for (auto& request : requests) {
        GC_EXPECT_FALSE(request->Wait());
        // A duplicate terminal answer is ignored and cannot increment dequeue
        // accounting or wake a second consumer.
        request->Satisfy(true);
    }
    GC_EXPECT_EQ(queue.Pending(), static_cast<size_t>(0));
    GC_EXPECT_EQ(queue.DequeuedCount(), waiterCount);
    GC_EXPECT_EQ(queue.FailedCount(), waiterCount);
}

#endif // MRT_GC_UNIT_TESTS
