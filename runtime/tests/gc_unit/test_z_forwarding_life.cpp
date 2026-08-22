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
#include "Heap/Verify/FromPageDetachCheck.h"
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

GC_TEST(ZForwardingLife, CopyInflightPairing)
{
    std::atomic<int32_t> copy{ 0 };
    ZForwardingLife::note_copy(copy);
    ZForwardingLife::note_copy(copy);
    GC_EXPECT_EQ(copy.load(), 2);
    ZForwardingLife::end_copy(copy);
    GC_EXPECT_EQ(copy.load(), 1);
    ZForwardingLife::end_copy(copy);
    GC_EXPECT_EQ(copy.load(), 0);
    ZForwardingLife::wait_copied(copy);
    GC_EXPECT_EQ(copy.load(), 0);
}

GC_TEST(ZForwardingLife, CopyInflightDrainWakes)
{
    std::atomic<int32_t> copy{ 0 };
    ZForwardingLife::note_copy(copy);
    std::atomic<bool> entered{ false };
    std::atomic<bool> finished{ false };
    std::thread waiter([&]() {
        entered.store(true, std::memory_order_release);
        ZForwardingLife::wait_copied(copy);
        finished.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    GC_EXPECT_FALSE(finished.load(std::memory_order_acquire));
    ZForwardingLife::end_copy(copy);
    waiter.join();
    GC_EXPECT_TRUE(finished.load(std::memory_order_acquire));
    GC_EXPECT_EQ(copy.load(), 0);
}

GC_TEST(ZForwardingLife, ResetIdleWakesRetainClaimed)
{
    // Fifth face: retain_page n<0 inlines add_and_wait as WaitUntilDone.
    // ExpireKept / InitRegionInfo ResetIdle the same words (ZGC destroys the
    // forwarding). Without notify + n==0 exit the waiter sleeps forever.
    Life life;
    ZForwardingLife::ResetForForwarding(life.ref, life.claimed, life.done);
    ZForwardingLife::in_place_relocation_claim_page(life.ref); // 1 → -1
    GC_EXPECT_TRUE(life.ref.load() < 0);
    std::atomic<bool> entered{ false };
    std::atomic<bool> finished{ false };
    std::atomic<bool> retained{ true };
    std::thread waiter([&]() {
        entered.store(true, std::memory_order_release);
        const bool ok = ZForwardingLife::retain_page(life.ref, life.done);
        retained.store(ok, std::memory_order_release);
        finished.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    GC_EXPECT_FALSE(finished.load(std::memory_order_acquire));
    ZForwardingLife::ResetIdle(life.ref, life.claimed, life.done);
    waiter.join();
    GC_EXPECT_TRUE(finished.load(std::memory_order_acquire));
    GC_EXPECT_FALSE(retained.load(std::memory_order_acquire));
}

GC_TEST(ZForwardingLife, DetachCheckMeasuresAndHonorsGate)
{
    GcHeapFixture fx;
    const auto site = FromPageDetach::Site::TAKE_GARBAGE_REUSE;
    const FromPageDetach::Counters before = FromPageDetach::GetCounters(site);

    fx.region0->SetRouteDestHold(1);
    ZForwardingLife::ResetForForwarding(fx.region0->metadata.fwdRefCount, fx.region0->metadata.fwdClaimed,
                                        fx.region0->metadata.fwdDone);
    fx.region0->NoteCopyInflight();

    const bool allowed = FromPageDetach::FromPageDetachCheck(fx.region0, site);
    GC_EXPECT_EQ(allowed, !FromPageDetach::GateEnabled());
    const FromPageDetach::Counters after = FromPageDetach::GetCounters(site);
    GC_EXPECT_EQ(after.checks, before.checks + 1);
    GC_EXPECT_EQ(after.withEvidence, before.withEvidence + 1);
    GC_EXPECT_EQ(after.blocked, before.blocked + (FromPageDetach::GateEnabled() ? 1 : 0));
    GC_EXPECT_EQ(after.routeDestHeld, before.routeDestHeld + 1);
    GC_EXPECT_EQ(after.forwardingPositive, before.forwardingPositive + 1);
    GC_EXPECT_EQ(after.forwardingReaders, before.forwardingReaders);
    GC_EXPECT_EQ(after.copyInflight, before.copyInflight + 1);

    {
        FromPageDetach::ReusePermitScope permit;
        GC_EXPECT_TRUE(FromPageDetach::FromPageDetachCheck(fx.region0, site));
    }
    const FromPageDetach::Counters permitted = FromPageDetach::GetCounters(site);
    GC_EXPECT_EQ(permitted.checks, after.checks + 1);
    GC_EXPECT_EQ(permitted.withEvidence, after.withEvidence + 1);
    GC_EXPECT_EQ(permitted.blocked, after.blocked);

    // Both arms observe without draining or clearing any evidence word here.
    GC_EXPECT_EQ(fx.region0->ForwardingRefCount(), 1);
    GC_EXPECT_EQ(fx.region0->CopyInflight(), 1);
    GC_EXPECT_TRUE(fx.region0->IsRouteDestHeld());

    fx.region0->EndCopyInflight();
    ZForwardingLife::ResetIdle(fx.region0->metadata.fwdRefCount, fx.region0->metadata.fwdClaimed,
                               fx.region0->metadata.fwdDone);
    fx.region0->SetRouteDestHold(0);

    // A completed drain retains the claimed latch until the next region life.
    // ref=0/done=1 is already detached and must not self-quarantine.
    fx.region0->metadata.fwdClaimed.store(true, std::memory_order_release);
    fx.region0->metadata.fwdDone.store(true, std::memory_order_release);
    const FromPageDetach::Counters completedBefore = FromPageDetach::GetCounters(site);
    GC_EXPECT_TRUE(FromPageDetach::FromPageDetachCheck(fx.region0, site));
    const FromPageDetach::Counters completedAfter = FromPageDetach::GetCounters(site);
    GC_EXPECT_EQ(completedAfter.checks, completedBefore.checks + 1);
    GC_EXPECT_EQ(completedAfter.withEvidence, completedBefore.withEvidence);
    GC_EXPECT_EQ(completedAfter.forwardingClaimed, completedBefore.forwardingClaimed + 1);
    GC_EXPECT_EQ(completedAfter.forwardingReleased, completedBefore.forwardingReleased + 1);
    ZForwardingLife::ResetIdle(fx.region0->metadata.fwdRefCount, fx.region0->metadata.fwdClaimed,
                               fx.region0->metadata.fwdDone);

}
