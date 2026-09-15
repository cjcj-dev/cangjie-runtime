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
#include "Common/OopStorage.h"
#include "Common/PageAllocator.h"
#include "Common/TypeDef.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zReferenceProcessor.hpp"

namespace MapleRuntime {

class FinalizerProcessor {
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct GenerationCycleRootTestAccess;
#endif
public:
    FinalizerProcessor();
    ~FinalizerProcessor() = default;

    // zRootsIterator: strong queued/running roots and weak registrations
    // share one physical enumeration, with distinct closures.
    OopStorage& StrongRootStorage() { return strongStorage; }
    OopStorage& WeakRootStorage() { return weakStorage; }
    U32 VisitFinalizers(const NativeSlotVisitor& visitor) { return VisitRootLists({}, visitor); }
    void VisitGCRoots(const NativeSlotVisitor& visitor) { VisitRootLists(visitor, {}); }
    void VisitNativePointers(const NativeSlotVisitor& visitor) { VisitRootLists(visitor, visitor); }

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

    NativeSlot* AllocateFinalizerHandle(BaseObject* obj);
    void RegisterFinalizer(BaseObject* obj);
    void RegisterFinalizers(NativeRootHandles& objs);
    bool IsRunning() const { return running.load(std::memory_order_acquire); }
    uint32_t GetTid() const { return tid; }
    ReferenceProcessor& GetReferenceProcessor() { return referenceProcessor; }
    void ProcessReferences(const ReferenceProcessor::IsStronglyLive& isStronglyLive);
    void EnqueueReferences();

#if defined(MRT_TESTABLE_INTERNALS)
    using BeforeFinalizableIdleCheck = std::function<void()>;
    void SetBeforeFinalizableIdleCheckForTest(BeforeFinalizableIdleCheck hook);
    void EnqueueFinalizableForTest(BaseObject* obj);
    void FinishFinalizableBatchForTest();
    bool HasFinalizableJobForTest();
#endif

    Mutator* GetMutator() const { return fpMutator; }

    void NotifyToReclaimGarbage()
    {
        shouldReclaimHeapGarbage.store(true, std::memory_order_release);
        Notify();
    }
    void NotifyToFeedAllocBuffers()
    {
        shouldFeedHungryBuffers.store(true, std::memory_order_release);
        Notify();
    }

private:
    U32 VisitRootLists(const NativeSlotVisitor& strong, const NativeSlotVisitor& weak)
    {
        if (strong) { strongStorage.OopsDo(strong); }
        U32 count = weak ? static_cast<U32>(weakStorage.OopsDo(weak)) : 0;
        return count;
    }

    void InitFinalizerCJThread();
    void NotifyStarted();
    void Wait();
    void Wait(U32 timeoutMilliSeconds);
    bool EnqueueFinalizableReference(BaseObject* obj);
    bool HasFinalizableJob();
    void FinishFinalizableBatch();
    void ProcessFinalizables();
    void ProcessFinalizableList();
    void ReclaimHeapGarbage();
    void FeedHungryBuffers();

    std::mutex wakeLock;
    std::condition_variable wakeCondition; // notify finalizer processing continue

    std::mutex startedLock;
    std::condition_variable startedCondition; // notify finalizerProcessor thread is started
    volatile bool started;

    std::atomic<bool> running{ false };
    U32 iterationWaitTime;

    // finalization
    OopStorage strongStorage;
    OopStorage weakStorage;
    std::mutex listLock;                 // lock for finalizers & finalizables & workingFinalizables
    NativeRootHandles finalizers; // created finalizer record, accessed by mutator & GC

    // a dead finalizer is moved into finalizable by GC, then run finalize method by FP thread
    NativeRootHandles finalizables;

    NativeRootHandles workingFinalizables; // FP working list, swap from finalizables
    ReferenceProcessor referenceProcessor;

    // Protected by listLock.  Queue non-emptiness and the cached predicate are
    // one synchronization decision, so a worker cannot clear a later enqueue.
    bool hasFinalizableJob = false;
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
