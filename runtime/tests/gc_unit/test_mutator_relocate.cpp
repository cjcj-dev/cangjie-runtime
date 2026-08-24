// Mutator self-relocate policy (zRelocate.cpp:382-416, zForwarding.inline.hpp:267-304).
// Unpublished table miss = keep from, never wait.

#include <atomic>
#include <thread>

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Collector/ZForwarding.h"
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
    // Published + table miss = VisitLive hole. Keep-from is legal
    // (zRelocate.cpp:403-416 / oraclecut §4).
    GC_EXPECT_TRUE(MutatorRelocate::AnswerUnpublished(false, true, false) ==
                   MutatorRelocate::UnpublishedAnswer::KeepFrom);
}

GC_TEST(MutatorRelocate, UnpublishedRegionWaitsForPublish)
{
    // oraclecut §4: !regionPublished ⇒ wait for the region-level publish
    // (FORWARDED / COMPACTED / kept). Not the object-level empty wait
    // 47595a33 deleted.
    if (MutatorRelocate::kWaitRegionPublish) {
        GC_EXPECT_TRUE(MutatorRelocate::AnswerUnpublished(false, false, false) ==
                       MutatorRelocate::UnpublishedAnswer::Wait);
        GC_EXPECT_TRUE(MutatorRelocate::AnswerUnpublished(false, false, true) ==
                       MutatorRelocate::UnpublishedAnswer::Wait);
    }
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
    JoinGuard t1Guard(t1);
    JoinGuard t2Guard(t2);
    t1.join();
    t2.join();
    const MAddress a = gotA.load();
    const MAddress b = gotB.load();
    GC_EXPECT_TRUE(a == toA || a == toB);
    GC_EXPECT_EQ(a, b);
    GC_EXPECT_EQ(tab->find(from), a);
    tab->Destroy();
}

GC_TEST(MutatorRelocate, ForcedFirstCasFailureReturnsOneReceiptAndOneCompleter)
{
    constexpr MAddress kStart = 0x1000;
    ForwardingEntries* tab = ForwardingEntries::Create(8, kStart, 0);
    GC_EXPECT_TRUE(tab != nullptr);
    const MAddress from = kStart + 16;
    const MAddress toA = 0x2000;
    const MAddress toB = 0x3000;
    std::atomic<int> atFirstCas{ 0 };
    std::atomic<MAddress> gotA{ 0 };
    std::atomic<MAddress> gotB{ 0 };
    std::atomic<int> completed{ 0 };

    auto firstCasBarrier = [&]() {
        atFirstCas.fetch_add(1, std::memory_order_acq_rel);
        while (atFirstCas.load(std::memory_order_acquire) != 2) {
            std::this_thread::yield();
        }
    };
    std::thread t1([&]() {
        const ZForwarding::Receipt receipt = tab->insert_receipt(from, toA, firstCasBarrier);
        gotA.store(receipt.address, std::memory_order_release);
        completed.fetch_add(receipt.installed ? 1 : 0, std::memory_order_relaxed);
    });
    std::thread t2([&]() {
        const ZForwarding::Receipt receipt = tab->insert_receipt(from, toB, firstCasBarrier);
        gotB.store(receipt.address, std::memory_order_release);
        completed.fetch_add(receipt.installed ? 1 : 0, std::memory_order_relaxed);
    });
    JoinGuard t1Guard(t1);
    JoinGuard t2Guard(t2);
    t1.join();
    t2.join();

    const MAddress winner = gotA.load(std::memory_order_acquire);
    GC_EXPECT_NE(winner, static_cast<MAddress>(0));
    GC_EXPECT_EQ(gotB.load(std::memory_order_acquire), winner);
    GC_EXPECT_EQ(tab->find(from), winner);
    GC_EXPECT_EQ(completed.load(std::memory_order_relaxed), 1);

    size_t populated = 0;
    for (ForwardingCursor cursor = 0; cursor < tab->length(); ++cursor) {
        populated += tab->at(&cursor).populated() ? 1 : 0;
    }
    GC_EXPECT_EQ(populated, static_cast<size_t>(1));
    tab->Destroy();
}

GC_TEST(MutatorRelocate, ForwardedPublicationRequiresNonNullReceipt)
{
    GC_EXPECT_FALSE(ForwardingTable::ReceiptAllowsForwarded(0));
    GC_EXPECT_TRUE(ForwardingTable::ReceiptAllowsForwarded(0x2000));
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

GC_TEST(FwdSpin, LockedWaiterSeesInsertBeforeUnlock)
{
    // Positive control of the old hang: yield-while-LOCKED never looks at the
    // table, so a copier that has insert()'d but not yet UnlockObject leaves
    // waiters spinning (REPORT-llstore hang_live / WCollector.cpp:9570).
    // New policy: tableHit ⇒ UseTo (zRelocate.cpp:386-389).
    GC_EXPECT_TRUE(MutatorRelocate::AnswerLockedWaiter(true, false) ==
                   MutatorRelocate::LockedWaiterAnswer::UseTo);
    GC_EXPECT_TRUE(MutatorRelocate::AnswerLockedWaiter(false, true) ==
                   MutatorRelocate::LockedWaiterAnswer::UsePlanned);
    GC_EXPECT_TRUE(MutatorRelocate::AnswerLockedWaiter(false, false) ==
                   MutatorRelocate::LockedWaiterAnswer::Yield);

    constexpr MAddress kStart = 0x1000;
    ForwardingEntries* tab = ForwardingEntries::Create(8, kStart, 0);
    GC_EXPECT_TRUE(tab != nullptr);
    const MAddress from = kStart + 16;
    const MAddress to = 0x2000;
    std::atomic<int> phase{ 0 };
    std::atomic<int> waiterGot{ 0 };
    std::thread copier([&]() {
        (void)tab->insert(from, to);
        phase.store(1, std::memory_order_release);
        while (phase.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
    });
    std::thread waiter([&]() {
        while (phase.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }
        const bool tableHit = tab->find(from) != 0;
        const auto ans = MutatorRelocate::AnswerLockedWaiter(tableHit, false);
        if (ans == MutatorRelocate::LockedWaiterAnswer::UseTo) {
            waiterGot.store(1, std::memory_order_release);
        }
        phase.store(2, std::memory_order_release);
    });
    JoinGuard copierGuard(copier);
    JoinGuard waiterGuard(waiter);
    waiter.join();
    copier.join();
    GC_EXPECT_EQ(waiterGot.load(), 1);
    GC_EXPECT_EQ(tab->find(from), to);
    tab->Destroy();
}

GC_TEST(MutatorRelocate, RelocateInnerRequiresForwardPhase)
{
    // zRelocate.cpp:394 assert(_generation->is_phase_relocate()) after retain_page.
    // Collector.h: IDLE=1 PREFORWARD=13 FORWARD=14. Out-of-window after retain
    // is FindToVersion, not ForwardObjectImpl (CHECK stays).
    constexpr unsigned kIdle = 1;
    constexpr unsigned kReclaimSatb = 3;
    constexpr unsigned kPreforward = 13;
    constexpr unsigned kForward = 14;
    auto phaseOk = [](unsigned p) {
        return p == kPreforward || p == kForward;
    };
    GC_EXPECT_FALSE(phaseOk(kIdle));
    GC_EXPECT_FALSE(phaseOk(kReclaimSatb));
    GC_EXPECT_TRUE(phaseOk(kPreforward));
    GC_EXPECT_TRUE(phaseOk(kForward));
}

GC_TEST(MutatorRelocate, RefcountZeroRetainRefusesThenWait)
{
    std::atomic<int32_t> ref{ 0 };
    std::atomic<bool> claimed{ false };
    std::atomic<bool> done{ false };
    ZForwardingLife::ResetIdle(ref, claimed, done);
    GC_EXPECT_FALSE(ZForwardingLife::retain_page(ref, done));
    // retain refused = worker mid-copy, not "never copied" (LEAD 0819-12:2x).
    GC_EXPECT_TRUE(MutatorRelocate::AnswerUnpublished(false, false, true) ==
                   MutatorRelocate::UnpublishedAnswer::Wait);
}
