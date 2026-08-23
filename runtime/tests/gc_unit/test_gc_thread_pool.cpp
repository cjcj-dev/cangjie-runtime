// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#include "Heap/GcThreadPool.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(GCThreadPool, EveryTaskExecutesExactlyOnceBeforeWaitFinishReturns)
{
    constexpr size_t kTasks = 64;
    std::array<std::atomic<int>, kTasks> executions;
    for (auto& execution : executions) {
        execution.store(0, std::memory_order_relaxed);
    }
    std::atomic<size_t> completed{ 0 };
    GCThreadPool pool("gc-unit-once", 3, GCPoolThread::GC_THREAD_PRIORITY);
    for (size_t i = 0; i < kTasks; ++i) {
        pool.AddWork(new LambdaWork([&, i](size_t) {
            executions[i].fetch_add(1, std::memory_order_relaxed);
            completed.fetch_add(1, std::memory_order_release);
        }));
    }
    pool.Start();
    pool.WaitFinish();
    GC_EXPECT_EQ(completed.load(std::memory_order_acquire), kTasks);
    for (const auto& execution : executions) {
        GC_EXPECT_EQ(execution.load(std::memory_order_relaxed), 1);
    }
    pool.Exit();
}

GC_TEST(GCThreadPool, CallerDrainPreservesOrdinaryFifo)
{
    GCThreadPool pool("gc-unit-fifo", 1, GCPoolThread::GC_THREAD_PRIORITY);
    pool.SetMaxActiveThreadNum(0);
    std::vector<int> order;
    for (int i = 0; i < 4; ++i) {
        pool.AddWork(new LambdaWork([&order, i](size_t) { order.push_back(i); }));
    }
    pool.Start();
    pool.WaitFinish();
    GC_EXPECT_EQ(order.size(), static_cast<size_t>(4));
    for (size_t i = 0; i < order.size(); ++i) {
        GC_EXPECT_EQ(order[i], static_cast<int>(i));
    }
    pool.Exit();
}
