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
#include "Heap/z/zReferenceDiscoverer.hpp"
#include "Heap/z/zValue.inline.hpp"

namespace MapleRuntime {

class ZWorkers;

class ReferenceProcessor : public ReferenceDiscoverer {
    friend class ZReferenceProcessorTask;

public:
    static constexpr size_t REFERENCE_TYPE_COUNT = static_cast<size_t>(ReferenceType::COUNT);

    explicit ReferenceProcessor(ZWorkers* workers = nullptr);
    ~ReferenceProcessor();
    ReferenceProcessor(const ReferenceProcessor&) = delete;
    ReferenceProcessor& operator=(const ReferenceProcessor&) = delete;

    void set_workers(ZWorkers* workers);
    void set_soft_reference_policy(bool clear_all_soft_references);
    bool uses_clear_all_soft_reference_policy() const;

    void reset_statistics();
    bool discover_reference(BaseObject* reference, ReferenceType type) override;
    void process_references();
    void enqueue_references();
    void verify_pending_references();

    size_t Encountered(ReferenceType type) const;
    size_t Discovered(ReferenceType type) const;
    size_t Enqueued(ReferenceType type) const;
    bool Empty() const;

private:
    using Counters = size_t[REFERENCE_TYPE_COUNT];

    static constexpr size_t TypeIndex(ReferenceType type) { return static_cast<size_t>(type); }
    static uint32_t worker_index();
    static void list_append(BaseObject*& head, BaseObject*& tail, BaseObject* reference);

    bool is_inactive(BaseObject* reference, BaseObject* referent, ReferenceType type) const;
    bool is_strongly_live(BaseObject* referent) const;
    bool is_softly_live(BaseObject* reference, ReferenceType type) const;
    bool should_discover(BaseObject* reference, ReferenceType type) const;
    bool try_make_inactive(BaseObject* reference, ReferenceType type) const;
    void discover(BaseObject* reference, ReferenceType type);
    void verify_empty() const;
    void process_worker_discovered_list(BaseObject* discovered_list);
    void work();
    void collect_statistics();
    void soft_reference_update_clock();

    ZWorkers* workers;
    bool clear_all_soft_references;
    ZPerWorker<Counters> encountered_count;
    ZPerWorker<Counters> discovered_count;
    ZPerWorker<Counters> enqueued_count;
    ZPerWorker<BaseObject*> discovered_list;
    ZContended<BaseObject*> pending_list;
    BaseObject* pending_list_tail;
};

class Mutator;
class FinalizerProcessor {
public:
    explicit FinalizerProcessor(ZWorkers* workers = nullptr);
    ~FinalizerProcessor();
    OopStorage& StrongRootStorage() { return strongStorage; }
    void VisitGCRoots(const NativeSlotVisitor& visitor) { strongStorage.OopsDo(visitor); }
    void Start();
    void Stop();
    void Notify();
    void WaitStarted();
    void WaitStop();
    void Run();
    void RunReferenceHandler();
    BaseObject* RegisterFinalizer(BaseObject* object);
    bool IsRunning() const { return running.load(std::memory_order_acquire); }
    uint32_t GetTid() const { return tid; }
    Mutator* GetMutator() const { return fpMutator; }
    ReferenceProcessor& GetReferenceProcessor() { return referenceProcessor; }
    void ProcessReferences() { referenceProcessor.process_references(); }
    void EnqueueReferences() { referenceProcessor.enqueue_references(); }
    void SetReferenceMethods(void* registerMethod, void* handlerMethod, void* finalizerMethod);
    BaseObject* SwapPendingList(BaseObject* head);
    BaseObject* WaitPending();
    static void InvokeFinalize(BaseObject* object);
    std::mutex& PendingLock() { return pendingLock; }
    void NotifyPending();
private:
    void RunWorker(bool handler);
    void NotifyStarted();
    static void InvokeManaged(void* entry, BaseObject* argument);
    OopStorage strongStorage;
    NativeSlot* referencePending;
    ReferenceProcessor referenceProcessor;
    std::atomic<bool> running{false};
    std::atomic<bool> methodsReady{false};
    void* registerMethod = nullptr;
    void* handlerMethod = nullptr;
    void* finalizerMethod = nullptr;
    std::mutex pendingLock;
    std::condition_variable pendingCondition;
    bool firstPending = false;
    std::mutex startedLock;
    std::condition_variable startedCondition;
    unsigned started = 0;
    pthread_t finalizerThread = 0;
    pthread_t referenceThread = 0;
    Mutator* fpMutator = nullptr;
    uint32_t tid = 0;
};
} // namespace MapleRuntime
#endif
