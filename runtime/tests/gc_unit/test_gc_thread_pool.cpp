// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>

#include "Heap/GcThreadPool.h"
#include "Heap/Collector/RelocationRequestQueue.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(GCThreadPool, RelocationRequestHasOneCompletionOwnerBeforeWaitFinishReturns)
{
    constexpr size_t kWorkers = 3;
    constexpr MAddress kFrom = 0x6000;
    constexpr MAddress kTo = 0x7000;
    int owner = 0;
    RelocationRequestQueue queue;
    queue.BeginCycle();
    queue.BeginWorkers(kWorkers);
    const auto added = queue.Add(&owner, kFrom);
    std::atomic<size_t> completionOwners{ 0 };

    GCThreadPool pool("gc-unit-relocate", static_cast<int32_t>(kWorkers - 1),
                      GCPoolThread::GC_THREAD_PRIORITY);
    for (size_t i = 0; i < kWorkers; ++i) {
        pool.AddWork(new LambdaWork([&](size_t) {
            for (;;) {
                auto selected = queue.SelectBeforeOrdinary([]() -> void* { return nullptr; });
                if (!selected) {
                    selected = queue.SynchronizePoll();
                    if (selected.workersDone) {
                        return;
                    }
                }
                if (selected.is_request()) {
                    const size_t n = queue.CompleteOwner(&owner, [](MAddress from) {
                        return from == kFrom ? kTo : static_cast<MAddress>(0);
                    });
                    completionOwners.fetch_add(n, std::memory_order_relaxed);
                }
            }
        }));
    }
    pool.Start();
    pool.WaitFinish();
    GC_EXPECT_EQ(queue.Wait(added.request), kTo);
    GC_EXPECT_EQ(completionOwners.load(std::memory_order_relaxed), static_cast<size_t>(1));
    GC_EXPECT_EQ(queue.CompletionCount(), static_cast<uint64_t>(1));
    pool.Exit();
}
