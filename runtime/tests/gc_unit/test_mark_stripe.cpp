// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <new>
#include <thread>
#include <vector>

#include "Heap/Collector/MarkStripe.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;

namespace {
constexpr uintptr_t ENTRY_BASE = 0x200000;
constexpr uintptr_t ENTRY_STEP = 32;

MarkStripeStack* StackWithOne(uintptr_t value)
{
    MarkStripeStack* stack = MarkStripeStack::Create(true);
    GC_EXPECT_TRUE(stack != nullptr);
    stack->Push(MarkStackEntry::MarkAndFollow(reinterpret_cast<BaseObject*>(value)));
    return stack;
}
} // namespace

// Deterministic positive control for the exact race described in
// ZGC zMarkingSMR.cpp:34-56. The first logical node is removed, its address is
// reused for another node with a poisoned next link, and the paused pop wins an
// ABA CAS. If this test does not corrupt the head, it has stopped exercising ABA.
GC_TEST(MarkStripe, NoSmrPositiveControlCorruptsHead)
{
    MarkStripeStack* firstStack = StackWithOne(ENTRY_BASE);
    MarkStripeStack* tailStack = StackWithOne(ENTRY_BASE + ENTRY_STEP);
    MarkStripeStack* reusedStack = StackWithOne(ENTRY_BASE + 2 * ENTRY_STEP);

    void* storage = ::operator new(sizeof(MarkStripeStackListNode));
    auto* first = new (storage) MarkStripeStackListNode(firstStack);
    auto* tail = new MarkStripeStackListNode(tailStack);
    first->SetNext(tail);
    std::atomic<MarkStripeStackListNode*> head{ first };
    auto* const poison = reinterpret_cast<MarkStripeStackListNode*>(static_cast<uintptr_t>(0x5a5a5a00));
    std::atomic<unsigned> stage{ 0 };
    std::atomic<bool> abaCasWon{ false };

    std::thread pausedPop([&]() {
        MarkStripeStackListNode* observed = head.load(std::memory_order_relaxed);
        stage.store(1, std::memory_order_release);
        while (stage.load(std::memory_order_acquire) != 2) {
            std::this_thread::yield();
        }
        // Without a hazard, this is no longer the next pointer belonging to
        // the logical node that was observed above.
        MarkStripeStackListNode* next = observed->Next();
        abaCasWon.store(head.compare_exchange_strong(observed, next, std::memory_order_relaxed),
                        std::memory_order_release);
    });

    std::thread recycler([&]() {
        while (stage.load(std::memory_order_acquire) != 1) {
            std::this_thread::yield();
        }
        MarkStripeStackListNode* expected = first;
        GC_EXPECT_TRUE(head.compare_exchange_strong(expected, tail, std::memory_order_relaxed));
        first->~MarkStripeStackListNode();
        auto* reincarnated = new (storage) MarkStripeStackListNode(reusedStack);
        reincarnated->SetNext(poison); // explicit poison: a successful stale CAS installs this.
        head.store(reincarnated, std::memory_order_release); // A -> B -> A
        stage.store(2, std::memory_order_release);
    });

    pausedPop.join();
    recycler.join();
    GC_EXPECT_TRUE(abaCasWon.load(std::memory_order_acquire));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(head.load(std::memory_order_relaxed)),
                 reinterpret_cast<uintptr_t>(poison));

    static_cast<MarkStripeStackListNode*>(storage)->~MarkStripeStackListNode();
    ::operator delete(storage);
    delete tail;
    MarkStripeStack::Destroy(firstStack);
    MarkStripeStack::Destroy(tailStack);
    MarkStripeStack::Destroy(reusedStack);
}

GC_TEST(MarkStripe, SmrHazardPreventsAbaReuse)
{
    MarkingSMR smr(2);
    MarkStripeStack* firstStack = StackWithOne(ENTRY_BASE);
    MarkStripeStack* tailStack = StackWithOne(ENTRY_BASE + ENTRY_STEP);
    MarkStripeStack* newStack = StackWithOne(ENTRY_BASE + 2 * ENTRY_STEP);
    auto* first = new MarkStripeStackListNode(firstStack);
    auto* tail = new MarkStripeStackListNode(tailStack);
    auto* replacement = new MarkStripeStackListNode(newStack);
    first->SetNext(tail);
    std::atomic<MarkStripeStackListNode*> head{ first };

    MarkStripeStackListNode* observed = head.load(std::memory_order_relaxed);
    smr.Hazard(0).store(observed, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(head.load(std::memory_order_acquire)),
                 reinterpret_cast<uintptr_t>(observed));

    MarkStripeStackListNode* expected = first;
    GC_EXPECT_TRUE(head.compare_exchange_strong(expected, tail, std::memory_order_relaxed));
    smr.Retire(1, first);
    smr.Reclaim(1);
    GC_EXPECT_EQ(smr.PendingCount(1), static_cast<size_t>(1));

    replacement->SetNext(tail);
    head.store(replacement, std::memory_order_release);
    GC_EXPECT_NE(reinterpret_cast<uintptr_t>(head.load(std::memory_order_acquire)),
                 reinterpret_cast<uintptr_t>(observed));
    smr.Hazard(0).store(nullptr, std::memory_order_release);
    smr.Reclaim(1);
    GC_EXPECT_EQ(smr.PendingCount(1), static_cast<size_t>(0));

    delete replacement;
    delete tail;
    MarkStripeStack::Destroy(firstStack);
    MarkStripeStack::Destroy(tailStack);
    MarkStripeStack::Destroy(newStack);
}

GC_TEST(MarkStripe, ConcurrentGlobalStealIsLiveAndLossless)
{
    constexpr size_t workers = 8;
    constexpr size_t stripeCount = 8;
    constexpr size_t entries = 16384;
    MarkStripeSet stripes(stripeCount);
    MarkingSMR smr(workers);
    MarkThreadLocalStacks seed(stripeCount);
    for (size_t i = 0; i < entries; ++i) {
        const uintptr_t value = ENTRY_BASE + i * ENTRY_STEP;
        // Put all initial work on one shared stripe so non-owner workers must
        // take the global steal path; flush converts private tails to nodes.
        seed.Push(stripes, 0, MarkStackEntry::MarkAndFollow(reinterpret_cast<BaseObject*>(value)), true);
    }
    GC_EXPECT_TRUE(seed.Flush(stripes, true));

    std::unique_ptr<std::atomic<unsigned>[]> seen(new std::atomic<unsigned>[entries]);
    for (size_t i = 0; i < entries; ++i) {
        seen[i].store(0, std::memory_order_relaxed);
    }
    std::atomic<size_t> remaining{ entries };
    std::atomic<size_t> stealSuccess{ 0 };
    std::atomic<size_t> ready{ 0 };
    std::atomic<bool> invalidEntry{ false };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (size_t workerId = 0; workerId < workers; ++workerId) {
        threads.emplace_back([&, workerId]() {
            MarkContext context(workers, workerId, stripes);
            // Reserve one shared chunk per non-owner before anyone drains.
            // A start barrier alone is insufficient: the owner could still
            // consume everything before another worker gets scheduled.
            if (workerId != 0) {
                MarkStripeStack* stack = stripes.At(0).StealStack(smr, workerId);
                if (stack != nullptr) {
                    context.Stacks().Install(context.StripeId(), stack);
                    stealSuccess.fetch_add(1, std::memory_order_relaxed);
                }
            }
            ready.fetch_add(1, std::memory_order_release);
            while (ready.load(std::memory_order_acquire) != workers) {
                std::this_thread::yield();
            }
            MarkStackEntry entry;
            while (remaining.load(std::memory_order_acquire) != 0 &&
                   !invalidEntry.load(std::memory_order_relaxed) &&
                   std::chrono::steady_clock::now() < deadline) {
                if (context.Stacks().Pop(smr, workerId, stripes, context.StripeId(), entry)) {
                    const uintptr_t value = reinterpret_cast<uintptr_t>(entry.object());
                    const size_t index = (value - ENTRY_BASE) / ENTRY_STEP;
                    // Report failures after joining; an assertion exception in
                    // a worker would bypass the losslessness diagnostic.
                    if (value < ENTRY_BASE || (value - ENTRY_BASE) % ENTRY_STEP != 0 ||
                        index >= entries || seen[index].fetch_add(1, std::memory_order_relaxed) != 0) {
                        invalidEntry.store(true, std::memory_order_relaxed);
                        break;
                    }
                    remaining.fetch_sub(1, std::memory_order_release);
                    continue;
                }

                bool stole = false;
                for (size_t victim = stripes.Next(context.StripeId()); victim != context.StripeId();
                     victim = stripes.Next(victim)) {
                    MarkStripeStack* stack = context.Stacks().StealLocal(victim);
                    if (stack == nullptr) {
                        stack = stripes.At(victim).StealStack(smr, workerId);
                    }
                    if (stack != nullptr) {
                        context.Stacks().Install(context.StripeId(), stack);
                        stealSuccess.fetch_add(1, std::memory_order_relaxed);
                        stole = true;
                        break;
                    }
                }
                if (!stole) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    std::printf("MARK_STRIPE_DRAIN remaining=%zu invalid=%d steals=%zu ready=%zu\n",
                remaining.load(), invalidEntry.load(), stealSuccess.load(), ready.load());
    GC_EXPECT_FALSE(invalidEntry.load());
    GC_EXPECT_EQ(remaining.load(), static_cast<size_t>(0));
    for (size_t i = 0; i < entries; ++i) {
        GC_EXPECT_EQ(seen[i].load(std::memory_order_relaxed), 1u);
    }
    GC_EXPECT_TRUE(stealSuccess.load(std::memory_order_relaxed) >= workers - 1);
    GC_EXPECT_TRUE(stripes.IsEmpty());
}

// Empty-victim failure is an explicit input, not a scheduling requirement on
// the concurrent drain. ZGC zMark.cpp:511-528 also permits either steal result.
GC_TEST(MarkStripe, GlobalStealReportsEmptyAfterDrain)
{
    MarkStripe stripe;
    MarkingSMR smr(1);
    MarkStripeStack* const empty = stripe.StealStack(smr, 0);
    GC_EXPECT_TRUE(empty == nullptr);
    // Positive control: the same consumer must return the published payload.
    stripe.PublishStack(StackWithOne(ENTRY_BASE), true);
    MarkStripeStack* const stack = stripe.StealStack(smr, 0);
    GC_EXPECT_TRUE(stack != nullptr);
    const uintptr_t value = reinterpret_cast<uintptr_t>(stack->Pop().object());
    MarkStripeStack::Destroy(stack);
    GC_EXPECT_EQ(value, ENTRY_BASE);
    GC_EXPECT_TRUE(stripe.StealStack(smr, 0) == nullptr);
}
