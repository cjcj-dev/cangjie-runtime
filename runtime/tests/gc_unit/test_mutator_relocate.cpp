// Mutator self-relocate policy (zRelocate.cpp:382-416, zForwarding.inline.hpp:267-304).
// Unpublished table miss = keep from, never wait.

#include <atomic>
#include <thread>

#include "Heap/Allocator/ForwardingEntry.h"
#include "Heap/Collector/ZForwardingLife.h"
#include "Heap/Verify/MutatorRelocate.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// Third time today a test has asserted a switch's *value*, so it is worth saying why that is wrong
// rather than just deleting the lines. A switch exists to be turned off -- to isolate a failure, to
// ship without a half-finished mechanism, to bisect. A test that fails when it is off makes it
// un-turn-off-able, and today that cost two builds: StayYoung.PolicyOn went red when adaptive
// tenuring was correctly disabled, and an earlier version of kPageAgeAdaptiveTenuring short-circuited
// the very computation its own tests checked.
//
// What is worth pinning is the decision the switch feeds, which holds either way.
GC_TEST(MutatorRelocate, UnpublishedTargetIsKeptRatherThanWaitedFor)
{
    // The answer that replaced the spin: a target that is not published yet is not a reason to wait,
    // because nothing was ever enqueued to make it appear. Keep the from-copy, which is what
    // ZForwarding::find returning null means for a mutator (zForwarding.inline.hpp:248-252).
    GC_EXPECT_TRUE(MutatorRelocate::AnswerUnpublished(false, true, false) ==
                   MutatorRelocate::UnpublishedAnswer::KeepFrom);
}

GC_TEST(MutatorRelocate, TwoThreadsInsertSameFromLoserTakesWinner)
{
    constexpr MAddress kStart = 0x1000;
    ForwardingEntries* tab = ForwardingEntries::Create(8, kStart, 0);
    GC_EXPECT_TRUE(tab != nullptr);
    const MAddress from = kStart + 16;
    const MAddress toA = 0x2000;
    const MAddress toB = 0x3000;
    std::atomic<int> ready{ 0 };
    std::atomic<MAddress> gotA{ 0 };
    std::atomic<MAddress> gotB{ 0 };
    std::thread t1([&]() {
        ready.fetch_add(1);
        while (ready.load() < 2) {
        }
        gotA.store(tab->insert(from, toA));
    });
    std::thread t2([&]() {
        ready.fetch_add(1);
        while (ready.load() < 2) {
        }
        gotB.store(tab->insert(from, toB));
    });
    t1.join();
    t2.join();
    const MAddress a = gotA.load();
    const MAddress b = gotB.load();
    GC_EXPECT_TRUE(a == toA || a == toB);
    GC_EXPECT_EQ(a, b);
    GC_EXPECT_EQ(tab->find(from), a);
    tab->Destroy();
}

GC_TEST(MutatorRelocate, ForwardedRegionNoEntryKeepsFrom)
{
    const bool tableHit = false;
    const bool regionPublished = true;
    const bool retainRefused = false;
    GC_EXPECT_TRUE(MutatorRelocate::AnswerUnpublished(tableHit, regionPublished, retainRefused) ==
                   MutatorRelocate::UnpublishedAnswer::KeepFrom);
    ForwardingEntries* tab = ForwardingEntries::Create(4, 0x1000, 0);
    GC_EXPECT_EQ(tab->find(0x1010), static_cast<MAddress>(0));
    tab->Destroy();
}

GC_TEST(MutatorRelocate, RefcountZeroRetainRefusesThenKeepFrom)
{
    std::atomic<int32_t> ref{ 0 };
    std::atomic<bool> claimed{ false };
    std::atomic<bool> done{ false };
    ZForwardingLife::ResetIdle(ref, claimed, done);
    GC_EXPECT_FALSE(ZForwardingLife::retain_page(ref, done));
    GC_EXPECT_TRUE(MutatorRelocate::AnswerUnpublished(false, false, true) ==
                   MutatorRelocate::UnpublishedAnswer::KeepFrom);
}
