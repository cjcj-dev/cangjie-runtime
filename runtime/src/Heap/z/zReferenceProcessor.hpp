#ifndef MRT_REFERENCE_PROCESSOR_H
#define MRT_REFERENCE_PROCESSOR_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Common/TypeDef.h"
#include <climits>
#include <pthread.h>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include "Base/Panic.h"
#include "Common/OopStorage.h"
#include "Common/PageAllocator.h"
#include "Heap/z/zValue.hpp"
#include "Heap/z/zValue.inline.hpp"

namespace MapleRuntime {

enum class ReferenceType : uint8_t {
    SOFT = 0,
    WEAK,
    FINAL,
    PHANTOM,
    COUNT,
};

class ZWorkers;

class ReferenceProcessor {
    friend class ZReferenceProcessorTask;

public:
    static constexpr size_t REFERENCE_TYPE_COUNT = static_cast<size_t>(ReferenceType::COUNT);
    using IsStronglyLive = std::function<bool(BaseObject*)>;
    using EnqueueFinal = std::function<bool(BaseObject*)>;
    using ObserveWeakFinal = std::function<void(BaseObject*, BaseObject*)>;

    explicit ReferenceProcessor(ZWorkers* workers = nullptr);
    ~ReferenceProcessor();
    ReferenceProcessor(const ReferenceProcessor&) = delete;
    ReferenceProcessor& operator=(const ReferenceProcessor&) = delete;

    void set_workers(ZWorkers* workers);
    void set_soft_reference_policy(bool clear_all_soft_references);
    bool uses_clear_all_soft_reference_policy() const;

    void reset_statistics();
    bool DiscoverReference(BaseObject* reference, ReferenceType type);
    void process_references();
    void ProcessReferences(const IsStronglyLive& isStronglyLive);
    void EnqueueReferences(const EnqueueFinal& enqueueFinal);
#if defined(MRT_TESTABLE_INTERNALS)
    void ProcessReferences(const IsStronglyLive& isStronglyLive, const ObserveWeakFinal& observeWeakFinal);
    static void SetBeforeWeakCleanCasForTest(std::function<void()> hook);
#endif
    void verify_pending_references();

    size_t Encountered(ReferenceType type) const;
    size_t Discovered(ReferenceType type) const;
    size_t Enqueued(ReferenceType type) const;
    bool Empty() const;

private:
    struct Node {
        BaseObject* reference;
        ReferenceType type;
        Node* next;
    };
    using Counters = size_t[REFERENCE_TYPE_COUNT];

    static constexpr size_t TypeIndex(ReferenceType type) { return static_cast<size_t>(type); }
    static uint32_t worker_index();
    static void list_append(Node*& head, Node*& tail, Node* reference);
    static void DeleteList(Node* list);
    static bool is_object_finalizable(BaseObject* reference);

    bool is_inactive(BaseObject* reference, BaseObject* referent, ReferenceType type) const;
    bool is_strongly_live(BaseObject* referent) const;
    bool is_softly_live(BaseObject* reference, ReferenceType type) const;
    bool should_discover(BaseObject* reference, ReferenceType type) const;
    bool try_make_inactive(BaseObject* reference, ReferenceType type) const;
    void discover(BaseObject* reference, ReferenceType type);
    void verify_empty() const;
    void process_worker_discovered_list(Node* discovered_list);
    void work();
    void collect_statistics();
    void soft_reference_update_clock();
    bool CleanWeakReference(BaseObject* reference);

    ZWorkers* workers;
    bool clear_all_soft_references;
    ZPerWorker<Counters> encountered_count;
    ZPerWorker<Counters> discovered_count;
    ZPerWorker<Counters> enqueued_count;
    ZPerWorker<Node*> discovered_list;
    ZContended<Node*> pending_list;
    Node* pending_list_tail;
    IsStronglyLive isStronglyLiveFn;
    ObserveWeakFinal observeWeakFinalFn;
};

class Mutator;
class FinalizerProcessor {
public:
    explicit FinalizerProcessor(ZWorkers* workers = nullptr);
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
#endif
