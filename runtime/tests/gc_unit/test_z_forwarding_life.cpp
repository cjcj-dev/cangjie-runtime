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
    std::atomic<bool> waiting{ false };
    void Wait()
    {
        waiting.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
    }
};

} // namespace

GC_TEST(ZForwardingLife, RetainAfterReleaseRefuses)
{
    Life life;
    ZForwardingLife::ResetForForwarding(life.ref, life.claimed, life.done);
    GC_EXPECT_TRUE(ZForwardingLife::retain_page(life.ref, [&] { life.Wait(); }));
    ZForwardingLife::release_page(life.ref); // construction 1 + retain → 2 → 1
    ZForwardingLife::release_page(life.ref); // 1 → 0
    GC_EXPECT_EQ(life.ref.load(), 0);
    GC_EXPECT_FALSE(ZForwardingLife::retain_page(life.ref, [&] { life.Wait(); }));
}

GC_TEST(ZForwardingLife, DetachWaitsForLastReader)
{
    Life life;
    ZForwardingLife::ResetForForwarding(life.ref, life.claimed, life.done);
    GC_EXPECT_TRUE(ZForwardingLife::retain_page(life.ref, [&] { life.Wait(); })); // 2
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

// Port of zForwarding.cpp:95-100: a claimed page completes before false.
GC_TEST(ZForwardingLife, ClaimedRetainWaitsForPageTask)
{
    Life life;
    ZForwardingLife::ResetForForwarding(life.ref, life.claimed, life.done);
    ZForwardingLife::in_place_relocation_claim_page(life.ref);
    std::atomic<bool> returned{ false };
    bool retained = true;
    std::thread waiter([&] {
        retained = ZForwardingLife::retain_page(life.ref, [&] { life.Wait(); });
        returned.store(true, std::memory_order_release);
    });
    JoinGuard guard(waiter);
    while (!life.waiting.load(std::memory_order_acquire)) std::this_thread::yield();
    const bool beforeDone = returned.load(std::memory_order_acquire);
    ZForwardingLife::release_page(life.ref);
    const bool afterRelease = returned.load(std::memory_order_acquire);
    ZForwardingLife::mark_done(life.done);
    waiter.join();
    GC_EXPECT_FALSE(beforeDone);
    GC_EXPECT_FALSE(afterRelease);
    GC_EXPECT_FALSE(retained);
    GC_EXPECT_TRUE(returned.load());
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

GC_TEST(ZForwardingLife, IdleStateDoesNotCompleteTask)
{
    Life life;
    ZForwardingLife::ResetForForwarding(life.ref, life.claimed, life.done);
    ZForwardingLife::ResetIdle(life.ref, life.claimed, life.done);
    GC_EXPECT_EQ(life.ref.load(), 0);
    GC_EXPECT_FALSE(life.done.load());
}
