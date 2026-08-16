// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZForwardingLife: three-state refcount + claim + done + detach wait.
// Anchors: zForwarding.cpp:86-194.

#include <atomic>
#include <chrono>
#include <thread>

#include "Heap/Collector/ZForwardingLife.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct Life {
    std::atomic<int32_t> ref{ 0 };
    std::atomic<bool> claimed{ false };
    std::atomic<bool> done{ false };
};

} // namespace

GC_TEST(ZForwardingLife, RetainAfterReleaseRefuses)
{
    Life life;
    ZForwardingLife::ResetForForwarding(life.ref, life.claimed, life.done);
    GC_EXPECT_TRUE(ZForwardingLife::retain_page(life.ref, life.done));
    ZForwardingLife::release_page(life.ref); // construction 1 + retain → 2 → 1
    ZForwardingLife::release_page(life.ref); // 1 → 0
    GC_EXPECT_EQ(life.ref.load(), 0);
    GC_EXPECT_FALSE(ZForwardingLife::retain_page(life.ref, life.done));
}

GC_TEST(ZForwardingLife, DetachWaitsForLastReader)
{
    Life life;
    ZForwardingLife::ResetForForwarding(life.ref, life.claimed, life.done);
    GC_EXPECT_TRUE(ZForwardingLife::retain_page(life.ref, life.done)); // 2
    std::atomic<bool> detachEntered{ false };
    std::atomic<bool> detachDone{ false };
    std::thread waiter([&]() {
        detachEntered.store(true, std::memory_order_release);
        ZForwardingLife::detach_page(life.ref);
        detachDone.store(true, std::memory_order_release);
    });
    while (!detachEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    GC_EXPECT_FALSE(detachDone.load(std::memory_order_acquire));
    ZForwardingLife::release_page(life.ref); // 2 → 1, still held by construction token
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    GC_EXPECT_FALSE(detachDone.load(std::memory_order_acquire));
    ZForwardingLife::release_page(life.ref); // 1 → 0
    waiter.join();
    GC_EXPECT_TRUE(detachDone.load(std::memory_order_acquire));
    GC_EXPECT_EQ(life.ref.load(), 0);
}

GC_TEST(ZForwardingLife, ClaimInvertsAndLateRetainRefuses)
{
    Life life;
    ZForwardingLife::ResetForForwarding(life.ref, life.claimed, life.done);
    GC_EXPECT_TRUE(ZForwardingLife::retain_page(life.ref, life.done)); // 2
    GC_EXPECT_TRUE(ZForwardingLife::claim(life.claimed));
    GC_EXPECT_FALSE(ZForwardingLife::claim(life.claimed));
    std::atomic<bool> claimEntered{ false };
    std::atomic<bool> claimDone{ false };
    std::thread claimer([&]() {
        claimEntered.store(true, std::memory_order_release);
        ZForwardingLife::in_place_relocation_claim_page(life.ref);
        claimDone.store(true, std::memory_order_release);
    });
    while (!claimEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    GC_EXPECT_FALSE(claimDone.load(std::memory_order_acquire));
    GC_EXPECT_TRUE(life.ref.load() < 0);
    std::atomic<bool> lateDone{ false };
    std::atomic<bool> lateRetained{ true };
    std::thread late([&]() {
        const bool ok = ZForwardingLife::retain_page(life.ref, life.done);
        lateRetained.store(ok, std::memory_order_release);
        lateDone.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    GC_EXPECT_FALSE(lateDone.load(std::memory_order_acquire));
    ZForwardingLife::release_page(life.ref); // 2 → -2, then +1 → -1, claim proceeds
    claimer.join();
    GC_EXPECT_TRUE(claimDone.load(std::memory_order_acquire));
    GC_EXPECT_EQ(life.ref.load(), -1);
    ZForwardingLife::mark_done(life.done);
    late.join();
    GC_EXPECT_TRUE(lateDone.load(std::memory_order_acquire));
    GC_EXPECT_FALSE(lateRetained.load(std::memory_order_acquire));
    ZForwardingLife::release_page(life.ref); // -1 → 0
    GC_EXPECT_EQ(life.ref.load(), 0);
    GC_EXPECT_FALSE(ZForwardingLife::retain_page(life.ref, life.done));
}
