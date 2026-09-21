// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_THREAD_LOCAL_H
#define MRT_THREAD_LOCAL_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "Base/RwLock.h"
#include "Interpreter/Options.h"
#include "Interpreter/RTInterface.h"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkStack.hpp"

#include "Heap/z/zThreadLocalData.hpp"
namespace MapleRuntime {
class AllocBuffer;
class Mutator;
class ZMark;

enum class ThreadType { CJ_PROCESSOR = 0, GC_THREAD, FP_THREAD, HOT_UPDATE_THREAD, UNCOMMITTER_THREAD };

// Backend and CJThread will use external tls var through offset calculation, so external tls
// must in the first place, followed by the internal tls.
struct ThreadLocalData {
    // External thread local var.
    AllocBuffer* buffer;
    Mutator* mutator;
    uint8_t* cjthread;
    uint8_t* schedule;
    uint8_t* preemptFlag;
    uint8_t* protectAddr;
    uint64_t safepointState;
    uint64_t tid;
    void* foreignCJThread;
#ifdef INTERPRETER_ENABLED
    // Duplicate of Mutator::interpreterCJThreadData for fast access.
    // Should be updated together with mutator field.
    DYN_CJThreadSpecificData interpreterCJThreadData;
#endif
    // Internal thread local var.
    ThreadType threadType;
    bool isCJProcessor;
    void* threadCache;
#ifndef INTERPRETER_ENABLED
    void* gcDataOffsetPadding;
#endif
    // Fixed ABI offset: borrowed current logical owner, followed by the
    // native owner retained across managed bindings on this OS thread.
    ThreadGCData* gcData;
    ThreadGCData* nativeGCData;

public:
    void SetMutator(Mutator* newMutator);
};

static_assert(offsetof(ThreadLocalData, buffer) == 0, "compiler TLS buffer ABI");

void MarkFlushOnEnterSaferegion();
void MarkFlushBeginLeaveSaferegion();
void MarkFlushEndLeaveSaferegion();
bool MarkFlushPendingForCurrentThread();
void RegisterCurrentMarkFlushThread();

struct CleanThreadLocalData {
    CleanThreadLocalData() noexcept;
    ~CleanThreadLocalData();
    ThreadGCData nativeData;
    AllocBuffer* nativeBuffer = nullptr;
};

class ThreadLocal { // merge this to ThreadLocalData.
public:
    static ThreadLocalData* GetThreadLocalData();
    static void InitializeCleaner();
    static AllocBuffer*& NativeAllocBuffer();
    static ThreadGCData& GetGCData();
    static void FlushCurrentThreadMarkStacks();
    static MarkThreadLocalStacks& GetMarkStacks(ZMark& domain);
    static bool FlushMarkStacks(ThreadLocalData* tls, ZMark& domain);

    static void SetMutator(Mutator* newMutator) { GetThreadLocalData()->SetMutator(newMutator); }

    static Mutator* GetMutator() { return GetThreadLocalData()->mutator; }

    static AllocBuffer* GetAllocBuffer() { return GetThreadLocalData()->buffer; }

    static void SetAllocBuffer(AllocBuffer* buffer) { GetThreadLocalData()->buffer = buffer; }

    static uint8_t* GetPreemptFlag() { return GetThreadLocalData()->preemptFlag; }

    static void SetProtectAddr(uint8_t* addr) { GetThreadLocalData()->protectAddr = addr; }

    static void SetThreadType(ThreadType type) { InitializeCleaner(); GetThreadLocalData()->threadType = type; }

    static ThreadType GetThreadType() { return GetThreadLocalData()->threadType; }

    static void SetCJProcessorFlag(bool flag) { GetThreadLocalData()->isCJProcessor = flag; }

    static bool IsCJProcessor() { return GetThreadLocalData()->isCJProcessor; }

    static void SetForeignCJThread(void* cjthread)
    {
        GetThreadLocalData()->foreignCJThread = cjthread;
    }

    static void* GetForeignCJThread()
    {
        return GetThreadLocalData()->foreignCJThread;
    }

    static void SetCJThread(void* cjthread)
    {
        GetThreadLocalData()->cjthread = reinterpret_cast<uint8_t*>(cjthread);
    }

    static void* GetSchedule()
    {
        return GetThreadLocalData()->schedule;
    }

    static void SetSchedule(void* schedule)
    {
        GetThreadLocalData()->schedule = reinterpret_cast<uint8_t*>(schedule);
    }

    static void* GetThreadCache()
    {
        return GetThreadLocalData()->threadCache;
    }

    static void* SetThreadCache(void* threadCache)
    {
        return GetThreadLocalData()->threadCache = threadCache;
    }

    // When runtime is stop, we need to lock any operation which may access runtime.
    static void ThreadLocalFini()
    {
        tlEnableLock.LockWrite();
    }

    static bool TryGetRdLock()
    {
        return tlEnableLock.TryLockRead();
    }

    static void UnlockRdLock()
    {
        tlEnableLock.UnlockRead();
    }

private:
    static RwLock tlEnableLock;
};
} // namespace MapleRuntime

#include "Handshake.h"
#endif // MRT_THREAD_LOCAL_H
