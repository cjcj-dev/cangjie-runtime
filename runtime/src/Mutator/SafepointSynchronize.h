// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Base/RwLock.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#if !defined(_WIN64)
#include "SafepointPageManager.h"
#endif

namespace MapleRuntime {
struct ThreadLocalData;
// Preserve the host's shared registration/exclusive safepoint discipline,
// using a platform-blocking lock so final native waiters do not busy-spin.
class ThreadListLock {
public:
    void LockRead() { lock.lock_shared(); }
    void UnlockRead() { lock.unlock_shared(); }
    bool TryLockRead() { return lock.try_lock_shared(); }
    void LockWrite() { lock.lock(); }
    void UnlockWrite() { lock.unlock(); }
    bool TryLockWrite() { return lock.try_lock(); }
private:
    std::shared_timed_mutex lock;
};
// HotSpot mutexLocker.cpp:322 / safepoint.cpp:343,586. Normal pauses and
// the final permanent pause share one process-lifetime synchronization base.
// Runtime owns the manager facade, never the locks awaited by returning native
// threads. No second shutdown-only safepoint mechanism is installed.
struct SafepointState {
    ThreadListLock mutatorManagementRWLock;
    std::atomic<uint32_t> mgmtWritersWaiting{0};
    std::atomic<uint32_t> suspensionMutatorCount{0};
    std::recursive_mutex syncMutex;
    std::atomic<bool> syncTriggered{false};
    std::atomic<bool> worldStopped{false};
    std::mutex markFlushThreadMutex;
    std::unordered_set<ThreadLocalData*> markFlushThreads;
#if defined(_WIN64) || defined(__APPLE__)
    std::condition_variable mutatorSuspensionCV;
    std::mutex mutatorSuspensionMtx;
#endif
#if !defined(_WIN64)
    SafepointPageManager* safepointPageManager = nullptr;
#endif
};

class SafepointSynchronize {
public:
    static SafepointState& state();
    static void initialize();
    static void block();
    static bool is_at_safepoint() { return state().worldStopped.load(std::memory_order_acquire); }
    static bool is_synchronizing() { return state().syncTriggered.load(std::memory_order_acquire); }
};
} // namespace MapleRuntime
