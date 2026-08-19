// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_FINALIZER_PROCESSOR_H
#define MRT_FINALIZER_PROCESSOR_H

#include <climits>
#include <condition_variable>
#include <list>
#include <mutex>

#include "Base/Panic.h"
#include "Common/PageAllocator.h"
#include "Common/TypeDef.h"
#include "Heap/Collector/Collector.h"

namespace MapleRuntime {

class FinalizerProcessor {
public:
    FinalizerProcessor();
    ~FinalizerProcessor() = default;

    // mainly for resurrection.
    U32 VisitFinalizers(const RootVisitor& visitor)
    {
        U32 count = 0;
        std::lock_guard<std::mutex> l(listLock);
        for (RootSlot& obj : finalizers) {
            visitor(obj);
            ++count;
        }
        return count;
    }

    // for : *finalizers* are not proper gc roots.
    void VisitGCRoots(const RootVisitor& visitor)
    {
        std::lock_guard<std::mutex> l(listLock);
        for (RootSlot& obj : finalizables) {
            visitor(obj);
        }
        for (RootSlot& obj : workingFinalizables) {
            visitor(obj);
        }
    }

    // mainly for fixing old pointers
    void VisitRawPointers(const RootVisitor& visitor)
    {
        std::lock_guard<std::mutex> l(listLock);
        for (RootSlot& obj : finalizables) {
            visitor(obj);
        }
        for (RootSlot& obj : workingFinalizables) {
            visitor(obj);
        }
        for (RootSlot& obj : finalizers) {
            visitor(obj);
        }
    }

    // notify for finalizer processing loop, invoked after GC
    void Notify();
    // wait started flag set, call after create finalizerProcessor thread
    void WaitStarted();

    void Start();
    void Stop();
    void Run();
    void Init();
    void Fini();
    void WaitStop();

    void EnqueueFinalizables(const std::function<bool(BaseObject*)>& finalizable, U32 countLimit = UINT_MAX);
    void RegisterFinalizer(BaseObject* obj);
    void RegisterFinalizers(ManagedList<RootSlot>& objs);
    bool IsRunning() const { return running; }
    uint32_t GetTid() const { return tid; }

    Mutator* GetMutator() const { return fpMutator; }

    void NotifyToReclaimGarbage()
    {
        shouldReclaimHeapGarbage.store(true);
        Notify();
    }
    void NotifyToFeedAllocBuffers()
    {
        shouldFeedHungryBuffers.store(true, std::memory_order_release);
        Notify();
    }

private:
    void InitFinalizerCJThread();
    void NotifyStarted();
    void Wait(U32 timeoutMilliSeconds);
    void ProcessFinalizables();
    void ProcessFinalizableList();
    void ReclaimHeapGarbage();
    void UncommitIdleMemory();
    void FeedHungryBuffers();

    std::mutex wakeLock;
    std::condition_variable wakeCondition; // notify finalizer processing continue

    std::mutex startedLock;
    std::condition_variable startedCondition; // notify finalizerProcessor thread is started
    volatile bool started;

    volatile bool running; // Initially false and set true after finalizerProcessor thread start, set false when stop

    U32 iterationWaitTime;

    // finalization
    std::mutex listLock;                 // lock for finalizers & finalizables & workingFinalizables
    ManagedList<RootSlot> finalizers; // created finalizer record, accessed by mutator & GC

    // a dead finalizer is moved into finalizable by GC, then run finalize method by FP thread
    ManagedList<RootSlot> finalizables;

    ManagedList<RootSlot> workingFinalizables; // FP working list, swap from finalizables

    std::atomic<bool> hasFinalizableJob;
    std::atomic<bool> shouldReclaimHeapGarbage;
    std::atomic<bool> shouldFeedHungryBuffers;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // stats
    void LogAfterProcess();
#endif
    uint64_t timeProcessorBegin;
    uint64_t timeProcessUsed;
    uint64_t timeCurrentProcessBegin;
    uint32_t tid = 0;
    pthread_t threadHandle = 0; // thread handle to thread
    Mutator* fpMutator = nullptr;
    // Tracks whether the current finalizer OS thread has already been bound to a CJThread.
    bool finalizerCJThreadInitialized = false;
};
} // namespace MapleRuntime
#endif // MRT_FINALIZER_PROCESSOR_H
