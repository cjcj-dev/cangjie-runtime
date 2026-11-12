// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// HotSpot interfaceSupport.inline.hpp:87-108 against handshake.cpp:691-733:
// the owner leaves and re-enters the safe state without a lock while remote
// handshakers claim the product HandshakeState.

#include "gc_unittest.hpp"

#include "CangjieRuntime.h"
#include "Common/Runtime.h"
#include "Mutator/Handshake.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
using namespace MapleRuntime;

class NativeTransitionHost final : public Runtime {
public:
    NativeTransitionHost()
    {
        runtime = this;
        mutatorManager = &manager;
        manager.Init();
        tls = ThreadLocal::GetThreadLocalData();
        savedPoll = tls->GetPollWord();
        manager.RegisterMarkFlushThread(tls);
    }
    ~NativeTransitionHost() override
    {
        manager.UnregisterMarkFlushThread(tls);
        tls->SetPollWord(savedPoll);
        runtime = nullptr;
    }
    RuntimeParam GetRuntimeParam() const override { return {}; }
    void SetGCThreshold(uint64_t) override {}

private:
    MutatorManager manager;
    ThreadLocalData* tls = nullptr;
    uintptr_t savedPoll = 0;
};

struct Shared {
    std::atomic<std::thread::id> owner {};
    // Set by the owner for as long as it runs outside the safe state.
    std::atomic<int> ownerRunning { 0 };
    // Set by a remote handshaker for as long as its closure runs.
    std::atomic<int> remoteActive { 0 };
    std::atomic<uint64_t> remoteWhileRunning { 0 };
    std::atomic<uint64_t> runningWhileRemote { 0 };
    std::atomic<uint64_t> byOwner { 0 };
    std::atomic<uint64_t> byRemote { 0 };
};

class Observe final : public HandshakeClosure {
public:
    explicit Observe(Shared& shared) : HandshakeClosure("native-transition-observe"), shared_(shared) {}
    void do_thread(Mutator*) override
    {
        if (std::this_thread::get_id() == shared_.owner.load()) {
            shared_.byOwner.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        shared_.remoteActive.fetch_add(1);
        // Long enough for the owner to reach leave_safe while this runs.
        for (int i = 0; i < 20000; ++i) {
            if (shared_.ownerRunning.load() != 0) {
                shared_.remoteWhileRunning.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        shared_.remoteActive.fetch_sub(1);
        shared_.byRemote.fetch_add(1, std::memory_order_relaxed);
    }

private:
    Shared& shared_;
};
} // namespace

// The owner's unsafe window opens as soon as leave_safe returns, before it
// takes lock_ for anything else: that return alone must exclude a remote
// closure. The rest follows DoLeaveSaferegion. No operation may be lost while
// the owner keeps crossing.
GC_TEST(HandshakeNativeTransition, LockFreeCrossingExcludesRemoteHandshaker)
{
    NativeTransitionHost host;
    HandshakeState state(nullptr);
    Shared shared;
    constexpr size_t requesters = 3;
    constexpr uint64_t perRequester = 20000;
    std::atomic<size_t> finished { 0 };
    std::atomic<uint64_t> crossings { 0 };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);

    std::thread owner([&] {
        shared.owner.store(std::this_thread::get_id());
        while (finished.load(std::memory_order_acquire) != requesters) {
            state.leave_safe();
            shared.ownerRunning.store(1);
            if (shared.remoteActive.load() != 0) {
                shared.runningWhileRemote.fetch_add(1, std::memory_order_relaxed);
            }
            shared.ownerRunning.store(0);
            if (state.has_operation()) { (void)state.process_by_self(); }
            state.enter_safe();
            crossings.fetch_add(1, std::memory_order_relaxed);
            // Stay inside the safe state long enough for a remote claim.
            for (int i = 0; i < 512 && shared.remoteActive.load() == 0; ++i) {}
        }
    });
    while (shared.owner.load() == std::thread::id()) { std::this_thread::yield(); }

    std::vector<std::thread> threads;
    for (size_t r = 0; r < requesters; ++r) {
        threads.emplace_back([&, r] {
            Observe closure(shared);
            for (uint64_t i = 0; i < perRequester; ++i) {
                HandshakeOperation op(&closure, nullptr);
                state.add_operation(&op);
                while (!op.is_completed()) {
                    (void)state.try_process();
                    if (std::chrono::steady_clock::now() > deadline) {
                        std::fprintf(stderr, "NATIVE_TRANSITION_LOST_OPERATION requester=%zu op=%lu\n", r,
                                     static_cast<unsigned long>(i));
                        std::abort();
                    }
                }
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }
    for (auto& thread : threads) { thread.join(); }
    owner.join();

    const uint64_t total = requesters * perRequester;
    std::fprintf(stderr,
                 "NATIVE_TRANSITION crossings=%lu byOwner=%lu byRemote=%lu remoteWhileRunning=%lu "
                 "runningWhileRemote=%lu pending=%d\n",
                 static_cast<unsigned long>(crossings.load()), static_cast<unsigned long>(shared.byOwner.load()),
                 static_cast<unsigned long>(shared.byRemote.load()),
                 static_cast<unsigned long>(shared.remoteWhileRunning.load()),
                 static_cast<unsigned long>(shared.runningWhileRemote.load()), state.has_operation());
    GC_EXPECT_EQ(shared.byOwner.load() + shared.byRemote.load(), total);
    GC_EXPECT_EQ(shared.remoteWhileRunning.load(), static_cast<uint64_t>(0));
    GC_EXPECT_EQ(shared.runningWhileRemote.load(), static_cast<uint64_t>(0));
    GC_EXPECT_FALSE(state.has_operation());
}

// The state the owner publishes is what a handshaker acts on: an operation
// queued while the owner is outside the safe state is declined, and the same
// operation is executed remotely once the owner is back inside it.
GC_TEST(HandshakeNativeTransition, HandshakerFollowsPublishedState)
{
    NativeTransitionHost host;
    HandshakeState state(nullptr);
    GC_EXPECT_TRUE(state.observed_safe());
    state.leave_safe();
    GC_EXPECT_FALSE(state.observed_safe());
    GC_EXPECT_FALSE(state.try_process());
    state.enter_safe();
    GC_EXPECT_TRUE(state.observed_safe());

    Shared shared;
    Observe closure(shared);
    HandshakeOperation op(&closure, nullptr);
    state.leave_safe();
    state.add_operation(&op);
    GC_EXPECT_FALSE(state.try_process());
    GC_EXPECT_FALSE(op.is_completed());
    state.enter_safe();
    GC_EXPECT_TRUE(state.try_process());
    GC_EXPECT_TRUE(op.is_completed());
    GC_EXPECT_FALSE(state.has_operation());
}

// PublishLeaveSaferegion pairs with an observer that announces itself under
// MutatorLock before it reads the saferegion state (CjFileLoader image scan,
// VisitStackRoots). While an announced observer holds the lock, the owner
// publishes the unsafe state but does not return; an observer that announces
// after the owner published declines.
GC_TEST(HandshakeNativeTransition, LeaveWaitsForAnnouncedObserver)
{
    NativeTransitionHost host;
    HandshakeState state(nullptr);
    Mutator owner;
    owner.MutatorLock();
    owner.IncObserver();
    const bool observerSawSafe = owner.InSaferegion();
    std::atomic<bool> returned { false };
    std::thread leaver([&] {
        owner.PublishLeaveSaferegion(&state);
        returned.store(true);
    });
    while (state.observed_safe()) { std::this_thread::yield(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool returnedWhileObserved = returned.load();
    owner.DecObserver();
    owner.MutatorUnlock();
    leaver.join();
    owner.MutatorLock();
    owner.IncObserver();
    const bool lateObserverSawSafe = owner.InSaferegion();
    owner.DecObserver();
    owner.MutatorUnlock();
    std::fprintf(stderr,
                 "MUTATOR_LEAVE_OBSERVER saw_safe=%d returned_while_observed=%d returned=%d late_saw_safe=%d "
                 "count=%zu\n",
                 observerSawSafe, returnedWhileObserved, returned.load(), lateObserverSawSafe,
                 owner.GetObserverCount());
    GC_EXPECT_TRUE(observerSawSafe);
    GC_EXPECT_FALSE(returnedWhileObserved);
    GC_EXPECT_TRUE(returned.load());
    GC_EXPECT_FALSE(lateObserverSawSafe);
    GC_EXPECT_EQ(owner.GetObserverCount(), static_cast<size_t>(0));
}
