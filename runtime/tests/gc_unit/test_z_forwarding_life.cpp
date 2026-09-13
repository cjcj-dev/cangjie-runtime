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

#include "Heap/Allocator/RouteDestHold.h"
#include "Heap/Collector/ZForwardingLife.h"
#include "gc_heap_fixture.hpp"
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
    JoinGuard waiterGuard(waiter);
    while (!detachEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool doneBeforeRelease = detachDone.load(std::memory_order_acquire);
    ZForwardingLife::release_page(life.ref); // 2 → 1, still held by construction token
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool doneWithConstructionToken = detachDone.load(std::memory_order_acquire);
    ZForwardingLife::release_page(life.ref); // 1 → 0
    waiter.join();
    GC_EXPECT_FALSE(doneBeforeRelease);
    GC_EXPECT_FALSE(doneWithConstructionToken);
    GC_EXPECT_TRUE(detachDone.load(std::memory_order_acquire));
    GC_EXPECT_EQ(life.ref.load(), 0);
}

GC_TEST(ZForwardingLife, ClaimInvertsAndLateRetainRefusesImmediately)
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
    JoinGuard claimerGuard(claimer);
    while (!claimEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool doneBeforeRelease = claimDone.load(std::memory_order_acquire);
    const int32_t refBeforeLateRetain = life.ref.load();
    // The late retain is a try-lock. It must not wait while this test still
    // owns the reader that the claimer needs to reach -1.
    const bool lateRetained = ZForwardingLife::retain_page(life.ref, life.done);
    const int32_t refAfterLateRetain = life.ref.load();
    if (lateRetained) {
        // Keep the negative control joinable even if retain_page regresses and
        // unexpectedly creates a second reader token.
        ZForwardingLife::release_page(life.ref);
    }
    ZForwardingLife::release_page(life.ref); // 2 → -2, then +1 → -1, claim proceeds
    claimer.join();
    GC_EXPECT_FALSE(doneBeforeRelease);
    GC_EXPECT_TRUE(refBeforeLateRetain < 0);
    GC_EXPECT_FALSE(lateRetained);
    GC_EXPECT_EQ(refAfterLateRetain, -2);
    GC_EXPECT_TRUE(claimDone.load(std::memory_order_acquire));
    GC_EXPECT_EQ(life.ref.load(), -1);
    ZForwardingLife::mark_done(life.done);
    ZForwardingLife::release_page(life.ref); // -1 → 0
    GC_EXPECT_EQ(life.ref.load(), 0);
    GC_EXPECT_FALSE(ZForwardingLife::retain_page(life.ref, life.done));
}

GC_TEST(ZForwardingLife, RouteDestHoldDecisionDistribution)
{
    GcHeapFixture fx;
    constexpr RouteDestHold::Site sites[] = {
        RouteDestHold::Site::ASSEMBLE_RECENT_FULL,
        RouteDestHold::Site::ASSEMBLE_UNMOVABLE,
        RouteDestHold::Site::YOUNG_UNMOVABLE,
        RouteDestHold::Site::YOUNG_RECENT_FULL,
        RouteDestHold::Site::TAKE_GARBAGE,
        RouteDestHold::Site::TAKE_AFTER_DISPEL,
    };
    size_t accepted = 0;
    size_t heldBack = 0;
    auto account = [&](const RegionInfo* region) {
        for (RouteDestHold::Site site : sites) {
            if (RouteDestHold::HoldsBack(region, site)) {
                ++heldBack;
            } else {
                ++accepted;
            }
        }
    };

    account(nullptr);
    fx.region0->SetRouteDestHold(0);
    account(fx.region0);
    fx.region0->SetRouteDestHold(1);
    account(fx.region0);
    fx.region0->SetRouteDestHold(0);

    GC_EXPECT_EQ(accepted, 12u);
    GC_EXPECT_EQ(heldBack, 6u);
}

GC_TEST(ZForwardingLife, ClaimedRetainRefusesImmediatelyAndResetIdle)
{
    // Our mutator retain is a try-lock and can be nested under an existing
    // retain on this page. Once DrainScope has claimed the page (n<0), waiting
    // here would retain that outer pin and deadlock the drain at -1. Refuse
    // immediately; ResetIdle remains responsible for the next forwarding era.
    Life life;
    ZForwardingLife::ResetForForwarding(life.ref, life.claimed, life.done);
    ZForwardingLife::in_place_relocation_claim_page(life.ref); // 1 → -1
    GC_EXPECT_TRUE(life.ref.load() < 0);
    GC_EXPECT_FALSE(ZForwardingLife::retain_page(life.ref, life.done));
    GC_EXPECT_EQ(life.ref.load(), -1);
    ZForwardingLife::ResetIdle(life.ref, life.claimed, life.done);
    GC_EXPECT_EQ(life.ref.load(), 0);
}
