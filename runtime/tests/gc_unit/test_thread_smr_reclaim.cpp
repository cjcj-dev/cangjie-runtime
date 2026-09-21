// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

// threadSMR.cpp:932-946: a reclaim scan must gather every hazard pointer
// first, take an acquire barrier, and only then read the nested reference
// counters. Reading the counters before the hazards can miss a nested
// handoff that bumps the counter after the scan read it but clears the
// hazard before the scan reads it, freeing a list the nested handle still
// protects (ZGC threadSMR.cpp:498-512 for the handoff order).

#include "gc_unittest.hpp"

#if defined(__linux__)
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

#include "Mutator/ThreadSMR.h"

namespace {
using MapleRuntime::Mutator;
using MapleRuntime::ThreadsListHandle;
using MapleRuntime::ThreadsSMRSupport;

constexpr size_t kParkedExecutors = 1024;
constexpr int kRounds = 512;

struct ReclaimInterleave {
    std::atomic<int> roundTodo{0};
    std::atomic<int> outerReady{0};
    std::atomic<int> handoffGo{0};
    std::atomic<int> handoffDone{0};
    std::atomic<int> releaseGo{0};
    std::atomic<int> releaseDone{0};
    std::atomic<bool> churnPause{true};
    std::atomic<bool> churnActive{false};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> churnScans{0};
    std::atomic<uint64_t> windowScans{0};
    std::atomic<bool> inWindow{false};
};

bool WaitForSignal(const std::function<bool()>& done)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done() && std::chrono::steady_clock::now() < end) { std::this_thread::yield(); }
    return done();
}

Mutator* FakeIdentity(int round)
{
    return reinterpret_cast<Mutator*>(static_cast<uintptr_t>(0x6a0000000000ULL + 0x10ULL * (round + 1)));
}

Mutator* ChurnDummy()
{
    return reinterpret_cast<Mutator*>(static_cast<uintptr_t>(0x6a00000f0000ULL));
}

// Extra registered executors widen every reclaim scan's hazard walk so the
// vulnerable interval inside one scan spans many loads.
void ParkedExecutor(ReclaimInterleave& state)
{
    { ThreadsListHandle probe; (void)probe.length(); }
    while (!state.stop.load(std::memory_order_acquire)) { std::this_thread::yield(); }
}

// Drives the real product reclaim entry: every add/remove publishes a new
// membership list and scans the retired chain.
void ChurnLoop(ReclaimInterleave& state)
{
    bool added = false;
    while (!state.stop.load(std::memory_order_acquire)) {
        if (state.churnPause.load(std::memory_order_acquire)) {
            state.churnActive.store(false, std::memory_order_release);
            std::this_thread::yield();
            continue;
        }
        state.churnActive.store(true, std::memory_order_release);
        if (added) {
            ThreadsSMRSupport::remove_thread(ChurnDummy());
        } else {
            ThreadsSMRSupport::add_thread(ChurnDummy());
        }
        added = !added;
        state.churnScans.fetch_add(1, std::memory_order_relaxed);
        if (state.inWindow.load(std::memory_order_relaxed)) {
            state.windowScans.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Holds an outer handle on the round list, then performs the nested
// acquisition (threadSMR.cpp:498-512) while reclaim scans are in flight.
void HandoffLoop(ReclaimInterleave& state)
{
    for (int round = 1;; ++round) {
        if (!WaitForSignal([&] {
                return state.roundTodo.load(std::memory_order_acquire) >= round ||
                       state.stop.load(std::memory_order_acquire);
            })) {
            return;
        }
        if (state.stop.load(std::memory_order_acquire)) { return; }
        auto* outer = new ThreadsListHandle();
        state.outerReady.store(round, std::memory_order_release);
        const bool go = WaitForSignal([&] {
            return state.handoffGo.load(std::memory_order_acquire) >= round ||
                   state.stop.load(std::memory_order_acquire);
        });
        if (!go || state.stop.load(std::memory_order_acquire)) {
            delete outer;
            return;
        }
        auto* inner = new ThreadsListHandle();
        state.handoffDone.store(round, std::memory_order_release);
        const bool release = WaitForSignal([&] {
            return state.releaseGo.load(std::memory_order_acquire) >= round ||
                   state.stop.load(std::memory_order_acquire);
        });
        if (!release || state.stop.load(std::memory_order_acquire)) {
            delete inner;
            delete outer;
            return;
        }
        delete inner;
        delete outer;
        state.releaseDone.store(round, std::memory_order_release);
    }
}
}

GC_RUNTIME_OTHER_VM_TEST(ThreadSMRReclaim, NestedHandoffRetainsRetiredList)
{
    ReclaimInterleave state;
    std::vector<std::thread> parked;
    parked.reserve(kParkedExecutors);
    for (size_t i = 0; i < kParkedExecutors; ++i) { parked.emplace_back(ParkedExecutor, std::ref(state)); }
    std::thread churn(ChurnLoop, std::ref(state));
    std::thread handoff(HandoffLoop, std::ref(state));

    int checked = 0;
    bool protectedEveryRound = true;
    for (int round = 1; round <= kRounds; ++round) {
        Mutator* identity = FakeIdentity(round);
        ThreadsSMRSupport::add_thread(identity);
        state.roundTodo.store(round, std::memory_order_release);
        const bool outerOk = WaitForSignal([&] { return state.outerReady.load(std::memory_order_acquire) >= round; });
        GC_EXPECT_TRUE(outerOk);
        ThreadsSMRSupport::remove_thread(identity);
        state.churnPause.store(false, std::memory_order_release);
        GC_EXPECT_TRUE(WaitForSignal([&] { return state.churnActive.load(std::memory_order_acquire); }));
        state.inWindow.store(true, std::memory_order_relaxed);
        state.handoffGo.store(round, std::memory_order_release);
        GC_EXPECT_TRUE(WaitForSignal([&] { return state.handoffDone.load(std::memory_order_acquire) >= round; }));
        state.inWindow.store(false, std::memory_order_relaxed);
        state.churnPause.store(true, std::memory_order_release);
        GC_EXPECT_TRUE(WaitForSignal([&] { return !state.churnActive.load(std::memory_order_acquire); }));
        // Quiescent oracle: with the nested handle alive, the retired round
        // list still protects its member. A premature reclaim unlinks it and
        // flips this product query to false.
        const bool kept = ThreadsSMRSupport::is_a_protected_JavaThread(identity);
        if (!kept) {
            std::fprintf(stderr, "THREAD_SMR_RECLAIM_TARGET executed=1 round=%d protected=0\n", round);
            std::fflush(stderr);
            protectedEveryRound = false;
            break;
        }
        ++checked;
        state.releaseGo.store(round, std::memory_order_release);
        GC_EXPECT_TRUE(WaitForSignal([&] { return state.releaseDone.load(std::memory_order_acquire) >= round; }));
    }
    state.stop.store(true, std::memory_order_release);
    state.releaseGo.store(kRounds + 1, std::memory_order_release);
    state.handoffGo.store(kRounds + 1, std::memory_order_release);
    state.roundTodo.store(kRounds + 1, std::memory_order_release);
    handoff.join();
    churn.join();
    for (auto& thread : parked) { thread.join(); }
    std::fprintf(stderr, "THREAD_SMR_RECLAIM_TARGET executed=1 checked=%d protected=%d churn=%llu window=%llu\n",
                 checked, protectedEveryRound ? 1 : 0,
                 static_cast<unsigned long long>(state.churnScans.load()),
                 static_cast<unsigned long long>(state.windowScans.load()));
    GC_EXPECT_TRUE(protectedEveryRound && checked == kRounds);
}
#endif
