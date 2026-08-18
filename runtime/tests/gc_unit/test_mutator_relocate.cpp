// Relocate queue + insert CAS (zRelocate.cpp:134-151, :382-416, zForwarding.inline.hpp:267-304).

#include <atomic>
#include <chrono>
#include <thread>

#include "Heap/Allocator/ForwardingEntry.h"
#include "Heap/Collector/RelocateQueue.h"
#include "Heap/Collector/ZForwardingLife.h"
#include "Heap/Verify/MutatorRelocate.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(MutatorRelocate, SelfRelocateAndQueueWaitAreOn)
{
    GC_EXPECT_TRUE(MutatorRelocate::kMutatorSelfRelocate);
    GC_EXPECT_TRUE(RelocateWaitCore::kWaitUsesConditionNotYield);
    GC_EXPECT_FALSE(MutatorRelocate::kUnpublishedMeansKeepFrom);
    GC_EXPECT_TRUE(MutatorRelocate::AnswerUnpublished(false, true, false) ==
                   MutatorRelocate::UnpublishedAnswer::AssertForwarded);
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

GC_TEST(MutatorRelocate, AddAndWaitReturnsIfAlreadyDone)
{
    RelocateWaitCore q;
    std::atomic<bool> done{ true };
    int key = 1;
    q.add_and_wait(&key, done);
    GC_EXPECT_EQ(q.Enqueued(), static_cast<uint64_t>(0));
    GC_EXPECT_EQ(q.Waited(), static_cast<uint64_t>(0));
}

GC_TEST(MutatorRelocate, AddAndWaitWakesWhenDoneNotified)
{
    RelocateWaitCore q;
    std::atomic<bool> done{ false };
    int key = 2;
    std::atomic<bool> entered{ false };
    std::atomic<bool> finished{ false };
    std::thread waiter([&]() {
        entered.store(true, std::memory_order_release);
        q.add_and_wait(&key, done);
        finished.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    GC_EXPECT_FALSE(finished.load(std::memory_order_acquire));
    GC_EXPECT_TRUE(q.Enqueued() >= 1);
    done.store(true, std::memory_order_release);
    q.on_done();
    waiter.join();
    GC_EXPECT_TRUE(finished.load(std::memory_order_acquire));
}

GC_TEST(MutatorRelocate, PollClaimsEnqueuedPage)
{
    RelocateWaitCore q;
    std::atomic<bool> done{ false };
    int key = 3;
    std::thread waiter([&]() { q.add_and_wait(&key, done); });
    void* claimed = nullptr;
    for (int i = 0; i < 10000 && claimed == nullptr; ++i) {
        claimed = q.poll_and_claim();
        std::this_thread::yield();
    }
    GC_EXPECT_TRUE(claimed == static_cast<void*>(&key));
    done.store(true, std::memory_order_release);
    q.on_done();
    waiter.join();
}

GC_TEST(MutatorRelocate, RefcountZeroRetainRefuses)
{
    std::atomic<int32_t> ref{ 0 };
    std::atomic<bool> claimed{ false };
    std::atomic<bool> done{ false };
    ZForwardingLife::ResetIdle(ref, claimed, done);
    GC_EXPECT_FALSE(ZForwardingLife::retain_page(ref, done));
}
