// PROBE-1137 (investigation only, not a delivery): exiting mutator detach
// versus the young mark-end pause flush.
//
// Exiting side (product): MutatorManager::DestroyRuntimeMutator ->
//   TransitMutatorToExit -> EnterSaferegion(false) -> MutatorLock ->
//   Mutator::ResetMutator -> ZBarrierSet::on_thread_detach -> Heap::mark_flush
//   -> ZMark::Flush(ThreadGCData&) -> StoreBarrierBuffer::Flush ->
//   ZBarrier::mark_and_remember -> ZMark::MarkObject ->
//   MarkThreadLocalStacks::Push (Create() ... breakpoint ... Push(entry)).
// Pause side (product): ScopedStopTheWorld + ZGenerationYoung::mark_end ->
//   ZMark::TryEndYoungMark -> ZMark::TryEnd -> HandshakeFlush (world-stopped
//   branch) -> VisitOwners -> FlushTargetGCData -> MarkThreadLocalStacks::Flush
//   -> MarkStripe::PublishStack (CHECK never publish an empty mark stripe stack).
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
    std::fprintf(stderr, "PROBE1137_PARKED between=Create/Push in_saferegion=%d\n",
                 g_parkedInSaferegion.load());
    g_parked.store(true, std::memory_order_release);
    while (!g_release.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "PROBE1137_RESUMED\n");
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
        std::fprintf(stderr, "PROBE1137_EXIT_STATE parked=%d in_saferegion=%d pending_before=%zu local_young_before=%zu\n",
                     parked, g_parkedInSaferegion.load(), pendingBefore, localYoungBefore);
    }
    if (arm == Arm::FlushAfterDetach) {
        // Swap order: let detach finish completely before the pause flush.
        g_release.store(true, std::memory_order_release);
    }
    if (arm != Arm::RaceInWindow) {
        exiting.join();
        std::fprintf(stderr, "PROBE1137_EXIT_JOINED before_pause=1\n");
    }
    std::atomic<bool> worldStopped{false};
    std::atomic<bool> pauseDone{false};
    std::thread pause([&] {
        // Product pauses run on the ZDriver ConcurrentGCThread
        // (concurrentGCThread.cpp:78); SetThreadType attaches native GC TLS
        // data through ThreadLocal::InitializeCleaner (ThreadLocal.h:111).
        ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
        ScopedStopTheWorld stw("PROBE1137 young mark-end", false);
        worldStopped.store(true, std::memory_order_release);
        std::fprintf(stderr, "PROBE1137_OUTER_FLUSH_BEGIN world_stopped=%d\n",
                     MutatorManager::Instance().WorldStopped());
        const bool ended = Heap::GetHeap().young().mark_end();
        std::fprintf(stderr, "PROBE1137_OUTER_FLUSH_END mark_end=%d\n", ended);
        pauseDone.store(true, std::memory_order_release);
    });
    const bool stoppedWhileParked = WaitFor([&] { return worldStopped.load(std::memory_order_acquire); }, 3000);
    if (arm == Arm::RaceInWindow) {
        std::fprintf(stderr, "PROBE1137_PAUSE_WHILE_EXIT_PARKED world_stopped=%d\n", stoppedWhileParked);
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
    std::fprintf(stderr, "PROBE1137_TARGET executed=1 arm=%d hook=%d parked=%d marked_young=%d\n",
                 static_cast<int>(arm), hookArmed, parked, marked);
    GC_EXPECT_TRUE(marked);
}
} // namespace

// Red arm: pause flush lands inside the exiting owner's Create()/Push window.
GC_OTHER_VM_TEST(Probe1137, ExitDetachWindowVersusMarkEndPause)
{
    RunArm(Arm::RaceInWindow);
}

// Control 1: no breakpoint; the exit completes before the pause.
GC_OTHER_VM_TEST(Probe1137, ControlHookDisarmed)
{
    RunArm(Arm::HookDisarmed);
}

// Control 2: breakpoint reached, released, detach completes, then the pause.
GC_OTHER_VM_TEST(Probe1137, ControlFlushAfterDetach)
{
    RunArm(Arm::FlushAfterDetach);
}
#endif
