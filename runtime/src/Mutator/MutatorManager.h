// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MUTATOR_MANAGER_H
#define MRT_MUTATOR_MANAGER_H

#include <bitset>
#include <list>
#include <mutex>
#include <unordered_set>

#include "Base/AtomicSpinLock.h"
#include "Base/Globals.h"
#include "Base/Panic.h"
#include "Base/RwLock.h"
#include "Common/PageAllocator.h"
#include "Mutator.h"
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
#include "SafepointPageManager.h"
#endif
#include "ThreadLocal.h"
#include "schedule.h"

namespace MapleRuntime {
const uint64_t WAIT_LOCK_INTERVAL = 5000; // 5us
const uint64_t WAIT_LOCK_TIMEOUT = 240;   // seconds (default; was 30). Aligned with the
                                          // stop-the-world base timeout (STW_TIMEOUTS_BASE_MS)
                                          // so the mutator-list write lock is not more aggressive
                                          // than STW under heavy CPU contention. Override via env
                                          // cjMutatorLockTimeout (seconds).
const uint32_t MAX_TIMEOUT_TIMES = 1;
const int STW_TIMEOUTS_THREADS_BASE_COUNT = 100;
// STW wait base timeout in milliseconds, for every 100 threads, the time is increased by 240000ms.
const int STW_TIMEOUTS_BASE_MS = 240000;
const uint32_t LOCK_OWNER_NONE = 0;
const uint32_t LOCK_OWNER_GC = LOCK_OWNER_NONE + 1;
const uint32_t LOCK_OWNER_MUTATOR = LOCK_OWNER_GC + 1;

bool IsRuntimeThread();
bool IsGcThread();
extern "C" void HandleSafepoint(ThreadLocalData* tlData);
#if defined (__arm__)
extern "C" void HandleSafepointForArm(ThreadLocalData* tlData);
#endif

using MutatorVisitor = std::function<void(Mutator&)>;

class MutatorManager {
public:
    MutatorManager() {}
    ~MutatorManager()
    {
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
        if (safepointPageManager != nullptr) {
            delete safepointPageManager;
            safepointPageManager = nullptr;
        }
#endif
    }

    MutatorManager(const MutatorManager&) = delete;
    MutatorManager(MutatorManager&&) = delete;
    MutatorManager& operator=(const MutatorManager&) = delete;
    MutatorManager& operator=(MutatorManager&&) = delete;

    static MutatorManager& Instance() noexcept;

    void Init();
    void Fini() { SatbBuffer::Instance().Fini(); }

    // Get the mutator list instance
    size_t GetMutatorCount()
    {
        size_t count = 0;
        auto func = [&count](Mutator&) { count++; };
        VisitAllMutators(func);
        return count;
    }

    void GetAllMutators(std::vector<Mutator*>& mutators)
    {
        auto func = [&mutators](Mutator& mutator) { mutators.emplace_back(&mutator); };
        VisitAllMutators(func);
    }

    bool TryAcquireMutatorManagementWLock()
    {
        return mutatorManagementRWLock.TryLockWrite();
    }

    bool TryAcquireMutatorManagementRLock()
    {
        // Writer-preference: defer to a pending writer so a non-blocking reader does
        // not help starve the GC's mutator-list write lock. The caller (DestroyMutator)
        // already handles a false return by deferring the mutator to expiringMutators.
        if (mgmtWritersWaiting.load(std::memory_order_acquire) > 0) {
            return false;
        }
        return mutatorManagementRWLock.TryLockRead();
    }

    // Announce/withdraw a pending writer so readers back off. Used by the spin-based
    // write-lock acquisition (AcquireMutatorManagementWLock*) which does not go through
    // MutatorManagementWLock(). A counter (not a flag) supports several writers spinning
    // concurrently; readers wait while any writer is pending.
    void AnnounceMgmtWriterPending() { mgmtWritersWaiting.fetch_add(1, std::memory_order_acq_rel); }

    void WithdrawMgmtWriterPending() { mgmtWritersWaiting.fetch_sub(1, std::memory_order_acq_rel); }

    void AcquireMutatorManagementWLock();

    bool AcquireMutatorManagementWLockForCpuProfile();

    static void VisitMuatorHelper(void* argPtr, void* handle)
    {
        Mutator* mutator = reinterpret_cast<Mutator*>(argPtr);
        if (mutator != nullptr) {
            (*reinterpret_cast<MutatorVisitor*>(handle))(*mutator);
        }
    }

    // Visit all mutators, hold mutatorListLock firstly
    void VisitAllMutators(MutatorVisitor func);
    void VisitAllMutatorsExceptFinalizer(MutatorVisitor func);

    // Some functions about stw
    void StopTheWorld(bool syncGCPhase, GCPhase phase);
    void StartTheWorld() noexcept;
    void StartLightSync(bool syncGCPhase, GCPhase phase);
    void StopLightSync() noexcept;
    void WaitUntilAllMutatorStopped();
    void DumpMutators(uint32_t timeoutTimes);
    void DemandSuspensionForSync()
    {
        VisitAllMutators([](Mutator& mutator) {
            mutator.SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_SYNC);
            mutator.SetSafepointActive(true);
        });
    }

    void CancelSuspensionAfterSync()
    {
        VisitAllMutators([](Mutator& mutator) {
            mutator.SetSafepointActive(false);
            mutator.ClearSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_SYNC);
        });
    }

    void TriggerMutatorSuspension(Mutator& mutator) const
    {
        mutator.SetSafepointActive(true);
    }

    void RemoveMutatorSuspensionTrigger(Mutator& mutator) const
    {
        mutator.SetSafepointActive(false);
    }

    void BindMutator(Mutator& mutator) const;
    void UnbindMutator(Mutator& mutator) const;

    // Create and initialize the local mutator, then register to mutatorlist.
    Mutator* CreateMutator();

    // Delete the thread-local mutator and unregister from mutatorlist before a mutator exit
    void TransitMutatorToExit();

    void DestroyMutator(Mutator* mutator);

    Mutator* CreateRuntimeMutator(ThreadType threadType) __attribute__((noinline));
    void DestroyRuntimeMutator(ThreadType threadType);

    bool WorldStopped() const { return worldStopped.load(std::memory_order_acquire); }

    bool SyncTriggered() const { return syncTriggered.load(); }

    void SyncMutexLock() { syncMutex.lock(); }

    void SyncMutexUnlock() noexcept { syncMutex.unlock(); }

    void EnsurePhaseTransition(GCPhase phase, std::list<Mutator*> &undoneMutators);
    void TransitionAllMutatorsToGCPhase(GCPhase phase);

    void EnsureCpuProfileFinish(std::list<Mutator*> &undoneMutators);
    void TransitionAllMutatorsToCpuProfile();

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    void DumpForDebug();
    void DumpAllGcInfos();
#endif

    __attribute__((always_inline)) inline int* GetSyncFutexWord()
    {
        return reinterpret_cast<int*>(&suspensionMutatorCount);
    }

    __attribute__((always_inline)) inline int GetSyncFutexWordValue() const
    {
        return GetSuspensionMutatorCount();
    }

    __attribute__((always_inline)) inline uint32_t GetSuspensionMutatorCount() const
    {
        return suspensionMutatorCount.load(std::memory_order_acquire);
    }

    __attribute__((always_inline)) inline void SetSuspensionMutatorCount(uint32_t total)
    {
        suspensionMutatorCount.store(total, std::memory_order_release);
    }

#if defined(_WIN64) || defined (__APPLE__)
    __attribute__((always_inline)) inline void MutatorWait()
    {
        std::unique_lock<std::mutex> mutatorsToStopLck(mutatorSuspensionMtx);
        mutatorSuspensionCV.wait(mutatorsToStopLck, [this]() {
            return GetSuspensionMutatorCount() == 0;
        });
    }

    __attribute__((always_inline)) inline void WakeAllMutators()
    {
        std::unique_lock<std::mutex> mutatorsToStopLck(mutatorSuspensionMtx);
        mutatorSuspensionCV.notify_all();
    }
#endif

#if not defined (_WIN64)
    const SafepointPageManager* GetSafepointPageManager() const { return safepointPageManager; }
#endif

    void MutatorManagementRLock()
    {
        // Writer-preference: block new readers while a writer is pending so the GC's
        // mutator-list write lock cannot be starved by sustained reader churn (many
        // cjthreads registering/unregistering under heavy parallel compilation). A
        // reader waits here in the same state it would wait for an already-held write
        // lock, so this adds no new stop-the-world deadlock.
        while (mgmtWritersWaiting.load(std::memory_order_acquire) > 0) {
            (void)sched_yield();
        }
        mutatorManagementRWLock.LockRead();
    }

    void MutatorManagementRUnlock() { mutatorManagementRWLock.UnlockRead(); }

    void MutatorManagementWLock()
    {
        AnnounceMgmtWriterPending();
        mutatorManagementRWLock.LockWrite();
        WithdrawMgmtWriterPending();
    }

    void MutatorManagementWUnlock() { mutatorManagementRWLock.UnlockWrite(); }

    void DestroyExpiredMutators();

    bool HasNativeMutator();

    void SetMainThreadHandle(CJThreadHandle handle) { mainThreadHandle = handle; }

    CJThreadHandle GetMainThreadHandle() { return mainThreadHandle; }

private:
    using ExpiredMutatorList = std::list<Mutator*, StdContainerAllocator<Mutator*, MUTATOR_LIST>>;
    ExpiredMutatorList expiringMutators;
    std::mutex expiringMutatorListLock;

    // guard mutator set for stop-the-world/light-sync
    RwLock mutatorManagementRWLock;

    // Number of writers currently waiting for mutatorManagementRWLock. Read by the
    // read-lock paths to implement writer-preference (see MutatorManagementRLock /
    // TryAcquireMutatorManagementRLock), preventing GC write-lock starvation.
    std::atomic<uint32_t> mgmtWritersWaiting{ 0 };

    // count of mutators need to be suspended for stw/lsync.
    // this field is also used as futex wait/wakeup word for stw/lsync.
    std::atomic<uint32_t> suspensionMutatorCount;

    /*
    The following variables with sync are shared by stw and light sync.
    The STW starts the world after all mutators finish GC phase transition.
    The light sync starts the world immediately after all mutators stop and
    does not need to wait for all mutators to finish GC phase transition.
    */
    std::recursive_mutex syncMutex;
    std::atomic<bool> syncTriggered = { false };
    std::atomic<bool> worldStopped = { false };
    std::list<Mutator*> undoneLightSyncMutators;
    GCPhase lightSyncGCPhase;

#if defined(_WIN64) || defined (__APPLE__)
    std::condition_variable mutatorSuspensionCV;
    std::mutex mutatorSuspensionMtx;
#endif
#if not defined (_WIN64)
    SafepointPageManager* safepointPageManager = nullptr;
#endif
    Mutator* nativeMutator = nullptr;
    CJThreadHandle mainThreadHandle = nullptr;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool saferegionStateChanged = false;
#endif
};

// Scoped stop the world.
class ScopedStopTheWorld {
public:
    __attribute__((always_inline)) explicit ScopedStopTheWorld(const char* gcReason, bool syncGCPhase = false,
        GCPhase phase = GC_PHASE_IDLE) : reason(gcReason)
    {
        startTime = TimeUtil::NanoSeconds();
        MutatorManager::Instance().StopTheWorld(syncGCPhase, phase);
    }

    __attribute__((always_inline)) ~ScopedStopTheWorld()
    {
        LOG(RTLOG_REPORT, "%s stw time %zu us", reason, GetElapsedTime() / 1000); // 1000:nsec per usec
        MutatorManager::Instance().StartTheWorld();
    }

    uint64_t GetElapsedTime() const { return TimeUtil::NanoSeconds() - startTime; }

private:
    const char* reason = nullptr;
    uint64_t startTime = 0;
};

// Scoped light sync.
class ScopedLightSync {
public:
    __attribute__((always_inline)) explicit ScopedLightSync(const char* gcReason, bool syncGCPhase = false,
        GCPhase phase = GC_PHASE_IDLE) : reason(gcReason)
    {
        startTime = TimeUtil::NanoSeconds();
        MutatorManager::Instance().StartLightSync(syncGCPhase, phase);
    }

    __attribute__((always_inline)) ~ScopedLightSync()
    {
        LOG(RTLOG_REPORT, "%s light sync time %zu us", reason, GetElapsedTime() / 1000); // 1000:nsec per usec
        MutatorManager::Instance().StopLightSync();
    }

    uint64_t GetElapsedTime() const { return TimeUtil::NanoSeconds() - startTime; }

private:
    const char* reason = nullptr;
    uint64_t startTime = 0;
};

// Scoped lock STW, this prevent other thread STW during the current scope.
class ScopedSTWLock {
public:
    __attribute__((always_inline)) explicit ScopedSTWLock() { MutatorManager::Instance().SyncMutexLock(); }

    __attribute__((always_inline)) ~ScopedSTWLock() { MutatorManager::Instance().SyncMutexUnlock(); }
};
} // namespace MapleRuntime
#endif
