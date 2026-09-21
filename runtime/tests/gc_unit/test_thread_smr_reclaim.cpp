// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

// threadSMR.cpp:932-946: a reclaim scan must gather every hazard pointer
// first, take an acquire barrier, and only then read the nested reference
// counters. Reading the counters before the hazards can miss a nested
// handoff (threadSMR.cpp:498-512: bump the counter, clear the hazard,
// acquire the new list) that starts after the counter read but completes
// before the hazard scan, freeing a list the nested handle still protects.
#include "gc_unittest.hpp"

#if defined(__linux__) && defined(MRT_TESTABLE_INTERNALS)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include "Mutator/ThreadSMR.h"

namespace {
using MapleRuntime::Mutator;
using MapleRuntime::ThreadsListHandle;
using MapleRuntime::ThreadsSMRSupport;

struct Interleave {
    std::atomic<bool> registered{false};
    std::atomic<bool> handoffGo{false};
    std::atomic<bool> handoffDone{false};
    std::atomic<bool> release{false};
};
Interleave* activeInterleave = nullptr;

Mutator* Identity()
{
    return reinterpret_cast<Mutator*>(static_cast<uintptr_t>(0x6a0000000010ULL));
}

void Await(const std::atomic<bool>& signal)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!signal.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "THREAD_SMR_SETUP_TIMEOUT target_executed=0\n");
            std::_Exit(2);
        }
        std::this_thread::yield();
    }
}

void ScanBoundary()
{
    // Fire only inside the publication that retired Identity()'s list. An
    // unrelated add/remove between arming and our remove_thread must keep
    // the breakpoint armed, or the interleave below stops being deterministic.
    if (ThreadsSMRSupport::get_java_thread_list()->includes(Identity())) { return; }
    // The product holds its registration lock here. The already registered
    // executor needs no lock to promote its outer handle and acquire inner.
    ThreadsSMRSupport::SetReclaimScanBreakpoint(nullptr);
    activeInterleave->handoffGo.store(true, std::memory_order_release);
    Await(activeInterleave->handoffDone);
    std::fprintf(stderr, "THREAD_SMR_SCAN_BOUNDARY handoff_complete=1\n");
}
}

GC_RUNTIME_OTHER_VM_TEST(ThreadSMRReclaim, NestedHandoffRetainsRetiredList)
{
    // Identity only: SMR membership never dereferences this address.
    Mutator* identity = Identity();
    ThreadsSMRSupport::add_thread(identity);
    auto state = std::make_shared<Interleave>();
    std::thread handoff([state] {
        ThreadsListHandle outer;
        // Registration AND outer acquisition complete before any query/scan.
        state->registered.store(true, std::memory_order_release);
        Await(state->handoffGo);
        ThreadsListHandle inner;
        state->handoffDone.store(true, std::memory_order_release);
        Await(state->release);
    });
    Await(state->registered);
    activeInterleave = state.get();
    ThreadsSMRSupport::SetReclaimScanBreakpoint(ScanBoundary);
    // Real publication/reclamation entrance. Correct arm captures outer's
    // hazard before the breakpoint; old-order cut reads nested=0 before it.
    // During the breakpoint the executor promotes outer then clears hazard
    // and acquires the new list (which no longer contains identity).
    ThreadsSMRSupport::remove_thread(identity);
    Await(state->handoffDone);
    // All executor registrations are complete; the only auxiliary executor
    // is latched before handle release. No executor/retired-list writer can
    // run until this quiescent query has finished (threadSMR.cpp:985-986).
    const bool retained = ThreadsSMRSupport::is_a_protected_JavaThread(identity);
    std::fprintf(stderr, "THREAD_SMR_RECLAIM_TARGET executed=1 handoffs=1 protected=%d\n", retained ? 1 : 0);
    std::fflush(stderr);
    if (!retained) {
        // The cut has reclaimed outer's list. Do not release that invalid
        // handle; the isolated OTHER_VM process exits after the assertion.
        // Shared ownership keeps the latch alive until process termination.
        handoff.detach();
        GC_EXPECT_TRUE(retained);
        return;
    }
    state->release.store(true, std::memory_order_release);
    handoff.join();
    activeInterleave = nullptr;
    GC_EXPECT_TRUE(retained);
}
#endif
