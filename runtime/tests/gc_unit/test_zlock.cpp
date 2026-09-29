// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_unittest.hpp"
#include "Heap/z/zLock.inline.hpp"

#include <atomic>
#include <cstdio>
#include <thread>

using namespace MapleRuntime;

// ZGC zLock.inline.hpp:92-94. Exercise the product inline body directly;
// there is no copied implementation or test-only export.
GC_TEST(ZLock, NotifyReturnsTrue)
{
    ZConditionLock monitor;
    std::atomic<bool> entering{false};
    bool woke = false;
    std::thread waiter([&] {
        ZLocker<ZConditionLock> held(&monitor);
        entering.store(true, std::memory_order_release);
        woke = monitor.wait(1000);
    });
    while (!entering.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    {
        // Taking the same lock ensures notify follows the wait's atomic unlock.
        ZLocker<ZConditionLock> held(&monitor);
        monitor.notify();
    }
    waiter.join();
    std::fprintf(stderr, "ZLOCK_RESULT notify woke=%d target=EXPECT_TRUE\n", woke);
    GC_EXPECT_TRUE(woke);
}

GC_TEST(ZLock, TimeoutReturnsFalse)
{
    ZConditionLock monitor;
    ZLocker<ZConditionLock> held(&monitor);
    const bool woke = monitor.wait(10);
    std::fprintf(stderr, "ZLOCK_RESULT timeout woke=%d target=EXPECT_FALSE\n", woke);
    GC_EXPECT_FALSE(woke);
}

// ZGC zLock.inline.hpp:33-43,105-117: mutual exclusion and scope exit.
GC_TEST(ZLock, LockerReleasesAtScopeExit)
{
    ZLock lock;
    bool acquiredWhileHeld = false;
    {
        ZLocker<ZLock> held(&lock);
        std::thread contender([&] {
            acquiredWhileHeld = lock.try_lock();
            if (acquiredWhileHeld) {
                lock.unlock();
            }
        });
        contender.join();
    }
    const bool acquiredAfterExit = lock.try_lock();
    if (acquiredAfterExit) {
        lock.unlock();
    }
    std::fprintf(stderr, "ZLOCK_RESULT exclusion held=%d released=%d\n",
                 acquiredWhileHeld, acquiredAfterExit);
    GC_EXPECT_FALSE(acquiredWhileHeld);
    GC_EXPECT_TRUE(acquiredAfterExit);
}
