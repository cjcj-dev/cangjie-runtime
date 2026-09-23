// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zAccess.hpp"
#include "Sync.h"
#include "Common/WeakHandle.inline.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.hpp"
#include <atomic>
#include <mutex>

#include "Base/TimeUtils.h"
#include "schedule.h"
#include "Concurrency/ConcurrencyModel.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#ifdef _WIN64
#include <windows.h>
#endif

namespace MapleRuntime {
template<typename T>
static T CastToT(const void* ptr)
{
    return reinterpret_cast<T>(const_cast<void*>(ptr));
}

static OopStorage g_syncWeakStorage;
static std::mutex g_syncNativeListMutex;
static NativeWaitSet* g_waitSets = nullptr;
static NativeMutexWait* g_mutexWaits = nullptr;

OopStorage& SyncWeakOopStorage()
{
    return g_syncWeakStorage;
}

static void LinkWaitSet(NativeWaitSet* n)
{
    std::lock_guard<std::mutex> lg(g_syncNativeListMutex);
    n->next = g_waitSets;
    g_waitSets = n;
}

static void LinkMutexWait(NativeMutexWait* n)
{
    std::lock_guard<std::mutex> lg(g_syncNativeListMutex);
    n->next = g_mutexWaits;
    g_mutexWaits = n;
}

static void DestroyWaitSet(NativeWaitSet* n)
{
    pthread_mutex_destroy(&n->wq.mutex);
    n->object.release(&g_syncWeakStorage);
    delete n;
}

static void DestroyMutexWait(NativeMutexWait* n)
{
    pthread_mutex_destroy(&n->sema.queue.mutex);
    n->object.release(&g_syncWeakStorage);
    delete n;
}

void SyncRetireDead()
{
    std::lock_guard<std::mutex> lg(g_syncNativeListMutex);
    NativeWaitSet** wpp = &g_waitSets;
    while (*wpp != nullptr) {
        NativeWaitSet* n = *wpp;
        if (n->busy.load() == 0 && (n->object.is_null() || n->object.peek() == nullptr)) {
            *wpp = n->next;
            DestroyWaitSet(n);
        } else {
            wpp = &n->next;
        }
    }
    NativeMutexWait** mpp = &g_mutexWaits;
    while (*mpp != nullptr) {
        NativeMutexWait* n = *mpp;
        if (n->busy.load() == 0 && (n->object.is_null() || n->object.peek() == nullptr)) {
            *mpp = n->next;
            DestroyMutexWait(n);
        } else {
            mpp = &n->next;
        }
    }
}

static NativeWaitSet* AllocWaitSet(BaseObject* obj)
{
    NativeWaitSet* n = new NativeWaitSet();
    if (MRT_NewWaitQueue(&n->wq) != 0) {
        delete n;
        return nullptr;
    }
    n->object = WeakHandle(&g_syncWeakStorage, obj);
    LinkWaitSet(n);
    return n;
}

static NativeMutexWait* AllocMutexWait(BaseObject* obj)
{
    NativeMutexWait* n = new NativeMutexWait();
    if (MRT_NewSem(&n->sema) != 0) {
        delete n;
        return nullptr;
    }
    n->object = WeakHandle(&g_syncWeakStorage, obj);
    LinkMutexWait(n);
    return n;
}

#ifdef __cplusplus
extern "C" {
#endif

static constexpr int64_t INVALID_THREAD_ID = -1LL;

void MCC_FutureInit(void* ptr)
{
    CJFuture* future = reinterpret_cast<CJFuture*>(ptr);
    future->completeFlag = false;
    future->isWaitQueueInit = 0;
    future->waitNative = nullptr;
}

bool MCC_FutureIsComplete(void* ptr)
{
    CJFuture* future = CastToT<CJFuture*>(ptr);
#if defined(CANGJIE_TSAN_SUPPORT)
    bool res = future->completeFlag.load();
    if (res) {
        Sanitizer::TsanAcquire(future);
    }
    return res;
#else
    return future->completeFlag.load();
#endif
}

static bool NativeFutureIsComplete(void* nativePtr)
{
    NativeWaitSet* n = reinterpret_cast<NativeWaitSet*>(nativePtr);
    BaseObject* obj = n->object.resolve();
    if (obj == nullptr) {
        return true;
    }
    return MCC_FutureIsComplete(obj);
}

void MRT_FutureWait(const void* ptr, int64_t timeout)
{
    CJFuture* future = CastToT<CJFuture*>(ptr);
    constexpr int newWaitQueueMaxTimes = 32;
    for (int i = 0;;) {
        if (i > newWaitQueueMaxTimes) {
            LOG(RTLOG_ERROR, "FutureWait timeout failed.");
            break;
        }
        int oldWaitQueue = future->isWaitQueueInit.load();
        if (oldWaitQueue == -1) {
            continue;
        }
        if (oldWaitQueue == 1) {
            break;
        }

        if (future->isWaitQueueInit.compare_exchange_weak(oldWaitQueue, -1)) {
            NativeWaitSet* n = AllocWaitSet(from_native_ref(future));
            if (n == nullptr) {
                LOG(RTLOG_ERROR, "waitqueue init failed!\n");
                future->isWaitQueueInit.store(0);
                ++i;
                continue;
            }
            future->waitNative = n;
            future->isWaitQueueInit.store(1);
            break;
        }
    }

    NativeWaitSet* n = future->waitNative;
    if (n == nullptr) {
        return;
    }
    n->busy.fetch_add(1);
    MRT_SuspendWithTimeout(&n->wq, NativeFutureIsComplete, n, timeout);
    n->busy.fetch_sub(1);

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanAcquire(n->object.resolve());
#endif
}

void MCC_FutureNotifyAll(const void* ptr)
{
    CJFuture* future = CastToT<CJFuture*>(ptr);
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanRelease(future, Sanitizer::ReleaseType::K_RELEASE_MERGE);
#endif
    future->completeFlag.store(true);
    int waitQueue = future->isWaitQueueInit.load();
    if (waitQueue == 0 || waitQueue == -1) {
        return;
    }
    NativeWaitSet* n = future->waitNative;
    if (n == nullptr) {
        return;
    }
    MRT_ResumeAll(&n->wq, NULL, n);
}

int MCC_MutexInit(void* ptr)
{
    CJMutex* mutex = reinterpret_cast<CJMutex*>(ptr);
    NativeMutexWait* n = AllocMutexWait(from_native_ref(mutex));
    if (n == nullptr) {
        mutex->isSemaInit = false;
        mutex->waitNative = nullptr;
        return -1;
    }
    mutex->waitNative = n;
    mutex->isSemaInit = true;
    mutex->ownerThreadId = INVALID_THREAD_ID;
    mutex->ownCount = 0;
    return 0;
}

// LOCKED is used in llvm for mutex opt. Don't just modify it here.
const int64_t SPINNING = 0x1;
const int64_t STARVING = 0x2;
const int64_t LOCKED  = 0x4;
const int64_t WAITER_UNIT_SHIFT = 3;
const int64_t WAITER_UNIT = 1 << WAITER_UNIT_SHIFT;
const uint64_t STARVING_THRESHOLD = 1000; // 1000us
const int64_t SPIN_THRESHOLD = 4;

static bool IsSpinning(int64_t state) { return state & SPINNING; }
static bool IsStarving(int64_t state) { return state & STARVING; }
static bool IsLocked(int64_t state) { return state & LOCKED; }
static int64_t GetWaiters(int64_t state) { return state >> WAITER_UNIT_SHIFT; }
static void SetLocked(int64_t& state) { state |= LOCKED; }
static void SetStarving(int64_t& state) { state |= STARVING; }
static void UnsetSpinning(int64_t& state) { state &= ~SPINNING; }
static void IncWaiters(int64_t& state) { state += WAITER_UNIT; }

#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
#if defined(__aarch64__) || defined (__arm__)
#define YIELD_PROCESSOR __asm__ __volatile__("yield")
#elif defined(__x86_64__)
#define YIELD_PROCESSOR __asm__ __volatile__("pause")
#endif // endif of Arch
#else // _WIN64
#define YIELD_PROCESSOR YieldProcessor()
#endif // end of OS

#ifndef YIELD_PROCESSOR
#warning "Processor yield not supported on this architecture."
#define YIELD_PROCESSOR ((void)0)
#endif

static void DoSpin()
{
    static const int spinNum = 30;
    for (int i = 0; i < spinNum; ++i) {
        YIELD_PROCESSOR;
    }
}

static bool MCC_MutexLockSlowPathImpl(CJMutex* mutex, uint64_t count)
{
    int64_t currThreadId = MRT_GetCurrentThreadID();
    if (mutex->ownerThreadId.load(std::memory_order_acquire) == currThreadId) {
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanAcquire(reinterpret_cast<void*>(mutex));
#endif
        mutex->ownCount += count;
        return true;
    }
    bool hasSetSpinFlag = false;
    bool isStarved = false;
    uint64_t firstWaitTime = 0;
    int trySpinCount = 0;
    for (;;) { // The main loop to acquire the mutex
        int64_t currState = mutex->state.load();
        // ========== Do spinning ============
        if (!IsStarving(currState) &&
            IsLocked(currState) &&
            trySpinCount < SPIN_THRESHOLD &&
            ProcessorCanSpin()) {
            // There are four types of threads:
            //   - Owner thread (at most one)
            //   - Newcoming thread
            //   - Spinning thread (at most one)
            //   - Waiter thread (including starved threads)
            // The first newcoming thread may be the spinning thread,
            // or a waked waiter thread will always be the spinning thread.
            if (!hasSetSpinFlag && // If the thread is newcoming (not set the spinning flag),
                !IsSpinning(currState) && // and there are no spinning threads,
                GetWaiters(currState) > 0 && // and there are waiter threads,
                // try to make the current thread as the spinning thread;
                mutex->state.compare_exchange_strong(currState, currState | SPINNING)) {
                // if succeeded, `unlock` will not wake up waiter threads.
                hasSetSpinFlag = true;
            }
            DoSpin();
            trySpinCount += 1;
            continue; // After spinning; restart
        }

        // ========== Prepare new state ==============
        int64_t newState = currState; // Copy the state first
        // If mutex is NOT starving, try to acquire the mutex;
        // so, set LOCKED.
        if (!IsStarving(currState)) {
            SetLocked(newState);
        }
        // If mutex is STARVING or LOCKED, we cannot acquire the mutex;
        // so, increase #waiters.
        if (IsStarving(currState) || IsLocked(currState)) {
            IncWaiters(newState);
        }
        // IF the current thread is starved and mutex is LOCKED,
        // set mutex as STARVING.
        if (isStarved && IsLocked(currState)) {
            SetStarving(newState);
        }
        // Since we have ended up spinning, clear the flag if necessary
        if (hasSetSpinFlag) {
            UnsetSpinning(newState);
        }

        // ========== Set new state ==============
        if (!mutex->state.compare_exchange_strong(currState, newState)) {
            continue; // Fail to set the new state; restart
        }

        // ========== Hold the mutex ==============
        // If the previous state is not LOCKED && STARVING,
        // CAS succeeded in acquiring the mutex.
        if (!IsLocked(currState) && !IsStarving(currState)) {
            mutex->ownCount = count;
            mutex->ownerThreadId.store(currThreadId, std::memory_order_release);
#if defined(CANGJIE_TSAN_SUPPORT)
            Sanitizer::TsanAcquire(reinterpret_cast<void*>(mutex));
#endif
            return true;
        }

        // ========== Put into wait queue ==============
        // If the current thread has already waited,
        // it will be pushed at the front of the queue.
        bool isPushToHead = firstWaitTime != 0;
        if (firstWaitTime == 0) {
            firstWaitTime = TimeUtil::MicroSeconds();
        }
        NativeMutexWait* waitNative = mutex->waitNative;
        waitNative->busy.fetch_add(1);
        MRT_SemAcquire(&waitNative->sema, isPushToHead);
        mutex = reinterpret_cast<CJMutex*>(waitNative->object.resolve());
        waitNative->busy.fetch_sub(1);

        // ========== After wake up ==============
        // If waiting too long, the current thread becomes starved
        isStarved = isStarved || (TimeUtil::MicroSeconds() - firstWaitTime) > STARVING_THRESHOLD;
        // Read the new state
        currState = mutex->state.load();
        if (!IsStarving(currState)) {
            // Retry to acquire the mutex; also, spinning restarts
            trySpinCount = 0;
            // Since The spinning flag is always set by `unlock`,
            // we should set the variable as well.
            hasSetSpinFlag = true;
            continue; // Waked up; restart
        }
        // Mutex is in starvation mode.
        // When the current thread was woken up, ownership was handed off to it directly.
        // There are some invariants:
        //  - only ONE thread can be woken up and reach here,
        //  - mutex cannot be spinning,
        //  - `LOCKED` is not set, and
        //  - #waiters must be positive because `unlock` will not change it under starvation.
        bool valid = !IsLocked(currState) && !IsSpinning(currState) && GetWaiters(currState) > 0;
        MRT_ASSERT(valid, "Sync error: inconsistent mutex state!\n");
        if (!valid) {
            LOG(RTLOG_ERROR, "Sync error: inconsistent mutex state!\n");
            return false;
        }
        // So, the inconsistent state should be fixed here.
        // A trick to use a single statement to fix all states.
        // Fix as: state + LOCKED - WAITER_UNIT
        int64_t delta = LOCKED - WAITER_UNIT;
        // Exit starvation if a non-starved thread is wakeup, or there are no waiters.
        if (!isStarved || GetWaiters(currState) == 1) {
            // Fix as: state + LOCKED - WAITER_UNIT - STARVING
            delta -= STARVING;
        }
        mutex->state.fetch_add(delta);
        MRT_ASSERT(mutex->ownerThreadId.load(std::memory_order_acquire) == INVALID_THREAD_ID,
                   "Sync error: invalid mutex owner\n");
        MRT_ASSERT(mutex->ownCount == 0, "Sync error: invalid mutex owning count\n");
        mutex->ownCount = count;
        mutex->ownerThreadId.store(currThreadId, std::memory_order_release);
#if defined(CANGJIE_TSAN_SUPPORT)
            Sanitizer::TsanAcquire(reinterpret_cast<void*>(mutex));
#endif
        return true;
    }
    return false; // Unreachable
}

/**
 * @brief Acquire the mutex.
 * @param ptr: raw pointer of a `CJMutex`.
 * @param count: number of acquision to hold the mutex.
 * @return true if succeed in holding the mutex.
 * @return false if fail to acquire the mutex.
 */
static bool MCC_MutexLockImpl(const void* ptr, uint64_t count)
{
    CJMutex* mutex = CastToT<CJMutex*>(ptr);
    int64_t expected = 0;
    // Fast path: try to acquire the mutex
    if (mutex->state.compare_exchange_strong(expected, LOCKED)) {
        int64_t currThreadId = MRT_GetCurrentThreadID();
        mutex->ownCount += count;
        mutex->ownerThreadId.store(currThreadId, std::memory_order_release);
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanAcquire(ptr);
#endif
        return true;
    }
    bool res = MCC_MutexLockSlowPathImpl(mutex, count);
    return res;
}

/**
 * @brief Acquire the mutex.
 * @param ptr: raw pointer of a `CJMutex`.
 * Current thread may be blocked until hold the mutex
 */
void MCC_MutexLock(void* ptr) { MCC_MutexLockImpl(ptr, 1); }

void MCC_MutexLockSlowPath(void* ptr)
{
    MCC_MutexLockSlowPathImpl(CastToT<CJMutex*>(ptr), 1);
}

/**
 * @brief Returns false when fail to hold the mutex.
 * @param ptr: raw pointer of a `CJMutex`.
 * Current thread will never be blocked.
 */
bool MCC_MutexTryLock(void* ptr)
{
    CJMutex* mutex = CastToT<CJMutex*>(ptr);
    int64_t currThreadId = MRT_GetCurrentThreadID();
    int64_t currOwner = mutex->ownerThreadId.load(std::memory_order_acquire);
    // Fast path: try to acquire the mutex
    if (currOwner == currThreadId) {
        mutex->ownCount += 1;
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanAcquire(ptr);
#endif
        return true;
    } else if (currOwner != INVALID_THREAD_ID) {
        return false;
    }

    int64_t currState = mutex->state.load();
    if (IsLocked(currState) || IsStarving(currState)) {
        return false;
    }
    // There may be some spinning threads or waited threads,
    // but the current thread can still acquire the mutex.
    if (mutex->state.compare_exchange_strong(currState, currState | LOCKED)) {
        mutex->ownCount += 1;
        mutex->ownerThreadId.store(currThreadId, std::memory_order_release);
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanAcquire(ptr);
#endif
        return true;
    }
    return false;
}

/**
 * @brief Whether current thread held the mutex.
 * @param ptr: raw pointer of a `CJMutex`.
 */
bool MCC_MutexCheckStatus(const void* ptr)
{
    CJMutex* mutex = CastToT<CJMutex*>(ptr);
    int64_t curThreadId = MRT_GetCurrentThreadID();
    return mutex->ownerThreadId.load(std::memory_order_acquire) == curThreadId;
}

/**
 * @brief If mutex is locked recursively, this method should be invoked N times to fully unlock mutex.
 * In Cangjie program, `checkStatus` must be called before this function.
 * @param ptr: raw pointer of a `CJMutex`.
 * @param count: number of acquision to release the mutex.
 */
static void MRT_MutexUnlockImpl(const void* ptr, uint64_t count)
{
    CJMutex* mutex = CastToT<CJMutex*>(ptr);
    MRT_ASSERT(IsLocked(mutex->state.load()), "Sync error: unlock an unlocked mutex");
    // Invariant: ownCount >= count
    uint64_t oldOwnCount = mutex->ownCount;
    mutex->ownCount -= count;
    if (oldOwnCount > count) {
        // Still hold the mutex
        return;
    }
    MRT_ASSERT(oldOwnCount == count, "Incorrect mutex state");
    // Release the mutex
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanRelease(ptr, Sanitizer::ReleaseType::K_RELEASE_MERGE);
#endif
    mutex->ownerThreadId.store(INVALID_THREAD_ID, std::memory_order_release);
    int64_t currState = mutex->state.fetch_sub(LOCKED);
    MRT_ASSERT(IsLocked(currState), "Sync error: unlock an unlocked mutex");
    currState -= LOCKED; // Clear the locked bit
    if (currState == 0) {
        return;
    }
    if (IsStarving(currState)) {
        // Starving mode: handoff mutex ownership to the next waiter.
        // Note 1: LOCKED is not set, the waiter will set it after wakeup.
        // However, mutex is still considered locked because STARVING is set,
        // so, new coming threads will not acquire it.
        // Note 2: #waiters is not decreased.
        MRT_SemRelease(&mutex->waitNative->sema);
        return;
    }
    for (;;) {
        // If there are no waiters, or a thread has already been woken or held the lock,
        // there is no need to wake anyone.
        if (GetWaiters(currState) == 0 || IsLocked(currState) ||
            IsSpinning(currState) || IsStarving(currState)) {
            return;
        }
        // Wake up a thread and make it as the spinning thread.
        int64_t newState = (currState - WAITER_UNIT) | SPINNING;
        if (mutex->state.compare_exchange_strong(currState, newState)) {
            // After designating the spinning thread,
            //  - newcoming threads may hold the mutex and do `unlock`,
            //  - however, ONLY one waited thread will be waked up by the current thread,
            // because no threads except the waked one can reset the spinning state.
            // Thus, mutex will not be starved until the following `release` is done.
            MRT_SemRelease(&mutex->waitNative->sema);
            return;
        }
        currState = mutex->state.load();
    }
}

void MCC_MutexUnlock(const void* ptr) { MRT_MutexUnlockImpl(ptr, 1); }

// =====================================================
// Following functions are used to implement monitors.
// =====================================================
static void MRT_MutexFullyLock(void* ptr, uint64_t count) { MCC_MutexLockImpl(ptr, count); }

/**
 * @brief Fully unlock a mutex.
 * @return false. This function return a `false` value
 * Because it is used as an argument (callback function) of `CJ_MRT_SuspendWithTimeout`.
 * If the callback function returns `false`, the caller thread will be suspended.
 */
static bool MRT_MutexFullyUnlock(void* ptr)
{
    CJMutex* mutex = CastToT<CJMutex*>(ptr);
    MRT_MutexUnlockImpl(ptr, mutex->ownCount);
    return false;
}

/**
 * @brief A warpper of concurrency library APIs.
 */
int MCC_WaitQueueForMonitorInit(void* ptr)
{
    CJMonitor* monitor = reinterpret_cast<CJMonitor*>(ptr);
    NativeWaitSet* n = AllocWaitSet(from_native_ref(monitor));
    if (n == nullptr) {
        monitor->isWaitQueueInit = false;
        monitor->waitNative = nullptr;
        return -1;
    }
    monitor->waitNative = n;
    monitor->isWaitQueueInit = true;
    return 0;
}

int MCC_WaitQueueInit(void* ptr)
{
    CJWaitQueue* queue = reinterpret_cast<CJWaitQueue*>(ptr);
    NativeWaitSet* n = AllocWaitSet(from_native_ref(queue));
    if (n == nullptr) {
        queue->isWaitQueueInit = false;
        queue->waitNative = nullptr;
        return -1;
    }
    queue->waitNative = n;
    queue->isWaitQueueInit = true;
    return 0;
}

bool MonitorWait(CJMutex* mutex, NativeWaitSet* wqNative, int64_t timeout)
{
    if (timeout <= 0) {
        return false;
    }
    NativeMutexWait* mutexNative = mutex->waitNative;
    uint64_t ownCount = mutex->ownCount;
    mutexNative->busy.fetch_add(1);
    wqNative->busy.fetch_add(1);
    bool wakeStatus = MRT_SuspendWithTimeout(&wqNative->wq, MRT_MutexFullyUnlock, mutex, timeout);
    mutex = reinterpret_cast<CJMutex*>(mutexNative->object.resolve());
    MRT_MutexFullyLock(mutex, ownCount);
    wqNative->busy.fetch_sub(1);
    mutexNative->busy.fetch_sub(1);
    return wakeStatus;
}

/**
 * @brief Implemented `Monitor` in C. A monitor is just a wrapper of a mutex and a wait queue.
 * In Cangjie program, `checkStatus` must be called before this function.
 * @param ptr: the "CJMonitor".
 * @param wq: the wait queue.
 * @param timeout: wake up the caller thread after `timeout` nanoseconds if no other threads notify it.
 * @return true if notified by other threads.
 * @return false if timeout.
 */
bool MCC_MonitorWait(const void* ptr, int64_t timeout)
{
    CJMonitor* monitor = CastToT<CJMonitor*>(ptr);
    // True read barrier (not uncolor_bits): pin must land on load-good / to-copy.
    BaseObject* mutexObj =
        HeapAccess<>::oop_load(&(monitor->mutexPtr));
    CJMutex* mutex = reinterpret_cast<CJMutex*>(mutexObj);
    bool ret = MonitorWait(mutex, monitor->waitNative, timeout);
    return ret;
}

/**
 * @brief Notify one thread (randomly picked) blocked on the wait queue.
 * In Cangjie program, `checkStatus` must be called before this function.
 */
void MCC_MonitorNotify(const void* ptr)
{
    MRT_ResumeOne(
        &CastToT<CJMonitor*>(ptr)->waitNative->wq, [](void*) { return false; }, NULL);
}

/**
 * @brief Notify all thread blocked on the wait queue.
 * In Cangjie program, `checkStatus` must be called before this function.
 */
void MCC_MonitorNotifyAll(const void* ptr)
{
    MRT_ResumeAll(
        &CastToT<CJMonitor*>(ptr)->waitNative->wq, [](void*) { return false; }, NULL);
}

bool MCC_MultiConditionMonitorWait(const void* ptr, void* waitQueuePtr, int64_t timeout)
{
    CJMultiConditionMonitor* monitor = CastToT<CJMultiConditionMonitor*>(ptr);
    // Same as MCC_MonitorWait: HeapSlot load must go through the real barrier.
    BaseObject* mutexObj =
        HeapAccess<>::oop_load(&(monitor->mutexPtr));
    CJMutex* mutex = reinterpret_cast<CJMutex*>(mutexObj);
    bool ret = MonitorWait(mutex, CastToT<CJWaitQueue*>(waitQueuePtr)->waitNative, timeout);
    return ret;
}

/**
 * @brief Notify one thread (randomly picked) blocked on the wait queue.
 * In Cangjie program, `checkStatus` must be called before this function.
 */
void MCC_MultiConditionMonitorNotify(const void* ptr __attribute__((unused)), const void* waitQueuePtr)
{
    MRT_ResumeOne(
        &CastToT<CJWaitQueue*>(waitQueuePtr)->waitNative->wq, [](void*) { return false; }, NULL);
}

/**
 * @brief Notify all thread blocked on the wait queue.
 * In Cangjie program, `checkStatus` must be called before this function.
 */
void MCC_MultiConditionMonitorNotifyAll(const void* ptr __attribute__((unused)), const void* waitQueuePtr)
{
    MRT_ResumeAll(
        &CastToT<CJWaitQueue*>(waitQueuePtr)->waitNative->wq, [](void*) { return false; }, NULL);
}

bool MCC_IsThreadObjectInited()
{
    return MRT_GetCurrentCJThreadObject() != nullptr;
}

void* MRT_GetCurrentCJThreadObject()
{
    void* argStart = CJThreadGetArg();
    if (argStart == nullptr) {
        return nullptr;
    }
    RootSlot& root = RootSlotAt(&reinterpret_cast<LWTData*>(argStart)->threadObject);
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanAcquire();
#endif
    // zBarrierSetNMethod.cpp:45-91: heal the root group from its saved
    // guard before a mutator consumes any member. The visitor shares the
    // guard predicate and group lock with StoreCJThreadObject.
    CJThreadRootEntryBarrier();
    auto res = to_object(safe(root.LoadPlain()));
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanRelease(Sanitizer::ReleaseType::K_RELEASE_MERGE);
#endif
    return res;
}

void MCC_SetCurrentCJThreadObject(void* ptr)
{
    LWTData* data = reinterpret_cast<LWTData*>(CJThreadGetArg());
    if (data == nullptr) {
        LOG(RTLOG_FATAL, "CJThread or arg of CJThread is nullptr.");
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanAcquire();
#endif
    StoreCJThreadObject(ptr);
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanRelease(Sanitizer::ReleaseType::K_RELEASE_MERGE);
#endif
}

void MRT_SetCJThreadName(void* handle, uint8_t* name, size_t len)
{
    CJThreadSetName(handle, reinterpret_cast<const char*>(name), len);
}

int64_t MRT_GetCJThreadId(void* handle)
{
    unsigned long long ret = CJThreadGetId(handle);
    // It's safe to cast the return value as int64
    return static_cast<int64_t>(ret);
}

int64_t MRT_GetCJThreadState(void* handle)
{
    int state = CJThreadGetState(handle);
    if (state == 0) {  // 0: IDLE
        return -1;
    }
    // 4: syscall which we treat as 2: running
    state = state == 4 ? 2 : state;
    return static_cast<int64_t>(state);
}

void* MRT_GetCurrentCJThread()
{
    return CJThreadGetHandle();
}

void MRT_ThreadResumeAndWait(void* handle)
{
    CJThreadResumeAndWait(handle);
}

void MRT_ThreadReady(void* handle)
{
    CJThreadReady(handle);
}

void MRT_ThreadWait()
{
    CJThreadWait();
}
#ifdef __APPLE__
#include "MacAlias.h"
#else
#include "CommonAlias.h"
#endif
#ifdef __cplusplus
};
#endif
} // namespace MapleRuntime
