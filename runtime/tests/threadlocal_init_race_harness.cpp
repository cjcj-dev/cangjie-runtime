// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Models CleanThreadLocalData's process-shared first-init flag.
// PROTOCOL_FIXED=0 is the shared volatile bool; =1 deletes it.
//
// usage: threadlocal_init_race_harness

#ifndef PROTOCOL_FIXED
#error "PROTOCOL_FIXED must be 0 (old) or 1 (fixed)"
#endif

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#if PROTOCOL_FIXED
void InitCleaner()
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
}
#else
void InitCleaner()
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    static volatile bool isInit = false;
    if (!isInit) {
        isInit = true;
    }
}
#endif

int main()
{
    constexpr int kThreads = 8;
    std::atomic<int> ready { 0 };
    std::atomic<int> go { 0 };
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (go.load(std::memory_order_acquire) == 0) {
            }
            InitCleaner();
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads) {
    }
    go.store(1, std::memory_order_release);
    for (std::thread& t : threads) {
        t.join();
    }
    std::cerr << "HARNESS_OK case=threadlocal_init protocol_fixed=" << PROTOCOL_FIXED << '\n';
    return 0;
}
