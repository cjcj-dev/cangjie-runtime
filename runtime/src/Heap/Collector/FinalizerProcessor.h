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

    void RegisterFinalizer(BaseObject* obj);
    void RegisterFinalizers(ManagedList<NativeSlot>& objs);
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
        std::lock_guard<std::mutex> lock(listLock);
        if (strong) {
            for (NativeSlot& root : finalizables) { strong(root); }
            for (NativeSlot& root : workingFinalizables) { strong(root); }
        }
        U32 count = 0;
        if (weak) {
            for (NativeSlot& root : finalizers) { weak(root); ++count; }
        }
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
    std::mutex listLock;                 // lock for finalizers & finalizables & workingFinalizables
    ManagedList<NativeSlot> finalizers; // created finalizer record, accessed by mutator & GC

    // a dead finalizer is moved into finalizable by GC, then run finalize method by FP thread
    ManagedList<NativeSlot> finalizables;

    ManagedList<NativeSlot> workingFinalizables; // FP working list, swap from finalizables
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
