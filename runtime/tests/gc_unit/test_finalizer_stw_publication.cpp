// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Registered, active mutators that take the mutator management read lock
// versus a stop-the-world pause that owns the write side.
//
// Stage B hello-spin: VMThread waited in WaitUntilAllMutatorStopped on the
// finalizer worker (LWP 925706), which spun in MutatorManagementRLock()
// inside FinalizerProcessor::RunWorker while the pause owned the write side.
// MutatorManager::CreateMutator has the same caller state on first schedule:
// the processor binds the identity and PreparedToRun publishes it active
// before StartMainTask / MRT_CreateMutator reach the read lock.
//
// Pause path: ScopedStopTheWorld -> MutatorManager::StopTheWorld ->
//   AcquireMutatorManagementWLock -> DemandSuspensionForSync ->
//   WaitUntilAllMutatorStopped.
//
// HotSpot form: the safepoint holds Threads_lock for the whole pause
// (safepoint.cpp:346, released at safepoint.cpp:465), and an active Java
// thread that finds a safepoint-checking lock contended waits in
// ThreadBlockInVMPreprocess (mutex.cpp:87-113), so the safepoint never waits
// on a thread that is waiting for the safepoint's own lock. A service thread
// whose native creation failed never runs its body (jvm.cpp:2992-3004).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
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
constexpr int kWaitMs = 10000;

template<class Pred> bool WaitFor(Pred pred, int ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// The product read-lock acquisition the worker performs while the pause owns
// the write side.
enum class Action { Publish, Retract, CreateMutator };
// Active: the state the product caller is in (MRT_PreRunManagedCode or
// PreparedToRun left the saferegion). Saferegion: control arm, the same call
// from a safepoint-safe state.
enum class Caller { Active, Saferegion };

const char* Name(Action action)
{
    switch (action) {
        case Action::Publish: return "publish";
        case Action::Retract: return "retract";
        default: return "create_mutator";
    }
}
const char* Name(Caller caller) { return caller == Caller::Active ? "active" : "saferegion"; }

// A timed-out wait means the worker may still be inside the product call and
// the threads cannot be joined; report and leave the test process without
// reading anything the worker writes.
[[noreturn]] void FailUnjoinable(Action action, Caller caller, const char* stage)
{
    std::fprintf(stderr, "FINALIZER_PUBLICATION_RESULT action=%s caller=%s timed_out=%s\n",
                 Name(action), Name(caller), stage);
    std::fflush(stderr);
    std::_Exit(1);
}

void RunUnderPause(Action action, Caller caller)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    auto& manager = MutatorManager::Instance();
    FinalizerProcessor& processor = Heap::GetHeap().GetFinalizerProcessor();
    const bool idleBefore = processor.GetMutator() == nullptr;
    // The product's out-of-line PreparedToRun, as test_binding_poll resolves it.
    using Resume = void (*)(Mutator*, ThreadLocalData*);
    auto resumeFirst = reinterpret_cast<Resume>(dlsym(RTLD_DEFAULT,
        "_ZN12MapleRuntime7Mutator13PreparedToRunEPNS_15ThreadLocalDataE"));
    GC_EXPECT_TRUE(action != Action::CreateMutator || resumeFirst != nullptr);
    if (action == Action::CreateMutator && resumeFirst == nullptr) { return; }
    std::atomic<Mutator*> owner{nullptr};
    std::atomic<bool> actNow{false};
    std::atomic<bool> acted{false};
    std::atomic<bool> exitNow{false};
    Mutator* created = nullptr; // written by the worker before `acted` (release)
    std::thread worker([&] {
        Mutator* current = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        if (action == Action::Retract) {
            // Published before the pause, from the initial saferegion.
            processor.PublishWorkerMutator(*current);
        }
        if (action == Action::CreateMutator) {
            // processor.cpp: the carrier binds the identity, then PreparedToRun
            // leaves the saferegion before the entry calls CreateMutator.
            resumeFirst(current, ThreadLocal::GetThreadLocalData());
        } else {
            // Registered and active, as after NewFinalizerCJThread returns.
            (void)current->LeaveSaferegion();
            StackWatermarkSet::on_safepoint(*current);
        }
        owner.store(current, std::memory_order_release);
        while (!actNow.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        const bool safe = caller == Caller::Saferegion && current->EnterSaferegion(false);
        switch (action) {
            case Action::Publish: processor.PublishWorkerMutator(*current); break;
            case Action::Retract: processor.RetractWorkerMutator(*current); break;
            default: created = manager.CreateMutator(); break;
        }
        if (safe) { (void)current->LeaveSaferegion(); }
        acted.store(true, std::memory_order_release);
        while (!exitNow.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        if (action == Action::Publish) { processor.RetractWorkerMutator(*current); }
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    if (!WaitFor([&] { return owner.load(std::memory_order_acquire) != nullptr; }, kWaitMs)) {
        FailUnjoinable(action, caller, "worker_setup");
    }
    Mutator* target = owner.load(std::memory_order_acquire);

    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::thread requester([&] {
        ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
        ScopedStopTheWorld stw("FINALIZER_PUBLICATION pause", false);
        paused.store(true, std::memory_order_release);
        while (!resume.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    });
    // The suspension request is set only after StopTheWorld owns the
    // management write lock, so the call below always meets the pause in
    // WaitUntilAllMutatorStopped with the lock held.
    const bool requested = WaitFor([&] {
        return manager.SyncTriggered() && target->HasAnySuspensionRequest();
    }, kWaitMs);
    if (!requested) { FailUnjoinable(action, caller, "request"); }
    const bool stoppedBeforeAct = paused.load(std::memory_order_acquire);
    actNow.store(true, std::memory_order_release);
    const bool pauseCompleted = WaitFor([&] { return paused.load(std::memory_order_acquire); }, kWaitMs);
    std::fprintf(stderr,
                 "FINALIZER_PUBLICATION_PAUSE action=%s caller=%s requested=1 stopped_before_act=%d "
                 "pause_completed=%d\n",
                 Name(action), Name(caller), stoppedBeforeAct, pauseCompleted);
    // The pause can only finish once the waiting caller is safe.
    if (!pauseCompleted) { FailUnjoinable(action, caller, "pause"); }
    // While the world is stopped the caller is safe and has not acquired the
    // read lock: the pause owns the write side the call needs.
    const bool safeInPause = target->InSaferegion();
    const bool actedInPause = acted.load(std::memory_order_acquire);
    resume.store(true, std::memory_order_release);
    requester.join();
    const bool actedAfter = WaitFor([&] { return acted.load(std::memory_order_acquire); }, kWaitMs);
    if (!actedAfter) { FailUnjoinable(action, caller, "act"); }
    // `acted` (acquire) orders every worker write of the call before these reads.
    bool identity = false;
    switch (action) {
        case Action::Publish:
            identity = processor.GetMutator() == target && processor.GetTid() == target->GetTid();
            break;
        case Action::Retract: identity = processor.GetMutator() == nullptr; break;
        default: identity = created == target; break;
    }
    exitNow.store(true, std::memory_order_release);
    worker.join();
    const bool idleAfter = processor.GetMutator() == nullptr;
    std::fprintf(stderr,
                 "FINALIZER_PUBLICATION_RESULT action=%s caller=%s pause_completed=1 safe_in_pause=%d "
                 "acted_in_pause=%d acted_after=1 identity=%d idle_after=%d\n",
                 Name(action), Name(caller), safeInPause, actedInPause, identity, idleAfter);
    GC_EXPECT_TRUE(idleBefore);
    GC_EXPECT_FALSE(stoppedBeforeAct);
    GC_EXPECT_TRUE(safeInPause);
    GC_EXPECT_FALSE(actedInPause);
    GC_EXPECT_TRUE(identity);
    GC_EXPECT_TRUE(idleAfter);
    GC_EXPECT_FALSE(manager.SyncTriggered());
}

// RunBoundWorker on a thread NewFinalizerCJThread did not bind: either no
// carrier (creation failed) or a carrier without a bound mutator.
void RunUnbound(bool handler, bool withCarrier)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    FinalizerProcessor& processor = Heap::GetHeap().GetFinalizerProcessor();
    const bool idleBefore = processor.GetMutator() == nullptr;
    int carrier = 0;
    std::atomic<int> served{-1};
    std::atomic<bool> unbound{false};
    std::thread worker([&] {
        unbound.store(ThreadLocal::GetMutator() == nullptr, std::memory_order_relaxed);
        served.store(processor.RunBoundWorker(handler, withCarrier ? &carrier : nullptr) ? 1 : 0,
                     std::memory_order_release);
    });
    worker.join();
    const bool idleAfter = processor.GetMutator() == nullptr;
    std::fprintf(stderr, "FINALIZER_WORKER_BINDING_RESULT worker=%s carrier=%d unbound=%d served=%d idle_after=%d\n",
                 handler ? "reference_handler" : "finalizer", withCarrier, unbound.load(), served.load(), idleAfter);
    GC_EXPECT_TRUE(idleBefore);
    GC_EXPECT_TRUE(unbound.load());
    GC_EXPECT_EQ(served.load(std::memory_order_acquire), 0);
    GC_EXPECT_TRUE(idleAfter);
}
} // namespace

// The worker publishes from the state MRT_PreRunManagedCode leaves it in while
// a pause owns the management write lock; the pause must still complete.
GC_OTHER_VM_TEST(FinalizerStwPublication, ActivePublisherDoesNotBlockPause)
{
    RunUnderPause(Action::Publish, Caller::Active);
}

// Control arm: the same interleaving with a safepoint-safe publisher.
GC_OTHER_VM_TEST(FinalizerStwPublication, SafePublisherDoesNotBlockPause)
{
    RunUnderPause(Action::Publish, Caller::Saferegion);
}

// The worker retracts after its managed service returned, still active, while
// a pause owns the management write lock.
GC_OTHER_VM_TEST(FinalizerStwPublication, ActiveRetractionDoesNotBlockPause)
{
    RunUnderPause(Action::Retract, Caller::Active);
}

// First schedule: MutatorManager::CreateMutator from the state PreparedToRun
// leaves a newly scheduled logical thread in.
GC_OTHER_VM_TEST(CreateMutatorStw, FirstScheduleDoesNotBlockPause)
{
    RunUnderPause(Action::CreateMutator, Caller::Active);
}

// Control arm: CreateMutator from a safepoint-safe caller.
GC_OTHER_VM_TEST(CreateMutatorStw, SafeCallerDoesNotBlockPause)
{
    RunUnderPause(Action::CreateMutator, Caller::Saferegion);
}

// NewFinalizerCJThread failed: neither worker publishes or enters managed code.
GC_OTHER_VM_TEST(FinalizerWorkerBinding, FinalizerWithoutCarrierIsNotServed)
{
    RunUnbound(false, false);
}

GC_OTHER_VM_TEST(FinalizerWorkerBinding, ReferenceHandlerWithoutCarrierIsNotServed)
{
    RunUnbound(true, false);
}

// A carrier without a bound mutator gets the same treatment.
GC_OTHER_VM_TEST(FinalizerWorkerBinding, FinalizerWithoutBoundMutatorIsNotServed)
{
    RunUnbound(false, true);
}
