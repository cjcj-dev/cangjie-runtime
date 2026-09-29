// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Base/SingleWriterSynchronizer.h"

namespace MapleRuntime {
// HotSpot utilities/singleWriterSynchronizer.cpp:44-95. The caller owns
// the writer synchronization lock; this never takes the list writer lock.
void SingleWriterSynchronizer::Synchronize()
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    unsigned value = enter.load(std::memory_order_relaxed);
    auto& newExit = exits[(value + 1u) & 1u];
    unsigned old;
    for (;;) {
        old = value;
        newExit.store(value + 1u, std::memory_order_relaxed);
        if (enter.compare_exchange_strong(value, old + 1u)) {
            break;
        }
    }
    auto& oldExit = exits[old & 1u];
    waitingFor.store(old, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    while (old != oldExit.load(std::memory_order_acquire)) {
        wakeup.wait();
    }
    while (wakeup.trywait()) {}
}
} // namespace MapleRuntime
