// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

extern "C" bool MRT_NewForeignCJThread();
extern "C" bool MRT_EndForeignCJThread();
extern "C" bool MRT_LeaveSaferegion();

namespace {
constexpr uint64_t FOREIGN_CHURN_ATTEMPTS = 512;
constexpr uint64_t FOREIGN_CHURN_BATCH = 8;

std::atomic<bool> stopForeignChurn = { false };
std::atomic<uint64_t> foreignChurnAttempts = { 0 };
std::atomic<uint64_t> foreignChurnAttached = { 0 };
std::thread foreignChurnController;
} // namespace

extern "C" void MRT_StartEpochHandshakeForeignChurn()
{
    stopForeignChurn.store(false, std::memory_order_release);
    foreignChurnAttempts.store(0, std::memory_order_relaxed);
    foreignChurnAttached.store(0, std::memory_order_relaxed);
    foreignChurnController = std::thread([]() {
        while (!stopForeignChurn.load(std::memory_order_acquire) &&
               foreignChurnAttempts.load(std::memory_order_relaxed) < FOREIGN_CHURN_ATTEMPTS) {
            std::vector<std::thread> wave;
            wave.reserve(FOREIGN_CHURN_BATCH);
            for (uint64_t index = 0; index < FOREIGN_CHURN_BATCH; ++index) {
                wave.emplace_back([]() {
                    foreignChurnAttempts.fetch_add(1, std::memory_order_relaxed);
                    if (MRT_NewForeignCJThread()) {
                        foreignChurnAttached.fetch_add(1, std::memory_order_relaxed);
                        (void)MRT_LeaveSaferegion();
                        std::this_thread::yield();
                        (void)MRT_EndForeignCJThread();
                    }
                });
            }
            for (std::thread& thread : wave) {
                thread.join();
            }
        }
    });
}

extern "C" uint64_t MRT_StopEpochHandshakeForeignChurn()
{
    stopForeignChurn.store(true, std::memory_order_release);
    foreignChurnController.join();
    return (foreignChurnAttached.load(std::memory_order_relaxed) << 32) |
        foreignChurnAttempts.load(std::memory_order_relaxed);
}
