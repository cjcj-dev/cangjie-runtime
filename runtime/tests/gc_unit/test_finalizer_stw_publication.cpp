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
//
// The scheduler cases drive the production first schedule itself: the
// processor binds the CJThread's identity in TLS and PreparedToRun leaves the
// saferegion (processor.cpp:795-798), CJThreadEntryInitMutator calls the
// SCHD_CREATE_MUTATOR hook (cjthread.cpp:882-884), and MRT_CreateMutator
// (CJThreadModel.cpp:115-117) reaches CreateMutator, which reads the identity
// through ConcurrencyModel::GetMutator -> CJThreadGetMutator on the
// CJ_PROCESSOR carrier (CJThreadModel.cpp:215-218).
#ifndef MRT_USE_CJTHREAD_RENAME
#define MRT_USE_CJTHREAD_RENAME 1
#endif
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include "Cangjie.h"
#include "CjScheduler.h"
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
// carrier (creation failed) or a carrier without a bound mutator. The guard's
// invariant is that such a worker never passes into publication, managed entry
// or teardown. Any of those on a missing identity stops the calling thread with
// a synchronous fault, so the probe below records a fault taken by the worker
// thread inside the RunBoundWorker call and parks that thread; the test then
// reports it through its own assertion instead of the process ending on the
// signal. Faults on other threads keep the previous disposition.
constexpr int kFaultSignals[] = {SIGSEGV, SIGBUS, SIGABRT};
constexpr size_t kFaultSignalCount = sizeof(kFaultSignals) / sizeof(kFaultSignals[0]);
struct sigaction g_previousFault[kFaultSignalCount];
std::atomic<int> g_unboundFaultSigno{0};
std::atomic<int> g_unboundFaultCode{0};
std::atomic<uintptr_t> g_unboundFaultAddr{0};
thread_local bool t_inUnboundCall = false;

void OnUnboundFault(int signo, siginfo_t* info, void* context)
{
    if (t_inUnboundCall) {
        g_unboundFaultAddr.store(reinterpret_cast<uintptr_t>(info->si_addr), std::memory_order_relaxed);
        g_unboundFaultCode.store(info->si_code, std::memory_order_relaxed);
        g_unboundFaultSigno.store(signo, std::memory_order_release);
        for (;;) { (void)pause(); }
    }
    for (size_t i = 0; i < kFaultSignalCount; ++i) {
        if (kFaultSignals[i] != signo) { continue; }
        const struct sigaction& previous = g_previousFault[i];
        if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
            previous.sa_sigaction(signo, info, context);
            return;
        }
        if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
            previous.sa_handler(signo);
            return;
        }
        (void)sigaction(signo, &previous, nullptr); // the fault recurs with the default action
        return;
    }
}

void InstallUnboundFaultProbe()
{
    struct sigaction action {};
    action.sa_sigaction = OnUnboundFault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < kFaultSignalCount; ++i) {
        (void)sigaction(kFaultSignals[i], &action, &g_previousFault[i]);
    }
}

void RemoveUnboundFaultProbe()
{
    for (size_t i = 0; i < kFaultSignalCount; ++i) {
        (void)sigaction(kFaultSignals[i], &g_previousFault[i], nullptr);
    }
}

void RunUnbound(bool handler, bool withCarrier)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    FinalizerProcessor& processor = Heap::GetHeap().GetFinalizerProcessor();
    const bool idleBefore = processor.GetMutator() == nullptr;
    const char* worker = handler ? "reference_handler" : "finalizer";
    int carrier = 0;
    std::atomic<int> served{-1};
    std::atomic<bool> unbound{false};
    InstallUnboundFaultProbe();
    std::thread thread([&] {
        unbound.store(ThreadLocal::GetMutator() == nullptr, std::memory_order_relaxed);
        t_inUnboundCall = true;
        const bool result = processor.RunBoundWorker(handler, withCarrier ? &carrier : nullptr);
        t_inUnboundCall = false;
        served.store(result ? 1 : 0, std::memory_order_release);
    });
    const bool returned = WaitFor([&] {
        return served.load(std::memory_order_acquire) != -1 ||
            g_unboundFaultSigno.load(std::memory_order_acquire) != 0;
    }, kWaitMs);
    // The release store of the signal number orders the worker's earlier writes.
    const int faultSigno = g_unboundFaultSigno.load(std::memory_order_acquire);
    if (faultSigno != 0) {
        // The worker passed the guard without an identity and stopped inside
        // the product call; it holds whatever it acquired and cannot be joined.
        const uintptr_t faultAddr = g_unboundFaultAddr.load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "FINALIZER_WORKER_BINDING_RESULT worker=%s carrier=%d unbound=%d served=-1 "
                     "boundary_entered=1 signo=%d si_code=%d null_identity_access=%d\n",
                     worker, withCarrier, unbound.load(std::memory_order_relaxed), faultSigno,
                     g_unboundFaultCode.load(std::memory_order_relaxed), faultAddr < 0x10000);
        thread.detach();
        try {
            GC_EXPECT_EQ(faultSigno, 0);
        } catch (const AssertFailure& failure) {
            std::fprintf(stderr, "FINALIZER_WORKER_BINDING_ASSERT %s\n", failure.what());
        }
        std::fflush(stderr);
        std::_Exit(1);
    }
    if (!returned) {
        std::fprintf(stderr, "FINALIZER_WORKER_BINDING_RESULT worker=%s carrier=%d timed_out=serve\n", worker,
                     withCarrier);
        std::fflush(stderr);
        std::_Exit(1);
    }
    thread.join();
    RemoveUnboundFaultProbe();
    const bool idleAfter = processor.GetMutator() == nullptr;
    std::fprintf(stderr,
                 "FINALIZER_WORKER_BINDING_RESULT worker=%s carrier=%d unbound=%d served=%d boundary_entered=0 "
                 "idle_after=%d\n",
                 worker, withCarrier, unbound.load(), served.load(), idleAfter);
    GC_EXPECT_TRUE(idleBefore);
    GC_EXPECT_TRUE(unbound.load());
    GC_EXPECT_EQ(faultSigno, 0);
    GC_EXPECT_EQ(served.load(std::memory_order_acquire), 0);
    GC_EXPECT_TRUE(idleAfter);
}

// The production first schedule of a C-created logical thread
// (SchdpollNotifyCallback, schdpoll.cpp:459-461). The gate occupies the
// SCHD_CREATE_MUTATOR slot in front of the product hook only to hold the
// scheduled identity at the entry until the pause owns the write side; it
// records what the entry sees and then calls the product MRT_CreateMutator.
struct FirstSchedule {
    bool safeWaiter = false;
    std::atomic<bool> armed{false}; // set just before the test creates its thread
    std::atomic<bool> claimed{false};
    std::atomic<bool> atEntry{false};
    Mutator* entry = nullptr;      // written before `atEntry` (release)
    Mutator* tlsAtEntry = nullptr; // written before `atEntry` (release)
    int typeAtEntry = -1;          // written before `atEntry` (release)
    bool activeAtEntry = false;    // written before `atEntry` (release)
    std::atomic<bool> actNow{false};
    std::atomic<bool> acted{false};
    Mutator* bound = nullptr; // written before `acted` (release)
    std::atomic<bool> taskRan{false};
};
std::atomic<FirstSchedule*> g_firstSchedule{nullptr};
uintptr_t (*g_productCreateMutator)() = nullptr;

const char* Name(const FirstSchedule& probe) { return probe.safeWaiter ? "saferegion" : "active"; }

uintptr_t GateFirstSchedule()
{
    FirstSchedule* probe = g_firstSchedule.load(std::memory_order_acquire);
    // Logical threads the runtime creates on its own pass straight through.
    if (probe == nullptr || !probe->armed.load(std::memory_order_acquire) ||
        probe->claimed.exchange(true, std::memory_order_acq_rel)) {
        return g_productCreateMutator();
    }
    Mutator* identity = static_cast<Mutator*>(CJThreadGetMutator());
    probe->entry = identity;
    probe->tlsAtEntry = ThreadLocal::GetMutator();
    probe->typeAtEntry = static_cast<int>(ThreadLocal::GetThreadType());
    probe->activeAtEntry = identity != nullptr && !identity->InSaferegion();
    // Control: the scheduled identity waits safepoint-safe instead.
    const bool safe = probe->safeWaiter && identity != nullptr && identity->EnterSaferegion(false);
    probe->atEntry.store(true, std::memory_order_release);
    while (!probe->actNow.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    if (safe) { (void)identity->LeaveSaferegion(); }
    const uintptr_t result = g_productCreateMutator();
    probe->bound = ThreadLocal::GetMutator();
    probe->acted.store(true, std::memory_order_release);
    return result;
}

void* FirstScheduleTask(void*, unsigned int)
{
    FirstSchedule* probe = g_firstSchedule.load(std::memory_order_acquire);
    if (probe != nullptr) { probe->taskRan.store(true, std::memory_order_release); }
    return nullptr;
}

// What the child observed; the test process asserts on it.
enum FirstScheduleStage : int { kStageNone, kStageEntry, kStageRequest, kStagePause, kStageAct, kStageTask, kStageDone };
const char* StageName(int stage)
{
    static const char* const names[] = {"none", "entry", "request", "pause", "act", "task", "done"};
    return stage >= kStageNone && stage <= kStageDone ? names[stage] : "unknown";
}
struct FirstScheduleReport {
    int registered = 0;
    int entered = 0;
    int processorCarrier = 0;
    int tlsBound = 0;
    int activeAtEntry = 0;
    int stage = kStageNone; // last stage reached; kStageDone when every wait returned
    int stoppedBeforeAct = 0;
    int pauseCompleted = 0;
    int safeInPause = 0;
    int actedInPause = 0;
    int identity = 0;
    int taskRan = 0;
    int syncAfter = 0;
};
FirstSchedule g_childProbe;
FirstScheduleReport g_childReport;
int g_childReportFd = -1;

[[noreturn]] void FinishChild(int status)
{
    const char* bytes = reinterpret_cast<const char*>(&g_childReport);
    size_t done = 0;
    while (done < sizeof(g_childReport)) {
        const ssize_t written = write(g_childReportFd, bytes + done, sizeof(g_childReport) - done);
        if (written <= 0) { break; }
        done += static_cast<size_t>(written);
    }
    std::fflush(stderr);
    _exit(status);
}

// A wait that does not return within the bound leaves the gate or the
// requester inside a product call; record the stage and leave the child.
[[noreturn]] void FailChild(int stage)
{
    g_childReport.stage = stage;
    std::fprintf(stderr, "FIRST_SCHEDULE_CHILD waiter=%s timed_out=%s\n", Name(g_childProbe), StageName(stage));
    FinishChild(1);
}

void FinishEntry(FirstSchedule& probe)
{
    if (!WaitFor([&] { return probe.acted.load(std::memory_order_acquire); }, kWaitMs)) { FailChild(kStageAct); }
    if (!WaitFor([&] { return probe.taskRan.load(std::memory_order_acquire); }, kWaitMs)) { FailChild(kStageTask); }
}

// The product main task (StartMainTask, CjScheduler.cpp:333-344). It waits
// safepoint-safe while it creates the C-created logical thread and holds the
// pause, so the only active identity the pause can wait for is the one the
// scheduler first-schedules on the other processor.
int32_t FirstScheduleMain()
{
    FirstSchedule& probe = g_childProbe;
    FirstScheduleReport& report = g_childReport;
    auto& manager = MutatorManager::Instance();
    Mutator* self = Mutator::GetMutator();
    if (self != nullptr) { (void)self->EnterSaferegion(false); }
    CJThreadAttr attr{};
    CJThreadAttrInit(&attr);
    CJThreadAttrCjFromCSet(&attr, true);
    probe.armed.store(true, std::memory_order_release);
    CJThreadHandle task = CJThreadNewToDefault(&attr, FirstScheduleTask, nullptr, 0);
    if (task == nullptr) { FailChild(kStageEntry); }
    if (!WaitFor([&] { return probe.atEntry.load(std::memory_order_acquire); }, kWaitMs)) { FailChild(kStageEntry); }
    Mutator* target = probe.entry;
    report.entered = target != nullptr;
    // CJThreadModel.cpp:217-218: a CJ_PROCESSOR carrier takes the identity
    // from the current CJThread; processor.cpp:797 bound that identity in TLS
    // and :798 left the saferegion before the entry.
    report.processorCarrier = probe.typeAtEntry == static_cast<int>(ThreadType::CJ_PROCESSOR);
    report.tlsBound = target != nullptr && probe.tlsAtEntry == target;
    report.activeAtEntry = probe.activeAtEntry;
    std::fprintf(stderr,
                 "FIRST_SCHEDULE_ENTRY waiter=%s cjthread_identity=%d processor_carrier=%d tls_bound=%d active=%d\n",
                 Name(probe), report.entered, report.processorCarrier, report.tlsBound, report.activeAtEntry);
    if (!report.entered || !report.processorCarrier || !report.tlsBound || !report.activeAtEntry) {
        // Not the state the production first schedule leaves: let the entry
        // finish without a pause and report what differed.
        probe.actNow.store(true, std::memory_order_release);
        FinishEntry(probe);
        report.stage = kStageEntry;
        FinishChild(0);
    }
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::thread requester([&] {
        ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
        ScopedStopTheWorld stw("FIRST_SCHEDULE pause", false);
        paused.store(true, std::memory_order_release);
        while (!resume.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    });
    const bool requested = WaitFor([&] {
        return manager.SyncTriggered() && target->HasAnySuspensionRequest();
    }, kWaitMs);
    if (!requested) { FailChild(kStageRequest); }
    report.stoppedBeforeAct = paused.load(std::memory_order_acquire);
    if (probe.safeWaiter) {
        // The identity is safe at the entry: the pause completes before it acts.
        if (!WaitFor([&] { return paused.load(std::memory_order_acquire); }, kWaitMs)) { FailChild(kStagePause); }
        report.safeInPause = target->InSaferegion();
        report.actedInPause = probe.acted.load(std::memory_order_acquire);
        probe.actNow.store(true, std::memory_order_release);
    } else {
        // The identity is active at the entry; the pause can complete only
        // once CreateMutator waits for the read lock safepoint-safe.
        probe.actNow.store(true, std::memory_order_release);
        if (!WaitFor([&] { return paused.load(std::memory_order_acquire); }, kWaitMs)) { FailChild(kStagePause); }
        report.safeInPause = target->InSaferegion();
        report.actedInPause = probe.acted.load(std::memory_order_acquire);
    }
    report.pauseCompleted = 1;
    std::fprintf(stderr, "FIRST_SCHEDULE_PAUSE waiter=%s requested=1 stopped_before_act=%d pause_completed=1\n",
                 Name(probe), report.stoppedBeforeAct);
    resume.store(true, std::memory_order_release);
    requester.join();
    FinishEntry(probe);
    // `acted` (acquire) orders the gate's write of the bound identity.
    report.identity = probe.bound == target;
    report.taskRan = 1;
    report.syncAfter = manager.SyncTriggered();
    report.stage = kStageDone;
    FinishChild(0);
}

void RunFirstSchedule(bool safeWaiter)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    std::fflush(nullptr);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        g_childReportFd = output[1];
        g_childProbe.safeWaiter = safeWaiter;
        g_firstSchedule.store(&g_childProbe, std::memory_order_release);
        setenv("cjHeapSize", "512MB", 1);
        setenv("cjProcessorNum", "2", 1);
        MRT_CjRuntimeInit();
        // The scheduler is still SCHEDULE_INIT here (cjthread.cpp:1392), the
        // state in which the hook slot may be registered.
        g_productCreateMutator = reinterpret_cast<uintptr_t (*)()>(dlsym(RTLD_DEFAULT, "MRT_CreateMutator"));
        g_childReport.registered = g_productCreateMutator != nullptr &&
            CJThreadSchdHookRegister(GateFirstSchedule, SCHD_CREATE_MUTATOR) == 0;
        if (!g_childReport.registered) { FinishChild(3); }
        MRT_CjRuntimeStart(reinterpret_cast<void*>(&FirstScheduleMain));
        _exit(4); // the product main leaves with its own status
    }
    close(output[1]);
    FirstScheduleReport report;
    char* bytes = reinterpret_cast<char*>(&report);
    size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5 * kWaitMs);
    while (received < sizeof(report) && std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{output[0], POLLIN, 0};
        const int ready = poll(&descriptor, 1, 100);
        if (ready <= 0) { continue; }
        const ssize_t count = read(output[0], bytes + received, sizeof(report) - received);
        if (count <= 0) { break; }
        received += static_cast<size_t>(count);
    }
    close(output[0]);
    int status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(child, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (waited == 0) {
        (void)kill(child, SIGKILL);
        waited = waitpid(child, &status, 0);
    }
    const bool complete = received == sizeof(report);
    std::fprintf(stderr,
                 "FIRST_SCHEDULE_RESULT waiter=%s report=%d registered=%d entered=%d processor_carrier=%d "
                 "tls_bound=%d active=%d stage=%s stopped_before_act=%d pause_completed=%d safe_in_pause=%d "
                 "acted_in_pause=%d identity=%d task_ran=%d sync_after=%d status=%d\n",
                 safeWaiter ? "saferegion" : "active", complete, report.registered, report.entered,
                 report.processorCarrier, report.tlsBound, report.activeAtEntry, StageName(report.stage),
                 report.stoppedBeforeAct, report.pauseCompleted, report.safeInPause, report.actedInPause,
                 report.identity, report.taskRan, report.syncAfter, status);
    GC_EXPECT_TRUE(complete);
    GC_EXPECT_TRUE(report.registered);
    GC_EXPECT_TRUE(report.entered);
    GC_EXPECT_TRUE(report.processorCarrier);
    GC_EXPECT_TRUE(report.tlsBound);
    GC_EXPECT_TRUE(report.activeAtEntry);
    if (!safeWaiter) { GC_EXPECT_FALSE(report.stoppedBeforeAct); }
    GC_EXPECT_TRUE(report.pauseCompleted);
    GC_EXPECT_TRUE(report.safeInPause);
    GC_EXPECT_FALSE(report.actedInPause);
    GC_EXPECT_TRUE(report.identity);
    GC_EXPECT_TRUE(report.taskRan);
    GC_EXPECT_FALSE(report.syncAfter);
    GC_EXPECT_EQ(report.stage, static_cast<int>(kStageDone));
    GC_EXPECT_TRUE(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
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

// The scheduler's own first schedule: the identity CreateMutator reads from
// the CJThread is the one the processor bound and published active, and a
// pause that owns the write side before the entry acts still completes.
GC_RUNTIME_OTHER_VM_TEST(CreateMutatorStw, SchedulerFirstScheduleDoesNotBlockPause)
{
    RunFirstSchedule(false);
}

// Control arm: the same scheduled identity waits safepoint-safe at the entry.
GC_RUNTIME_OTHER_VM_TEST(CreateMutatorStw, SchedulerFirstScheduleSafeWaiterControl)
{
    RunFirstSchedule(true);
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
