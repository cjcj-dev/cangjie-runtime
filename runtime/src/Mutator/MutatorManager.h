// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MUTATOR_MANAGER_H
#define MRT_MUTATOR_MANAGER_H

#include <bitset>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Base/AtomicSpinLock.h"
#include "Base/GcLog.h"
#include "Heap/z/zStat.hpp"
#include "Base/Globals.h"
#include "Base/Panic.h"
#include "Base/RwLock.h"
#include "Common/PageAllocator.h"
#include "Mutator.h"
#include "VMOperation.h"
#include "SafepointSynchronize.h"
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
    friend class MutatorManagerTest;
    friend class ZMark;
    friend class VMThread;
public:
    MutatorManager() {}
    ~MutatorManager() = default;

    MutatorManager(const MutatorManager&) = delete;
    MutatorManager(MutatorManager&&) = delete;
    MutatorManager& operator=(const MutatorManager&) = delete;
    MutatorManager& operator=(MutatorManager&&) = delete;

    static MutatorManager& Instance() noexcept;

    void Init();
    void Fini() {}

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
        return SafepointSynchronize::state().mutatorManagementRWLock.TryLockWrite();
    }

    bool TryAcquireMutatorManagementRLock()
    {
        // Writer-preference: defer to a pending writer so a non-blocking reader does
        // not help starve the GC's mutator-list write lock.
        if (SafepointSynchronize::state().mgmtWritersWaiting.load(std::memory_order_acquire) > 0) {
            return false;
        }
        if (!SafepointSynchronize::state().mutatorManagementRWLock.TryLockRead()) {
            return false;
        }
        // Close the race between the pending-writer check above and TryLockRead().
        // A reader that overlapped a writer announcement must not join the reader
        // set and extend the writer's wait indefinitely.
        if (SafepointSynchronize::state().mgmtWritersWaiting.load(std::memory_order_acquire) > 0) {
            SafepointSynchronize::state().mutatorManagementRWLock.UnlockRead();
            return false;
        }
        return true;
    }

    // Announce/withdraw a pending writer so readers back off. Used by the spin-based
    // write-lock acquisition (AcquireMutatorManagementWLock*) which does not go through
    // MutatorManagementWLock(). A counter (not a flag) supports several writers spinning
    // concurrently; readers wait while any writer is pending.
    void AnnounceMgmtWriterPending() { SafepointSynchronize::state().mgmtWritersWaiting.fetch_add(1, std::memory_order_acq_rel); }

    void WithdrawMgmtWriterPending() { SafepointSynchronize::state().mgmtWritersWaiting.fetch_sub(1, std::memory_order_acq_rel); }

    void AcquireMutatorManagementWLock();
    void AcquireMutatorManagementWLockForExit(Mutator& mutator);
    // threads.cpp:1089-1114: single removal transaction (detach publication,
    // list removal, GC-detached transition) in the pause-exclusive section.
    void RemoveMutator(Mutator& mutator);

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
    // Waiting driver/finalizer threads service the same M4 operation queue.
    bool MarkFlushHandshakeActive() const { return Handshake::Current().has_operation(); }
    bool AcknowledgeMarkFlushForCurrentThread();
    void VisitMarkingThreads(const std::function<void(const ThreadGCData*)>& visitor);
    void VisitStoreBarrierBuffers(const std::function<void(MAddress)>& visitor);
    void RegisterMarkFlushThread(ThreadLocalData* tls);
    void UnregisterMarkFlushThread(ThreadLocalData* tls);
    bool TlsHasMarkFlushPending(ThreadLocalData* tls);

    // Some functions about stw
private:
    void StopTheWorld();
    void StartTheWorld() noexcept;
public:
    void WaitUntilAllMutatorStopped();
    void DumpMutators(uint32_t timeoutTimes);
    void DemandSuspensionForSync();

    void CancelSuspensionAfterSync()
    {
        VisitAllMutators([](Mutator& mutator) {
            mutator.ClearSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_SYNC);
        });
    }

    void TriggerMutatorSuspension(Mutator& mutator) const
    {
        mutator.SetSafepointActive(true);
    }

    void RemoveMutatorSuspensionTrigger(Mutator& mutator) const
    {
        mutator.ClearSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_SYNC);
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

    bool WorldStopped() const { return SafepointSynchronize::state().worldStopped.load(std::memory_order_acquire); }

    bool SyncTriggered() const { return SafepointSynchronize::state().syncTriggered.load(); }

    void SyncMutexLock() { SafepointSynchronize::state().syncMutex.lock(); }

    void SyncMutexUnlock() noexcept { SafepointSynchronize::state().syncMutex.unlock(); }

    static bool ConcurrentStackScanEnabled();

    void TransitionAllMutatorsToCpuProfile();


    __attribute__((always_inline)) inline int* GetSyncFutexWord()
    {
        return reinterpret_cast<int*>(&SafepointSynchronize::state().suspensionMutatorCount);
    }

    __attribute__((always_inline)) inline int GetSyncFutexWordValue() const
    {
        return GetSuspensionMutatorCount();
    }

    __attribute__((always_inline)) inline uint32_t GetSuspensionMutatorCount() const
    {
        return SafepointSynchronize::state().suspensionMutatorCount.load(std::memory_order_acquire);
    }

    __attribute__((always_inline)) inline void SetSuspensionMutatorCount(uint32_t total)
    {
        SafepointSynchronize::state().suspensionMutatorCount.store(total, std::memory_order_release);
    }

#if defined(_WIN64) || defined (__APPLE__)
    __attribute__((always_inline)) inline void MutatorWait()
    {
        std::unique_lock<std::mutex> mutatorsToStopLck(SafepointSynchronize::state().mutatorSuspensionMtx);
        SafepointSynchronize::state().mutatorSuspensionCV.wait(mutatorsToStopLck, [this]() {
            return GetSuspensionMutatorCount() == 0;
        });
    }

    __attribute__((always_inline)) inline void WakeAllMutators()
    {
        std::unique_lock<std::mutex> mutatorsToStopLck(SafepointSynchronize::state().mutatorSuspensionMtx);
        SafepointSynchronize::state().mutatorSuspensionCV.notify_all();
    }
#endif

#if not defined (_WIN64)
    const SafepointPageManager* GetSafepointPageManager() const { return SafepointSynchronize::state().safepointPageManager; }
#endif

    void MutatorManagementRLock()
    {
        // Writer-preference: block new readers while a writer is pending so the GC's
        // mutator-list write lock cannot be starved by sustained reader churn (many
        // cjthreads registering/unregistering under heavy parallel compilation). A
        // reader waits here in the same state it would wait for an already-held write
        // lock, so this adds no new stop-the-world deadlock.
        for (;;) {
            while (SafepointSynchronize::state().mgmtWritersWaiting.load(std::memory_order_acquire) > 0) {
                (void)sched_yield();
            }
            SafepointSynchronize::state().mutatorManagementRWLock.LockRead();
            // A writer may announce itself after the check above but before LockRead
            // increments the reader count. Back out in that case. Once a writer is
            // pending, only the finite set of readers which completed both checks
            // before the announcement can remain, so the writer must make progress.
            if (SafepointSynchronize::state().mgmtWritersWaiting.load(std::memory_order_acquire) == 0) {
                return;
            }
            SafepointSynchronize::state().mutatorManagementRWLock.UnlockRead();
        }
    }

    void MutatorManagementRUnlock() { SafepointSynchronize::state().mutatorManagementRWLock.UnlockRead(); }

    void MutatorManagementWLock()
    {
        AnnounceMgmtWriterPending();
        SafepointSynchronize::state().mutatorManagementRWLock.LockWrite();
        WithdrawMgmtWriterPending();
    }

    void MutatorManagementWUnlock() { SafepointSynchronize::state().mutatorManagementRWLock.UnlockWrite(); }


    bool HasNativeMutator();

    void SetMainThreadHandle(CJThreadHandle handle) { mainThreadHandle = handle; }

    CJThreadHandle GetMainThreadHandle() { return mainThreadHandle; }

    template<typename Fn>
    void ForEachMarkFlushTls(Fn&& fn)
    {
        std::lock_guard<std::mutex> lock(SafepointSynchronize::state().markFlushThreadMutex);
        for (auto* tls : SafepointSynchronize::state().markFlushThreads) {
            if (tls != nullptr) {
                fn(tls);
            }
        }
    }

    private:

    Mutator* nativeMutator = nullptr;
    CJThreadHandle mainThreadHandle = nullptr;
};

} // namespace MapleRuntime
#endif
