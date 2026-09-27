// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Exit-detach versus the young mark-end pause flush (issue #1137).
//
// Exiting side (product): MutatorManager::DestroyRuntimeMutator ->
//   TransitMutatorToExit -> ZBarrierSet::on_thread_detach -> Heap::mark_flush
//   -> ZMark::Flush(ThreadGCData&) -> StoreBarrierBuffer::Flush ->
//   ZBarrier::mark_and_remember -> ZMark::MarkObject ->
//   MarkThreadLocalStacks::Push (Create() ... breakpoint ... Push(entry)).
// Pause side (product): ScopedStopTheWorld + ZGenerationYoung::mark_end ->
//   ZMark::TryEndYoungMark -> ZMark::TryEnd -> HandshakeFlush (world-stopped
//   branch) -> MarkStripe::PublishStack (CHECK never publish an empty mark
//   stripe stack).
//
// ZGC form: Threads::remove runs on_thread_detach while the exiting thread is
// not safepoint-safe (threads.cpp:1089-1104), and try_end flushes only
// non-Java threads (zMark.cpp:954-970), so a pause never observes the
// Create()/Push() window of an exiting owner's local stack.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "b09_runtime_fixture.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"
#include "mark_publication_fixture.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::atomic<Mutator*> g_armedOwner{nullptr};
std::atomic<bool> g_parked{false};
std::atomic<bool> g_release{false};
std::atomic<int> g_parkedInSaferegion{-1};

void ParkExitingPush()
{
    Mutator* const armed = g_armedOwner.load(std::memory_order_acquire);
    if (armed == nullptr || Mutator::GetMutator() != armed) {
        return;
    }
    // Fire once: the first stack created by this owner after arming.
    g_armedOwner.store(nullptr, std::memory_order_release);
    g_parkedInSaferegion.store(armed->InSaferegion() ? 1 : 0, std::memory_order_release);
    std::fprintf(stderr, "EXIT_DETACH_PARKED between=Create/Push in_saferegion=%d\n",
                 g_parkedInSaferegion.load());
    g_parked.store(true, std::memory_order_release);
    while (!g_release.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "EXIT_DETACH_RESUMED\n");
}

template<class Pred> bool WaitFor(Pred pred, int ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

enum class Arm { RaceInWindow, HookDisarmed, FlushAfterDetach };

void RunArm(Arm arm)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    heap.region0()->reset(PageAge::eden);
    heap.region1()->reset(PageAge::old);
    MarkPublicationFixture marking;
    const bool hookArmed = arm != Arm::HookDisarmed;
    MarkThreadLocalStacks::SetPushCreatedBreakpoint(hookArmed ? &ParkExitingPush : nullptr);
    std::atomic<bool> bufferReady{false};
    size_t pendingBefore = 0;
    size_t localYoungBefore = 0;
    std::thread exiting([&] {
        auto& manager = MutatorManager::Instance();
        Mutator* owner = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        StackWatermarkSet::on_safepoint(*owner);
        // One buffered store whose previous value is an unmarked young object.
        // The slot is in the young page, so remember() is a no-op.
        auto* buffer = owner->GetGCData().storeBarrierBuffer;
        buffer->add(heap.heapStart + 16, StoreGoodPointer(heap.obj0));
        pendingBefore = buffer->Pending();
        localYoungBefore = owner->GetGCData().markStacks[0].Population();
        if (hookArmed) { g_armedOwner.store(owner, std::memory_order_release); }
        bufferReady.store(true, std::memory_order_release);
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    GC_EXPECT_TRUE(WaitFor([&] { return bufferReady.load(std::memory_order_acquire); }, 10000));
    bool parked = false;
    if (hookArmed) {
        parked = WaitFor([] { return g_parked.load(std::memory_order_acquire); }, 10000);
        std::fprintf(stderr, "EXIT_DETACH_STATE parked=%d in_saferegion=%d pending_before=%zu local_young_before=%zu\n",
                     parked, g_parkedInSaferegion.load(), pendingBefore, localYoungBefore);
    }
    bool managementLockHeld = false;
    if (parked) {
        auto& manager = MutatorManager::Instance();
        const bool acquired = manager.TryAcquireMutatorManagementWLock();
        managementLockHeld = !acquired;
        if (acquired) { manager.MutatorManagementWUnlock(); }
    }
    if (arm == Arm::FlushAfterDetach) {
        // Swap order: let detach finish completely before the pause flush.
        g_release.store(true, std::memory_order_release);
    }
    if (arm != Arm::RaceInWindow) {
        exiting.join();
        std::fprintf(stderr, "EXIT_DETACH_JOINED before_pause=1\n");
    }
    std::atomic<bool> worldStopped{false};
    std::atomic<bool> pauseDone{false};
    std::thread pause([&] {
        // Product pauses run on the ZDriver ConcurrentGCThread
        // (concurrentGCThread.cpp:78); SetThreadType attaches native GC TLS
        // data through ThreadLocal::InitializeCleaner (ThreadLocal.h:111).
        ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
        ScopedStopTheWorld stw("EXIT_DETACH young mark-end", false);
        worldStopped.store(true, std::memory_order_release);
        std::fprintf(stderr, "EXIT_DETACH_FLUSH_BEGIN world_stopped=%d\n",
                     MutatorManager::Instance().WorldStopped());
        const bool ended = Heap::GetHeap().young().mark_end();
        std::fprintf(stderr, "EXIT_DETACH_FLUSH_END mark_end=%d\n", ended);
        pauseDone.store(true, std::memory_order_release);
    });
    const bool stoppedWhileParked = WaitFor([&] { return worldStopped.load(std::memory_order_acquire); }, 3000);
    if (arm == Arm::RaceInWindow) {
        std::fprintf(stderr, "EXIT_DETACH_PAUSE_WHILE_EXIT_PARKED world_stopped=%d\n", stoppedWhileParked);
        if (stoppedWhileParked) {
            (void)WaitFor([&] { return pauseDone.load(std::memory_order_acquire); }, 10000);
        }
        g_release.store(true, std::memory_order_release);
        exiting.join();
    }
    pause.join();
    MarkThreadLocalStacks::SetPushCreatedBreakpoint(nullptr);
    auto& young = Heap::GetHeap().young().Mark();
    young.MarkFollow();
    const bool marked = heap.region0()->is_object_marked(from_object(heap.obj0), false);
    std::fprintf(stderr, "EXIT_DETACH_TARGET executed=1 arm=%d hook=%d parked=%d marked_young=%d\n",
                 static_cast<int>(arm), hookArmed, parked, marked);
    // Check only after joining, so a failing assertion cannot strand a thread.
    if (hookArmed) {
        std::fprintf(stderr, "EXIT_DETACH_WINDOW_ASSERT parked=%d safe=%d lock_held=%d\n",
                     parked, g_parkedInSaferegion.load(), managementLockHeld);
        GC_EXPECT_TRUE(parked);
        GC_EXPECT_EQ(g_parkedInSaferegion.load(), 0);
        GC_EXPECT_TRUE(managementLockHeld);
    }
    if (arm == Arm::RaceInWindow) {
        std::fprintf(stderr, "EXIT_DETACH_EXCLUSION_ASSERT stopped_while_parked=%d\n", stoppedWhileParked);
        GC_EXPECT_FALSE(stoppedWhileParked);
    }
    GC_EXPECT_TRUE(worldStopped.load());
    GC_EXPECT_TRUE(pauseDone.load());
    GC_EXPECT_TRUE(marked);
}
} // namespace

// Target arm: the pause meets the exiting owner inside its Create()/Push()
// window. Before the fix the pause completes while the owner is parked
// (in_saferegion=1) and the world-stopped flush publishes the empty stack,
// hitting "never publish an empty mark stripe stack". After the fix the owner
// is parked outside the saferegion (in_saferegion=0), the pause cannot
// complete while it is parked, and after release detach publishes the entry.
GC_OTHER_VM_TEST(ExitDetachMarkEnd, PauseCannotOverlapExitDetachWindow)
{
    RunArm(Arm::RaceInWindow);
}

// Control 1: no breakpoint; the exit completes before the pause.
GC_OTHER_VM_TEST(ExitDetachMarkEnd, ControlHookDisarmed)
{
    RunArm(Arm::HookDisarmed);
}

// Control 2: breakpoint reached, released, detach completes, then the pause.
GC_OTHER_VM_TEST(ExitDetachMarkEnd, ControlFlushAfterDetach)
{
    RunArm(Arm::FlushAfterDetach);
}
// zMark.cpp:961-962: a mark-end pause flushes non-Java threads only.
// A still-attached saferegion owner retains its pending store until its own
// handshake/exit, even when mark_end is called by the real pause path.
GC_OTHER_VM_TEST(ExitDetachMarkEnd, PauseDoesNotFlushAttachedMutator)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    heap.region0()->reset(PageAge::eden);
    heap.region1()->reset(PageAge::old);
    MarkPublicationFixture marking;
    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
    StoreBarrierBuffer* buffer = nullptr;
    std::thread owner([&] {
        auto& manager = MutatorManager::Instance();
        Mutator* mutator = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        buffer = mutator->GetGCData().storeBarrierBuffer;
        buffer->add(heap.heapStart + 16, StoreGoodPointer(heap.obj0));
        ready.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    const bool reached = WaitFor([&] { return ready.load(std::memory_order_acquire); }, 10000);
    size_t before = 0;
    size_t after = 0;
    bool markedDuringPause = false;
    if (reached) {
        before = buffer->Pending();
        ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
        {
            ScopedStopTheWorld stw("EXIT_DETACH attached owner scope", false);
            (void)Heap::GetHeap().young().mark_end();
            after = buffer->Pending();
            markedDuringPause = heap.region0()->is_object_marked(from_object(heap.obj0), false);
        }
    }
    release.store(true, std::memory_order_release);
    owner.join();
    std::fprintf(stderr, "EXIT_DETACH_SCOPE_ASSERT reached=%d before=%zu after=%zu marked_in_pause=%d\n",
                 reached, before, after, markedDuringPause);
    GC_EXPECT_TRUE(reached);
    GC_EXPECT_EQ(before, 1U);
    GC_EXPECT_EQ(after, before);
    GC_EXPECT_FALSE(markedDuringPause);
    Heap::GetHeap().young().Mark().MarkFollow();
    GC_EXPECT_TRUE(heap.region0()->is_object_marked(from_object(heap.obj0), false));
}
#endif
