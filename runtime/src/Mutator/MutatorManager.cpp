// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MutatorManager.h"

#include <thread>
#include <cstdlib>
#include <cstring>
#include "Base/TimeUtils.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Collector/TracingCollector.h"
#include "Heap/Heap.h"
#include "Mutator.inline.h"
#include "UnwindStack/StackExposureHook.h"
#include "schedule.h"
#include "CpuProfiler/CpuProfiler.h"

namespace MapleRuntime {
namespace {
thread_local bool inEpochHandshake = false;

uint64_t GetEpochHandshakeTimeoutMillis()
{
    static const uint64_t timeout = []() -> uint64_t {
        const char* value = std::getenv("MRT_GCV2_EPOCH_HANDSHAKE_TIMEOUT_MS");
        if (value != nullptr) {
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(value, &end, 10);
            if (end != value && parsed > 0) {
                return static_cast<uint64_t>(parsed);
            }
        }
        return 30000;
    }();
    return timeout;
}
} // namespace
// Mutator-list write-lock watchdog timeout (seconds). Read once from env
// cjMutatorLockTimeout, falling back to WAIT_LOCK_TIMEOUT, so heavy CPU-oversubscribed
// builds can raise it without a rebuild. A reader holding the list lock can be starved
// off-CPU far longer than the old fixed 30s under contention; failing fast there is a
// false positive, not a real deadlock.
static uint64_t GetWaitLockTimeoutSec()
{
    static const uint64_t timeout = []() -> uint64_t {
        const char* env = std::getenv("cjMutatorLockTimeout");
        if (env != nullptr) {
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(env, &end, 10);
            if (end != env && parsed > 0) {
                return static_cast<uint64_t>(parsed);
            }
        }
        return WAIT_LOCK_TIMEOUT;
    }();
    return timeout;
}

extern "C" uintptr_t MRT_GetSafepointProtectedPage()
{
    return static_cast<uintptr_t>(true);
}

bool IsRuntimeThread()
{
    if (static_cast<int>(ThreadLocal::GetThreadType()) >= static_cast<int>(ThreadType::GC_THREAD)) {
        return true;
    }
    return false;
}

bool IsGcThread()
{
    if (static_cast<int>(ThreadLocal::GetThreadType()) == static_cast<int>(ThreadType::GC_THREAD)) {
        return true;
    }
    return false;
}

extern "C" void HandleSafepoint(ThreadLocalData* tlData)
{
    Mutator* mutator = tlData->mutator;
    // Current mutator enter saferegion
    mutator->DoEnterSaferegion();
    // Current mutator block before leaving saferegion
    mutator->DoLeaveSaferegion();
    DLOG(SIGNAL, "HandleSafepoint, thread restarted.");
}

#if defined (__arm__)
extern "C" void HandleSafepointForArm(ThreadLocalData* tlData)
{
    if (tlData->safepointState == 0) {
        return;
    }
    Mutator* mutator = tlData->mutator;
    // Current mutator enter saferegion
    mutator->DoEnterSaferegion();
    // Current mutator block before leaving saferegion
    mutator->DoLeaveSaferegion();
    DLOG(SIGNAL, "HandleSafepoint, thread restarted.");
}
#endif

void MutatorManager::BindMutator(Mutator& mutator) const
{
    ThreadLocalData* tlData = ThreadLocal::GetThreadLocalData();
    if (UNLIKELY(tlData->buffer == nullptr)) {
        (void)AllocBuffer::GetOrCreateAllocBuffer();
    }
    mutator.SetSafepointStatePtr(&tlData->safepointState);
    mutator.SetSafepointActive(false);
    tlData->SetMutator(&mutator);
}

void MutatorManager::UnbindMutator(Mutator& mutator) const
{
    ThreadLocalData* tlData = ThreadLocal::GetThreadLocalData();
    MRT_ASSERT(tlData->mutator == &mutator, "mutator in ThreadLocalData doesn't match in cjthread");
    tlData->SetMutator(nullptr);
    mutator.SetSafepointStatePtr(nullptr);
}

Mutator* MutatorManager::CreateMutator()
{
    RecordEpochHandshakeCreateAttempt();
    Mutator* mutator = ConcurrencyModel::GetMutator();
    if (mutator == nullptr) {
        mutator = new (std::nothrow) Mutator();
        CHECK_DETAIL(mutator != nullptr, "new Mutator failed");
        MutatorManagementRLock();
        mutator->Init();
        mutator->InitTid();
        BindMutator(*mutator);
        mutator->SetMutatorPhase(Heap::GetHeap().GetGCPhase());
        // dynjoin (乙): under active epoch, born-clean exclude (not wait-set join).
        ExcludeNewMutatorFromActiveEpoch(*mutator);
        ConcurrencyModel::SetMutator(mutator);
    } else {
        MutatorManagementRLock();
        mutator->Init();
        mutator->InitTid();
        BindMutator(*mutator);
        mutator->SetMutatorPhase(Heap::GetHeap().GetGCPhase());
        ExcludeNewMutatorFromActiveEpoch(*mutator);
    }
    MutatorManagementRUnlock();
    return mutator;
}

void MutatorManager::TransitMutatorToExit()
{
    Mutator* mutator = Mutator::GetMutator();
    CHECK_DETAIL(mutator != nullptr, "Mutator has not initialized or has been fini: %p", mutator);
    RecordEpochHandshakeExitTransition();
    mutator->SetEpochHandshakeLifecycle(Mutator::EPOCH_HANDSHAKE_EXITING);
    // Enter saferegion to avoid blocking gc stw
    mutator->MutatorLock();
    mutator->ResetMutator();
    mutator->MutatorUnlock();
    (void)mutator->EnterSaferegion(false);
    UnbindMutator(*mutator);
}

void MutatorManager::DestroyExpiredMutators()
{
    expiringMutatorListLock.lock();
    ExpiredMutatorList workList;
    workList.swap(expiringMutators);
    expiringMutatorListLock.unlock();
    for (auto it = workList.begin(); it != workList.end(); ++it) {
        Mutator* expiringMutator = *it;
        delete expiringMutator;
    }
}

void MutatorManager::DestroyMutator(Mutator* mutator)
{
    // dynjoin: while an epoch handshake is active, never free a participant (or a
    // racing create) under the old R-lock path — that used to be serialised by the
    // full-handshake W-lock. Defer to expiringMutators; PostGC drains them.
    if (EpochHandshakeActive()) {
        epochHandshakeDestroyDeferred.fetch_add(1, std::memory_order_relaxed);
        expiringMutatorListLock.lock();
        expiringMutators.push_back(mutator);
        expiringMutatorListLock.unlock();
        return;
    }
    if (TryAcquireMutatorManagementRLock()) {
        delete mutator; // call ~Mutator() under mutatorListLock
        MutatorManagementRUnlock();
    } else {
        expiringMutatorListLock.lock();
        expiringMutators.push_back(mutator);
        expiringMutatorListLock.unlock();
    }
}

Mutator* MutatorManager::CreateRuntimeMutator(ThreadType threadType)
{
    // Because TSAN tool can't identify the RwLock implemented by ourselves,
    // we use a global instance fpMutatorInstance instead of an instance created on
    // heap in order to prevent false positives.
    static Mutator fpMutatorInstance;
    Mutator* mutator = nullptr;
    if (threadType == ThreadType::FP_THREAD) {
        mutator = &fpMutatorInstance;
    } else {
        mutator = new (std::nothrow) Mutator();
    }
    CHECK_DETAIL(mutator != nullptr, "create mutator out of native memory");
    MutatorManagementRLock();
#ifdef INTERPRETER_ENABLED
    mutator->markAsRuntimeMutator();
#endif
    mutator->Init();
    mutator->InitTid();
    mutator->InitProtectStackAddr();
    mutator->SetManagedContext(false);
    MutatorManager::Instance().BindMutator(*mutator);
    mutator->SetMutatorPhase(Heap::GetHeap().GetGCPhase());
    ThreadLocal::SetMutator(mutator);
    ThreadLocal::SetThreadType(threadType);
    ThreadLocal::SetCJProcessorFlag(true);
    MutatorManagementRUnlock();
    ThreadLocalData* threadData = reinterpret_cast<ThreadLocalData*>(MRT_GetThreadLocalData());
    // Managed-entry setup may block on sync/STW, so do not hold the mutator
    // management lock across it.
    MRT_PreRunManagedCode(mutator, 2, threadData); // 2 layers
    // only running mutator can enter saferegion.
    return mutator;
}

void MutatorManager::DestroyRuntimeMutator(ThreadType threadType)
{
    Mutator* mutator = ThreadLocal::GetMutator();
    CHECK_DETAIL(mutator != nullptr, "Fini UpdateThreads with null mutator");

    MutatorManagementRLock();
    (void)mutator->LeaveSaferegion();
    // fp mutator is a static instance, we can't delete it, we reset the mutator to avoid invalid memory
    // access when static instance destruction.
    if (threadType != ThreadType::FP_THREAD) {
        delete mutator;
    } else {
        mutator->ResetMutator();
    }
    ThreadLocal::SetAllocBuffer(nullptr);
    ThreadLocal::SetMutator(nullptr);
    ThreadLocal::SetCJProcessorFlag(false);
    MutatorManagementRUnlock();
}

void MutatorManager::Init()
{
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    safepointPageManager = new (std::nothrow) SafepointPageManager();
    CHECK_DETAIL(safepointPageManager != nullptr, "new safepointPageManager failed");
    safepointPageManager->Init();
#endif
}

MutatorManager& MutatorManager::Instance() noexcept { return Runtime::Current().GetMutatorManager(); }

bool MutatorManager::EpochHandshakeEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("MRT_GCV2_EPOCH_HANDSHAKE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled || ConcurrentStackScanEnabled();
}

bool MutatorManager::ConcurrentStackScanEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("MRT_GCV2_CONCURRENT_STACK_SCAN");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

void MutatorManager::RecordEpochHandshakeAck(Mutator& mutator, uint64_t epoch, bool bySelf)
{
    std::lock_guard<std::mutex> lock(epochHandshakeLedgerMutex);
    if (epoch != epochHandshakeActive.load(std::memory_order_acquire) ||
        !epochHandshakeAckedMutators.insert(&mutator).second) {
        epochHandshakeAckedTwice.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    epochHandshakeAcked.fetch_add(1, std::memory_order_relaxed);
    if (bySelf) {
        epochHandshakeSelfAck.fetch_add(1, std::memory_order_relaxed);
    } else {
        epochHandshakeGcAssistedAck.fetch_add(1, std::memory_order_relaxed);
    }
    switch (mutator.GetEpochHandshakeLifecycle()) {
        case Mutator::EPOCH_HANDSHAKE_STARTING:
            epochHandshakeStartingAck.fetch_add(1, std::memory_order_relaxed);
            break;
        case Mutator::EPOCH_HANDSHAKE_RUNNING:
            epochHandshakeRunningAck.fetch_add(1, std::memory_order_relaxed);
            break;
        case Mutator::EPOCH_HANDSHAKE_PARKED:
            epochHandshakeParkedAck.fetch_add(1, std::memory_order_relaxed);
            break;
        case Mutator::EPOCH_HANDSHAKE_EXITING:
            epochHandshakeExitingAck.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            CHECK_DETAIL(false, "unknown epoch handshake lifecycle state");
    }
}

void MutatorManager::RecordEpochHandshakeStackScan(bool scanned, size_t frames)
{
    if (scanned) {
        epochHandshakeStackScanned.fetch_add(1, std::memory_order_relaxed);
        epochHandshakeStackFrames.fetch_add(frames, std::memory_order_relaxed);
    } else {
        epochHandshakeStackFallback.fetch_add(1, std::memory_order_relaxed);
    }
}

void MutatorManager::RecordEpochHandshakeCreateAttempt()
{
    if (epochHandshakeActive.load(std::memory_order_acquire) != 0) {
        epochHandshakeDeferredCreates.fetch_add(1, std::memory_order_relaxed);
    }
}

void MutatorManager::ExcludeNewMutatorFromActiveEpoch(Mutator& mutator)
{
    uint64_t active = epochHandshakeActive.load(std::memory_order_acquire);
    if (active == 0) {
        return;
    }
    // (乙) exclude + born-clean. Must serialise with the snapshot that fills
    // epochHandshakeParticipants: if this mutator was already claimed as a
    // participant, it must take the normal request/ack path (not overwrite).
    std::lock_guard<std::mutex> lock(epochHandshakeLedgerMutex);
    active = epochHandshakeActive.load(std::memory_order_acquire);
    if (active == 0) {
        return;
    }
    if (epochHandshakeParticipants.find(&mutator) != epochHandshakeParticipants.end()) {
        return;
    }
    if (mutator.FinishedEpochHandshake(active)) {
        return;
    }
    mutator.MarkBornCleanForEpoch(active);
    epochHandshakeBornCleanJoins.fetch_add(1, std::memory_order_relaxed);
}

void MutatorManager::RecordEpochHandshakeExitTransition()
{
    if (epochHandshakeActive.load(std::memory_order_acquire) != 0) {
        epochHandshakeExitTransitions.fetch_add(1, std::memory_order_relaxed);
    }
}

EpochHandshakeStats MutatorManager::RunEpochHandshake(const char* source)
{
    EpochHandshakeStats stats;
    if (!EpochHandshakeEnabled()) {
        return stats;
    }

    Mutator* caller = IsRuntimeThread() ? nullptr : Mutator::GetMutator();
    bool callerEnteredSaferegion = caller != nullptr && caller->EnterSaferegion(true);
    CHECK_DETAIL(!inEpochHandshake, "nested epoch handshake is not supported");
    inEpochHandshake = true;
    // Hold syncMutex so STW cannot interleave (StopTheWorld also takes it). Do NOT
    // hold mutator-management W-lock across the wait: that serialised thread create
    // (FIXED_ROSTER_IS_STEP0_ONLY). dynjoin replaces it with (乙) born-clean exclude
    // + participant-set pin for DestroyMutator.
    syncMutex.lock();
    CHECK_DETAIL(!WorldStopped(), "epoch handshake must not run while worldStopped=true");

    stats.epoch = epochHandshakeSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    CHECK_DETAIL(stats.epoch != 0, "epoch handshake sequence overflow");
    epochHandshakeAcked.store(0, std::memory_order_relaxed);
    epochHandshakeAckedTwice.store(0, std::memory_order_relaxed);
    epochHandshakeSelfAck.store(0, std::memory_order_relaxed);
    epochHandshakeGcAssistedAck.store(0, std::memory_order_relaxed);
    epochHandshakeStartingAck.store(0, std::memory_order_relaxed);
    epochHandshakeRunningAck.store(0, std::memory_order_relaxed);
    epochHandshakeParkedAck.store(0, std::memory_order_relaxed);
    epochHandshakeExitingAck.store(0, std::memory_order_relaxed);
    epochHandshakeDeferredCreates.store(0, std::memory_order_relaxed);
    epochHandshakeBornCleanJoins.store(0, std::memory_order_relaxed);
    epochHandshakeExitTransitions.store(0, std::memory_order_relaxed);
    epochHandshakeDestroyDeferred.store(0, std::memory_order_relaxed);
    epochHandshakeStopTheWorldCalls.store(0, std::memory_order_relaxed);
    epochHandshakeStackScanned.store(0, std::memory_order_relaxed);
    epochHandshakeStackFallback.store(0, std::memory_order_relaxed);
    epochHandshakeStackFrames.store(0, std::memory_order_relaxed);

    uint64_t residualLockStart = TimeUtil::NanoSeconds();
    {
        std::lock_guard<std::mutex> lock(epochHandshakeLedgerMutex);
        epochHandshakeAckedMutators.clear();
        epochHandshakeParticipants.clear();
    }
    // Publish active BEFORE snapshot so concurrent CreateMutator sees active and
    // takes the born-clean path. Snapshot then only captures pre-existing mutators;
    // anyone who raced past is either in the list or born-clean (not both in wait).
    epochHandshakeActive.store(stats.epoch, std::memory_order_release);

    std::list<Mutator*> snapshotted;
    VisitAllMutators([&snapshotted](Mutator& mutator) { snapshotted.push_back(&mutator); });
    std::list<Mutator*> pending;
    {
        // Claim participants under the same lock ExcludeNewMutatorFromActiveEpoch uses.
        std::lock_guard<std::mutex> lock(epochHandshakeLedgerMutex);
        for (Mutator* mutator : snapshotted) {
            if (mutator->FinishedEpochHandshake(stats.epoch)) {
                continue; // born-clean race: create already excluded this mutator
            }
            if (epochHandshakeParticipants.insert(mutator).second) {
                pending.push_back(mutator);
            }
        }
    }
    stats.requested = pending.size();
    stats.managementLockNanos = TimeUtil::NanoSeconds() - residualLockStart;
    for (Mutator* mutator : pending) {
        // Defensive: born-clean may still race in after claim if Exclude lost the
        // participants check; Request must not fire on an already-finished epoch.
        if (mutator->FinishedEpochHandshake(stats.epoch)) {
            continue;
        }
        mutator->RequestEpochHandshake(stats.epoch);
    }

    uint64_t waitStart = TimeUtil::MilliSeconds();
    // K-bound: timeout is the exit condition (ForwardBarrier.cpp:23-24 discipline).
    // Wait set is fixed at snapshot; born-clean joiners never enlarge it.
    while (!pending.empty()) {
        for (auto it = pending.begin(); it != pending.end();) {
            Mutator* mutator = *it;
            if (mutator->FinishedEpochHandshake(stats.epoch)) {
                it = pending.erase(it);
                continue;
            }
            if (mutator->CanGcAssistEpochHandshake()) {
                (void)mutator->AcknowledgeEpochHandshake(stats.epoch, false);
            }
            ++it;
        }
        if (UNLIKELY(TimeUtil::MilliSeconds() - waitStart > GetEpochHandshakeTimeoutMillis())) {
            LOG(RTLOG_ERROR,
                "[GCV2][epoch-handshake] source=%s epoch=%llu requested=%zu acked=%zu acked_twice=%zu "
                "missing=%zu timeout_ms=%llu",
                source, static_cast<unsigned long long>(stats.epoch), stats.requested,
                epochHandshakeAcked.load(std::memory_order_relaxed),
                epochHandshakeAckedTwice.load(std::memory_order_relaxed), pending.size(),
                static_cast<unsigned long long>(GetEpochHandshakeTimeoutMillis()));
            CHECK_DETAIL(false, "epoch handshake timed out");
        }
        if (!pending.empty()) {
            (void)sched_yield();
        }
    }

    stats.acked = epochHandshakeAcked.load(std::memory_order_relaxed);
    stats.ackedTwice = epochHandshakeAckedTwice.load(std::memory_order_relaxed);
    stats.selfAck = epochHandshakeSelfAck.load(std::memory_order_relaxed);
    stats.gcAssistedAck = epochHandshakeGcAssistedAck.load(std::memory_order_relaxed);
    stats.startingAck = epochHandshakeStartingAck.load(std::memory_order_relaxed);
    stats.runningAck = epochHandshakeRunningAck.load(std::memory_order_relaxed);
    stats.parkedAck = epochHandshakeParkedAck.load(std::memory_order_relaxed);
    stats.exitingAck = epochHandshakeExitingAck.load(std::memory_order_relaxed);
    stats.deferredCreates = epochHandshakeDeferredCreates.load(std::memory_order_relaxed);
    stats.bornCleanJoins = epochHandshakeBornCleanJoins.load(std::memory_order_relaxed);
    stats.exitTransitions = epochHandshakeExitTransitions.load(std::memory_order_relaxed);
    stats.destroyDeferred = epochHandshakeDestroyDeferred.load(std::memory_order_relaxed);
    stats.stopTheWorldCalls = epochHandshakeStopTheWorldCalls.load(std::memory_order_relaxed);
    stats.stackScanned = epochHandshakeStackScanned.load(std::memory_order_relaxed);
    stats.stackFallback = epochHandshakeStackFallback.load(std::memory_order_relaxed);
    stats.stackFrames = epochHandshakeStackFrames.load(std::memory_order_relaxed);
    CHECK_DETAIL(stats.acked == stats.requested && stats.ackedTwice == 0 && stats.stopTheWorldCalls == 0,
                 "epoch handshake accounting failed: requested=%zu acked=%zu acked_twice=%zu stw_calls=%zu",
                 stats.requested, stats.acked, stats.ackedTwice, stats.stopTheWorldCalls);
    CHECK_DETAIL(!WorldStopped(), "epoch handshake changed worldStopped");

    {
        std::lock_guard<std::mutex> lock(epochHandshakeLedgerMutex);
        epochHandshakeParticipants.clear();
    }
    epochHandshakeActive.store(0, std::memory_order_release);
    syncMutex.unlock();
    inEpochHandshake = false;
    if (callerEnteredSaferegion) {
        (void)caller->LeaveSaferegion();
    }

    LOG(RTLOG_ERROR,
         "[GCV2][epoch-handshake] source=%s epoch=%llu requested=%zu acked=%zu acked_twice=%zu "
         "self=%zu gc_assisted=%zu starting=%zu running=%zu parked=%zu exiting=%zu "
         "deferred_create=%zu born_clean=%zu exit_transition=%zu destroy_deferred=%zu stw_calls=%zu "
         "stack_scanned=%zu stack_fallback=%zu stack_frames=%zu wlock_us=%llu "
         "env=MRT_GCV2_EPOCH_HANDSHAKE=1 env_scan=MRT_GCV2_CONCURRENT_STACK_SCAN",
         source, static_cast<unsigned long long>(stats.epoch), stats.requested, stats.acked, stats.ackedTwice,
         stats.selfAck, stats.gcAssistedAck, stats.startingAck, stats.runningAck, stats.parkedAck,
         stats.exitingAck, stats.deferredCreates, stats.bornCleanJoins, stats.exitTransitions,
         stats.destroyDeferred, stats.stopTheWorldCalls, stats.stackScanned, stats.stackFallback, stats.stackFrames,
         static_cast<unsigned long long>(stats.managementLockNanos / 1000));
    return stats;
}

extern "C" MRT_EXPORT uint64_t MRT_RunEpochHandshake()
{
    return MutatorManager::Instance().RunEpochHandshake("explicit").epoch;
}

void MutatorManager::AcquireMutatorManagementWLock()
{
    // Announce the pending writer so readers back off (writer-preference), then spin on
    // the non-blocking write-lock acquisition. Without this, sustained mutator-list
    // reader churn (many cjthreads registering/unregistering under heavy parallel
    // compilation) keeps the lock count above zero and starves this acquisition until
    // the watchdog below fires a false-positive "deadlock".
    AnnounceMgmtWriterPending();
    uint64_t start = TimeUtil::NanoSeconds();
    bool acquired = TryAcquireMutatorManagementWLock();
    while (!acquired) {
        TimeUtil::SleepForNano(WAIT_LOCK_INTERVAL);
        acquired = TryAcquireMutatorManagementWLock();
        uint64_t now = TimeUtil::NanoSeconds();
        if (!acquired && ((now - start) / SECOND_TO_NANO_SECOND > GetWaitLockTimeoutSec())) {
            LOG(RTLOG_FATAL, "Wait mutator list lock timeout");
        }
    }
    WithdrawMgmtWriterPending();
}

bool MutatorManager::AcquireMutatorManagementWLockForCpuProfile()
{
    AnnounceMgmtWriterPending();
    uint64_t start = TimeUtil::NanoSeconds();
    bool acquired = TryAcquireMutatorManagementWLock();
    while (!acquired) {
        TimeUtil::SleepForNano(WAIT_LOCK_INTERVAL);
        acquired = TryAcquireMutatorManagementWLock();
        uint64_t now = TimeUtil::NanoSeconds();
        if (!acquired && ((now - start) / SECOND_TO_NANO_SECOND > GetWaitLockTimeoutSec())) {
            LOG(RTLOG_FATAL, "Wait mutator list lock timeout");
        }
        if (!CpuProfiler::GetInstance().GetGenerator().GetIsStart()) {
            break;
        }
    }
    WithdrawMgmtWriterPending();
    return acquired;
}

// Visit all mutators, hold mutatorListLock firstly
void MutatorManager::VisitAllMutators(MutatorVisitor func)
{
    ScheduleAllCJThreadVisitMutator(VisitMuatorHelper, &func);
    Mutator* mutator = Heap::GetHeap().GetFinalizerProcessor().GetMutator();
    if (mutator != nullptr) {
        func(*mutator);
    }
}

void MutatorManager::VisitAllMutatorsExceptFinalizer(MutatorVisitor func)
{
    ScheduleAllCJThreadVisitMutator(VisitMuatorHelper, &func);
}

void MutatorManager::StopTheWorld(bool syncGCPhase, GCPhase phase)
{
    // stackwm #5: exposure-hook slow path must not introduce STW (assertion ④).
    StackExposureHook::NoteStopTheWorldFromHook();
    if (UNLIKELY(inEpochHandshake)) {
        epochHandshakeStopTheWorldCalls.fetch_add(1, std::memory_order_relaxed);
        CHECK_DETAIL(false, "epoch handshake path must not call StopTheWorld");
    }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool saferegionEntered = false;
    // Ensure an active mutator entered saferegion before STW (aka. stop all other mutators).
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr) {
            saferegionEntered = mutator->EnterSaferegion(true);
        }
    }
#endif
    // Block if another thread is holding the syncMutex.
    // Prevent multi-thread doing STW concurrently.
    syncMutex.lock();
    syncTriggered.store(true);

    AcquireMutatorManagementWLock();

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // If current mutator saferegion state changed,
    // we should restore it after the mutator called StartTheWorld().
    saferegionStateChanged = saferegionEntered;
#endif

    size_t mutatorCount = GetMutatorCount();
    if (UNLIKELY(mutatorCount == 0)) {
        worldStopped.store(true, std::memory_order_release);
        if (syncGCPhase) { TransitionAllMutatorsToGCPhase(phase); }
        return;
    }
    // set mutatorCount as countOfMutatorsToStop.
    SetSuspensionMutatorCount(static_cast<uint32_t>(mutatorCount));
    DemandSuspensionForSync();
    WaitUntilAllMutatorStopped();

    // the world is stopped.
    worldStopped.store(true, std::memory_order_release);
    if (syncGCPhase) { TransitionAllMutatorsToGCPhase(phase); }
}

void MutatorManager::StartTheWorld() noexcept
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool shouldLeaveSaferegion = saferegionStateChanged;
#endif
    syncTriggered.store(false);
    worldStopped.store(false, std::memory_order_release);

    CancelSuspensionAfterSync();
    SetSuspensionMutatorCount(0);

    // wakeup all mutators which blocking on countOfMutatorsToStop futex.
#if defined(_WIN64) || defined(__APPLE__)
    WakeAllMutators();
#else
    (void)MapleRuntime::Futex(GetSyncFutexWord(), FUTEX_WAKE, INT_MAX);
#endif

    MutatorManagementWUnlock();

    // Release syncMutex to allow other thread call STW.
    syncMutex.unlock();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // Restore saferegion state if the state is changed when mutator calls StopTheWorld().
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr && shouldLeaveSaferegion) {
            (void)mutator->LeaveSaferegion();
        }
    }
#endif
}

void MutatorManager::StartLightSync(bool syncGCPhase, GCPhase phase)
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool saferegionEntered = false;
    // Ensure an active mutator entered saferegion before stw.
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr) {
            saferegionEntered = mutator->EnterSaferegion(true);
        }
    }
#endif
    // Block if another thread is holding the syncMutex.
    // Prevent multi-thread doing lsync concurrently.
    syncMutex.lock();
    syncTriggered.store(true);

    AcquireMutatorManagementWLock();

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // If current mutator saferegion state changed,
    // we should restore it after the mutator called StopLightSync().
    saferegionStateChanged = saferegionEntered;
#endif

    size_t mutatorCount = GetMutatorCount();
    if (UNLIKELY(mutatorCount == 0)) {
        worldStopped.store(true, std::memory_order_release);
    } else {
        // set mutatorCount as countOfMutatorsToStop.
        SetSuspensionMutatorCount(static_cast<uint32_t>(mutatorCount));
        DemandSuspensionForSync();
        WaitUntilAllMutatorStopped();
        worldStopped.store(true, std::memory_order_release);
    }

    DLOG(GCPHASE, "transition gc: %s(%u) -> %s(%u)",
         Collector::GetGCPhaseName(Heap::GetHeap().GetGCPhase()), Heap::GetHeap().GetGCPhase(),
         Collector::GetGCPhaseName(phase), phase);

    // Set global gc phase in the scope of mutatorlist lock
    Heap::GetHeap().InstallBarrier(phase);
    Heap::GetHeap().SetGCPhase(phase);
    lightSyncGCPhase = phase;
    undoneLightSyncMutators.clear();
    // Broadcast mutator phase transition signal to all mutators
    VisitAllMutators([this](Mutator& mutator) {
        mutator.SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_GC_PHASE);
        mutator.SetSafepointActive(true);
        this->undoneLightSyncMutators.push_back(&mutator);
    });
}

void MutatorManager::StopLightSync() noexcept
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool shouldLeaveSaferegion = saferegionStateChanged;
#endif

    syncTriggered.store(false);
    worldStopped.store(false, std::memory_order_release);

    CancelSuspensionAfterSync();
    SetSuspensionMutatorCount(0);

    // wakeup all mutators which blocking on countOfMutatorsToStop futex.
#if defined(_WIN64) || defined(__APPLE__)
    WakeAllMutators();
#else
    (void)MapleRuntime::Futex(GetSyncFutexWord(), FUTEX_WAKE, INT_MAX);
#endif
    EnsurePhaseTransition(lightSyncGCPhase, undoneLightSyncMutators);
    MutatorManagementWUnlock();
    // Release syncMutex to allow other thread call lsync.
    syncMutex.unlock();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // Restore saferegion state if the state is changed when mutator calls StartLightSync().
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr && shouldLeaveSaferegion) {
            (void)mutator->LeaveSaferegion();
        }
    }
#endif
}

void MutatorManager::WaitUntilAllMutatorStopped()
{
    uint64_t beginTime = TimeUtil::MilliSeconds();
    std::list<Mutator*> unstoppedMutators;
    auto func = [&unstoppedMutators](Mutator& mutator) {
        if ((!mutator.InSaferegion())) {
            unstoppedMutators.emplace_back(&mutator);
        }
    };
    VisitAllMutators(func);

    size_t remainMutatorsSize = unstoppedMutators.size();
    if (remainMutatorsSize == 0) {
        return;
    }

    // Synchronize operation to ensure that all mutators complete phase transition
    // Use unstoppedMutators to avoid traversing the entire mutatorList
    int timeoutTimes = 0;
    while (true) {
        for (auto it = unstoppedMutators.begin(); it != unstoppedMutators.end();) {
            Mutator* mutator = *it;
            if (mutator->InSaferegion()) {
                // current it(mutator) is finished by GC
                it = unstoppedMutators.erase(it);
            } else {
                ++it; // skip current round & check it next round
            }
        }

        if (unstoppedMutators.size() == 0) {
            return;
        }

        if (UNLIKELY(TimeUtil::MilliSeconds() - beginTime >
            (((remainMutatorsSize / STW_TIMEOUTS_THREADS_BASE_COUNT) * STW_TIMEOUTS_BASE_MS) + STW_TIMEOUTS_BASE_MS))) {
            timeoutTimes++;
            beginTime = TimeUtil::MilliSeconds();
            DumpMutators(timeoutTimes);
        }

        (void)sched_yield();
    }
}

void MutatorManager::EnsurePhaseTransition(GCPhase phase, std::list<Mutator*> &undoneMutators)
{
    // Traverse through undoneMutators to select mutators that have not yet completed transition
    // 1. ignore mutators which have completed transition
    // 2. gc compete phase transition with mutators which are in saferegion
    // 3. fill mutators which are running state in undoneMutators
    while (undoneMutators.size() > 0) {
        for (auto it = undoneMutators.begin(); it != undoneMutators.end();) {
            Mutator* mutator = *it;
            if (mutator->GetMutatorPhase() == phase && mutator->FinishedTransition()) {
                it = undoneMutators.erase(it);
                continue;
            }
            if (mutator->InSaferegion() && mutator->TransitionGCPhase(false)) {
                it = undoneMutators.erase(it);
                continue;
            }
            ++it;
        }
    }
}

void MutatorManager::TransitionAllMutatorsToGCPhase(GCPhase phase)
{
    // Try to occupy mutatorListLock prevent some mutators from exiting
    bool worldStopped = WorldStopped();
    if (!worldStopped) {
        AcquireMutatorManagementWLock();
    }

    DLOG(GCPHASE, "transition gc: %s(%u) -> %s(%u)",
         Collector::GetGCPhaseName(Heap::GetHeap().GetGCPhase()), Heap::GetHeap().GetGCPhase(),
         Collector::GetGCPhaseName(phase), phase);

    // Set global gc phase in the scope of mutatorlist lock
    Heap::GetHeap().InstallBarrier(phase);
    Heap::GetHeap().SetGCPhase(phase);

    std::list<Mutator*> undoneMutators;
    // Broadcast mutator phase transition signal to all mutators
    VisitAllMutators([&undoneMutators](Mutator& mutator) {
        mutator.SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_GC_PHASE);
        mutator.SetSafepointActive(true);
        undoneMutators.push_back(&mutator);
    });
    EnsurePhaseTransition(phase, undoneMutators);
    if (!worldStopped) {
        MutatorManagementWUnlock();
    }
}

void MutatorManager::EnsureCpuProfileFinish(std::list<Mutator*> &undoneMutators)
{
    while (undoneMutators.size() > 0) {
        for (auto it = undoneMutators.begin(); it != undoneMutators.end();) {
            Mutator* mutator = *it;
            if (mutator->FinishedCpuProfile()) {
                it = undoneMutators.erase(it);
                continue;
            }
            if (mutator->InSaferegion() && mutator->TransitionToCpuProfile(false)) {
                it = undoneMutators.erase(it);
                continue;
            }
            if (!CpuProfiler::GetInstance().GetGenerator().GetIsStart()) {
                mutator->ClearSuspensionFlag(Mutator::SUSPENSION_FOR_CPU_PROFILE);
                mutator->SetCpuProfileState(Mutator::FINISH_CPUPROFILE);
                it = undoneMutators.erase(it);
                continue;
            }
            ++it;
        }
    }
}

void MutatorManager::TransitionAllMutatorsToCpuProfile()
{
    bool worldStopped = WorldStopped();
    if (!worldStopped) {
        if (!AcquireMutatorManagementWLockForCpuProfile()) {
            return;
        }
    }
    std::list<Mutator*> undoneMutators;
    VisitAllMutatorsExceptFinalizer([&undoneMutators](Mutator& mutator) {
        if (mutator.GetCjthreadPtr() == MutatorManager::Instance().GetMainThreadHandle()) {
            mutator.SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_CPU_PROFILE);
            mutator.SetSafepointActive(true);
            undoneMutators.push_back(&mutator);
        }
    });
    EnsureCpuProfileFinish(undoneMutators);
    if (!worldStopped) {
        MutatorManagementWUnlock();
    }
}

void MutatorManager::DumpMutators(uint32_t timeoutTimes)
{
    constexpr size_t bufferSize = 4096;
    char buf[bufferSize];
    int index = 0;
    size_t visitedCount = 0;
    size_t visitedSaferegion = 0;
    int firstNotStoppedTid = -1;
    index += sprintf_s(buf, sizeof(buf), "not stopped: ");
    CHECK_DETAIL(index != -1, "Dump mutators state failed");
    size_t mutatorCount = 0;
    VisitAllMutators([&](const Mutator& mut) {
        mutatorCount++;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        mut.DumpMutator();
#endif
        if (!mut.InSaferegion()) {
            if (firstNotStoppedTid == -1) {
                firstNotStoppedTid = static_cast<int>(mut.GetTid());
            }
            int ret = sprintf_s(buf + index, sizeof(buf) - index, "%u ", mut.GetTid());
            CHECK_DETAIL(ret != -1, "Dump mutators state failed");
            index += ret;
        } else {
            ++visitedSaferegion;
        }
        ++visitedCount;
    });
    LOG(RTLOG_ERROR, "MutatorList size : %zu", mutatorCount);

    CHECK_DETAIL(sprintf_s(buf + index, sizeof(buf) - index, ", total: %u, visited: %zu/%zu",
                           GetSuspensionMutatorCount(), visitedSaferegion, visitedCount) != -1,
                 "Dump mutators state failed");
    CHECK_DETAIL(timeoutTimes <= MAX_TIMEOUT_TIMES, "Waiting mutators entering saferegion timeout status info:%s", buf);
    LOG(RTLOG_ERROR, "STW status info:%s", buf);
}

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void MutatorManager::DumpForDebug()
{
    size_t count = 0;
    auto func = [&count](Mutator& mutator) {
        mutator.DumpMutator();
        count++;
    };
    VisitAllMutators(func);
    LOG(RTLOG_INFO, "MutatorList size : %zu", count);
}

void MutatorManager::DumpAllGcInfos()
{
    auto func = [](Mutator& mutator) { mutator.DumpGCInfos(); };
    VisitAllMutators(func);
}
#endif

extern "C" void MRT_FlushGCInfo()
{
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    // MutatorManager::Instance().DumpAllGcInfos();
    Mutator::GetMutator()->DumpGCInfos();
#endif
}

#ifdef __APPLE__
extern "C" MRT_EXPORT void CJ_MRT_FlushGCInfo();
__asm__(".global _CJ_MRT_FlushGCInfo\n\t.set _CJ_MRT_FlushGCInfo, _MRT_FlushGCInfo");
#else
extern "C" MRT_EXPORT void CJ_MRT_FlushGCInfo() __attribute__((alias("MRT_FlushGCInfo")));
#endif
} // namespace MapleRuntime
