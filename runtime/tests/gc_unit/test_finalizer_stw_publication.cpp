// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Finalizer worker identity publication versus a stop-the-world pause.
//
// Stage B hello-spin: VMThread waited in WaitUntilAllMutatorStopped on the
// finalizer worker (LWP 925706), which spun in MutatorManagementRLock()
// inside FinalizerProcessor::RunWorker while the pause owned the write side.
//
// Product path: FinalizerProcessor::RunWorker -> NewFinalizerCJThread ->
//   MRT_PreRunManagedCode (leaves the saferegion) -> PublishWorkerMutator.
// Pause path: ScopedStopTheWorld -> MutatorManager::StopTheWorld ->
//   AcquireMutatorManagementWLock -> DemandSuspensionForSync ->
//   WaitUntilAllMutatorStopped.
//
// HotSpot form: the safepoint holds Threads_lock for the whole pause
// (safepoint.cpp:346, released at safepoint.cpp:465), and an active Java
// thread that finds a safepoint-checking lock contended waits in
// ThreadBlockInVMPreprocess (mutex.cpp:87-113), so the safepoint never waits
// on a thread that is waiting for the safepoint's own lock.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zReferenceProcessor.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "b09_runtime_fixture.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
template<class Pred> bool WaitFor(Pred pred, int ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Active: the worker is in the state MRT_PreRunManagedCode leaves it in.
// Saferegion: control arm, the same publication from a safepoint-safe state.
enum class Publisher { Active, Saferegion };

const char* Name(Publisher publisher) { return publisher == Publisher::Active ? "active" : "saferegion"; }

void RunPublication(Publisher publisher)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    auto& manager = MutatorManager::Instance();
    FinalizerProcessor& processor = Heap::GetHeap().GetFinalizerProcessor();
    const bool idleBefore = processor.GetMutator() == nullptr;
    std::atomic<Mutator*> owner{nullptr};
    std::atomic<bool> publishNow{false};
    std::atomic<bool> published{false};
    std::atomic<bool> exitNow{false};
    std::thread worker([&] {
        Mutator* current = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        // Registered and active, as after NewFinalizerCJThread returns.
        (void)current->LeaveSaferegion();
        StackWatermarkSet::on_safepoint(*current);
        owner.store(current, std::memory_order_release);
        while (!publishNow.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        if (publisher == Publisher::Active) {
            processor.PublishWorkerMutator(*current);
        } else {
            (void)current->EnterSaferegion(false);
            processor.PublishWorkerMutator(*current);
            (void)current->LeaveSaferegion();
        }
        published.store(true, std::memory_order_release);
        while (!exitNow.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        processor.RetractWorkerMutator(*current);
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    const bool created = WaitFor([&] { return owner.load() != nullptr; }, 10000);
    if (!created) { std::abort(); }
    Mutator* target = owner.load();

    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::thread requester([&] {
        ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
        ScopedStopTheWorld stw("FINALIZER_PUBLICATION pause", false);
        paused.store(true, std::memory_order_release);
        while (!resume.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    });
    // The suspension request is set only after StopTheWorld owns the
    // management write lock, so the publication below always meets the
    // pause in WaitUntilAllMutatorStopped with the lock held.
    const bool requested = WaitFor([&] {
        return manager.SyncTriggered() && target->HasAnySuspensionRequest();
    }, 10000);
    if (!requested) { std::abort(); }
    const bool stoppedBeforePublish = paused.load(std::memory_order_acquire);
    publishNow.store(true, std::memory_order_release);
    const bool pauseCompleted = WaitFor([&] { return paused.load(std::memory_order_acquire); }, 10000);
    std::fprintf(stderr,
                 "FINALIZER_PUBLICATION_PAUSE publisher=%s requested=%d stopped_before_publish=%d "
                 "pause_completed=%d\n",
                 Name(publisher), requested, stoppedBeforePublish, pauseCompleted);
    if (!pauseCompleted) {
        // The pause can only finish once the waiting publisher is safe; the
        // threads cannot be joined, so leave this test process now.
        std::fprintf(stderr, "FINALIZER_PUBLICATION_RESULT publisher=%s pause_completed=0\n", Name(publisher));
        std::fflush(stderr);
        std::_Exit(1);
    }
    // While the world is stopped the publisher is safe and has not published:
    // the pause owns the lock the publication needs.
    const bool safeInPause = target->InSaferegion();
    const bool publishedInPause = published.load(std::memory_order_acquire);
    resume.store(true, std::memory_order_release);
    requester.join();
    const bool publishedAfter = WaitFor([&] { return published.load(std::memory_order_acquire); }, 10000);
    const bool identity = processor.GetMutator() == target && processor.GetTid() == target->GetTid();
    exitNow.store(true, std::memory_order_release);
    worker.join();
    const bool retracted = processor.GetMutator() == nullptr;
    std::fprintf(stderr,
                 "FINALIZER_PUBLICATION_RESULT publisher=%s pause_completed=1 safe_in_pause=%d "
                 "published_in_pause=%d published_after=%d identity=%d retracted=%d\n",
                 Name(publisher), safeInPause, publishedInPause, publishedAfter, identity, retracted);
    GC_EXPECT_TRUE(idleBefore);
    GC_EXPECT_FALSE(stoppedBeforePublish);
    GC_EXPECT_TRUE(safeInPause);
    GC_EXPECT_FALSE(publishedInPause);
    GC_EXPECT_TRUE(publishedAfter);
    GC_EXPECT_TRUE(identity);
    GC_EXPECT_TRUE(retracted);
    GC_EXPECT_FALSE(manager.SyncTriggered());
}
} // namespace

// The worker publishes from the state MRT_PreRunManagedCode leaves it in while
// a pause owns the management write lock; the pause must still complete.
GC_OTHER_VM_TEST(FinalizerStwPublication, ActivePublisherDoesNotBlockPause)
{
    RunPublication(Publisher::Active);
}

// Control arm: the same interleaving with a safepoint-safe publisher.
GC_OTHER_VM_TEST(FinalizerStwPublication, SafePublisherDoesNotBlockPause)
{
    RunPublication(Publisher::Saferegion);
}
