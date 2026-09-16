// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_SYNC_H
#define MRT_SYNC_H

#include <atomic>

#include "CjScheduler.h"
#include "Common/BaseObject.h"
#include "Common/OopStorage.h"
#include "Common/WeakHandle.h"
#include "sema.h"
#include "waitqueue.h"

namespace MapleRuntime {
struct NativeWaitSet {
    Waitqueue wq;
    WeakHandle object;
    std::atomic<int> busy{ 0 };
    NativeWaitSet* next{ nullptr };
};

struct NativeMutexWait {
    Sema sema;
    WeakHandle object;
    std::atomic<int> busy{ 0 };
    NativeMutexWait* next{ nullptr };
};

OopStorage& SyncWeakOopStorage();
void SyncRetireDead();

#ifdef __cplusplus
extern "C" {
#endif
struct CJFuture {
    void* klass;
#ifdef __arm__
    uint32_t padding;
    uint32_t data[4];
#else
    long long int data[4]; // 4: occupied by _thread(1)/result(1)/executeFn(2)
#endif
    std::atomic<bool> completeFlag;
    std::atomic<int> isWaitQueueInit;
    NativeWaitSet* waitNative;

    static constexpr size_t SYNC_OBJECT_SIZE = 168;
};

struct CJMutex {
    void* klass;
#ifdef __arm__
    uint32_t padding;
#endif
    std::atomic<int64_t> ownerThreadId;
    uint64_t ownCount;
    std::atomic<int64_t> state;
    bool isSemaInit;
    NativeMutexWait* waitNative;
};

struct CJMonitor {
    void* klass;
#ifdef __arm__
    uint32_t padding;
#endif
    HeapSlot<> mutexPtr;
    bool isWaitQueueInit;
    NativeWaitSet* waitNative;
};

struct CJWaitQueue {
    void* klass;
#ifdef __arm__
    uint32_t padding;
#endif
    bool isWaitQueueInit;
    NativeWaitSet* waitNative;
};

struct CJMultiConditionMonitor {
    void* klass;
#ifdef __arm__
    uint32_t padding;
#endif
    // Same managed heap ref as CJMonitor::mutexPtr (std.sync MultiConditionMonitor).
    HeapSlot<> mutexPtr;
};

void ReleaseNativeResource(BaseObject* obj);

void MCC_FutureInit(void* ptr);
bool MCC_FutureIsComplete(void* ptr);
void MRT_FutureWait(const void* ptr, int64_t timeout);
void MCC_FutureNotifyAll(const void* ptr);
int MCC_MutexInit(void* ptr);
void MCC_MutexLock(void* ptr);
void MCC_MutexLockSlowPath(void* ptr);
bool MCC_MutexTryLock(void* ptr);
bool MCC_MutexCheckStatus(const void* ptr);
void MCC_MutexUnlock(const void* ptr);
int MCC_WaitQueueForMonitorInit(void* ptr);
int MCC_WaitQueueInit(void* ptr);
bool MCC_MonitorWait(const void* ptr, int64_t timeout);
void MCC_MonitorNotify(const void* ptr);
void MCC_MonitorNotifyAll(const void* ptr);
bool MCC_MultiConditionMonitorWait(const void* ptr, void* waitQueuePtr, int64_t timeout);
void MCC_MultiConditionMonitorNotify(const void* ptr, const void* waitQueuePtr);
void MCC_MultiConditionMonitorNotifyAll(const void* ptr, const void* waitQueuePtr);
bool MCC_IsThreadObjectInited();
void* MRT_GetCurrentCJThreadObject();
void MCC_SetCurrentCJThreadObject(void* ptr);
void MRT_SetCJThreadName(void* handle, uint8_t* name, size_t len);
int64_t MRT_GetCJThreadId(void* handle);
int64_t MRT_GetCJThreadState(void* handle);
void* MRT_GetCurrentCJThread();
void MRT_ThreadWait();
void MRT_ThreadResumeAndWait(void* handle);
void MRT_ThreadReady(void* handle);
#ifdef __cplusplus
};
#endif
} // namespace MapleRuntime

#endif // MRT_SYNC_H
