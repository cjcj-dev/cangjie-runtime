#include "Heap/z/zReferenceProcessor.hpp"

#include <new>

#include "Base/Panic.h"
#include "Common/SuspendibleThreadSet.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Base/TimeUtils.h"
#include "Heap/z/zGenerationId.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/workerThread.hpp"
#include "ObjectModel/RefField.inline.h"
#include <algorithm>
#include <chrono>
#include "Base/Macros.h"
#include "Heap/z/zUncommitter.hpp"
#include "Common/ScopedObjectAccess.h"
#include "ExceptionManager.inline.h"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/z/zBarrier.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MObject.h"
#include "CjScheduler.h"



namespace MapleRuntime {

static const ZStatCriticalPhase PFinalizer("Finalizer");
static const ZStatCriticalPhase PFinalizerProcessorWaittingTime("finalizerProcessor waitting time");

static const ZStatSubPhase ZSubPhaseConcurrentReferencesProcess("Concurrent References Process",
                                                                ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentReferencesEnqueue("Concurrent References Enqueue",
                                                                ZGenerationId::old);

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::function<void()> g_beforeWeakCleanCasForTest;
}
#endif


uint32_t ReferenceProcessor::worker_index()
{
    const uint32_t id = WorkerThread::worker_id();
    CHECK(id != UINT32_MAX);
    CHECK(id < ZPerWorkerStorage::count());
    return id;
}

void ReferenceProcessor::list_append(Node*& head, Node*& tail, Node* reference)
{
    if (head == nullptr) {
        head = reference;
    } else {
        tail->next = reference;
    }
    tail = reference;
}

void ReferenceProcessor::DeleteList(Node* list)
{
    while (list != nullptr) {
        Node* next = list->next;
        delete list;
        list = next;
    }
}

ReferenceProcessor::ReferenceProcessor(ZWorkers* workers)
    : workers(workers),
      clear_all_soft_references(true),
      encountered_count(),
      discovered_count(),
      enqueued_count(),
      discovered_list(nullptr),
      pending_list(nullptr),
      pending_list_tail(nullptr)
{
    reset_statistics();
}

ReferenceProcessor::~ReferenceProcessor()
{
    ZPerWorkerIterator<Node*> iter(&discovered_list);
    for (Node** start; iter.next(&start);) {
        DeleteList(*start);
        *start = nullptr;
    }
    DeleteList(pending_list.get());
    pending_list.set(nullptr);
}

void ReferenceProcessor::set_workers(ZWorkers* value) { workers = value; }

void ReferenceProcessor::set_soft_reference_policy(bool clear_all)
{
    clear_all_soft_references = clear_all;
}

bool ReferenceProcessor::uses_clear_all_soft_reference_policy() const
{
    return clear_all_soft_references;
}

bool ReferenceProcessor::is_inactive(BaseObject* reference, BaseObject* referent, ReferenceType type) const
{
    (void)reference;
    if (type == ReferenceType::FINAL) {
        return false;
    }
    return referent == nullptr;
}

bool ReferenceProcessor::is_strongly_live(BaseObject* referent) const
{
    if (referent == nullptr || !Heap::IsHeapAddress(referent)) {
        return false;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(referent));
    if (region == nullptr) {
        return false;
    }
    if (region->IsYoungRegion()) {
        return true;
    }
    return region->is_object_strongly_live(from_object(referent));
}

bool ReferenceProcessor::is_softly_live(BaseObject* reference, ReferenceType type) const
{
    (void)reference;
    if (type != ReferenceType::SOFT) {
        return false;
    }
    return !clear_all_soft_references;
}

bool ReferenceProcessor::should_discover(BaseObject* reference, ReferenceType type) const
{
    if (reference == nullptr) {
        return false;
    }
    if (type != ReferenceType::WEAK && type != ReferenceType::FINAL) {
        return false;
    }
    if (type == ReferenceType::FINAL) {
        Node* head = discovered_list.get(worker_index());
        for (Node* existing = head; existing != nullptr; existing = existing->next) {
            if (existing->type == type && existing->reference == reference) {
                return false;
            }
        }
    }
    return true;
}

bool ReferenceProcessor::is_object_finalizable(BaseObject* reference)
{
    if (reference == nullptr || !Heap::IsHeapAddress(reference)) {
        return false;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(reference));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return false;
    }
    const zaddress addr = from_object(reference);
    return region->is_object_live(addr) && !region->is_object_strongly_live(addr);
}

bool ReferenceProcessor::CleanWeakReference(BaseObject* reference)
{
    HeapSlot<>& referentField = HeapSlotAt<>(reinterpret_cast<uintptr_t>(reference) + TYPEINFO_PTR_SIZE);
    const zpointer observed = referentField.GetFieldValue(std::memory_order_acquire);
    BaseObject* referent = to_object(RefField<>(observed).GetTargetObject());
    if (referent == nullptr) {
        return false;
    }
    if (Heap::IsHeapAddress(referent)) {
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(referent));
        if (region != nullptr && !region->IsFreeRegion() && !region->IsGarbageRegion()) {
            if (region->is_object_strongly_live(from_object(referent))) {
                return false;
            }
        }
    }
#if defined(MRT_TESTABLE_INTERNALS)
    if (g_beforeWeakCleanCasForTest) {
        g_beforeWeakCleanCasForTest();
    }
#endif
    if (referentField.CompareExchange(observed, to_zpointer(0))) {
        return true;
    }
    return false;
}

bool ReferenceProcessor::try_make_inactive(BaseObject* reference, ReferenceType type) const
{
    if (type == ReferenceType::WEAK) {
        HeapSlot<>& referentField = HeapSlotAt<>(reinterpret_cast<uintptr_t>(reference) + TYPEINFO_PTR_SIZE);
        BaseObject* target = to_object(referentField.GetTargetObject(std::memory_order_acquire));
        if (isStronglyLiveFn && target != nullptr && isStronglyLiveFn(target)) {
            return false;
        }
        if (!isStronglyLiveFn && is_strongly_live(target)) {
            return false;
        }
        const bool cleared = const_cast<ReferenceProcessor*>(this)->CleanWeakReference(reference);
#if defined(MRT_TESTABLE_INTERNALS)
        if (observeWeakFinalFn) {
            BaseObject* terminal = to_object(referentField.GetTargetObject(std::memory_order_acquire));
            observeWeakFinalFn(reference, terminal);
        }
#endif
        return cleared;
    }
    if (type == ReferenceType::FINAL) {
        return is_object_finalizable(reference);
    }
    return false;
}

void ReferenceProcessor::discover(BaseObject* reference, ReferenceType type)
{
    Node* node = new (std::nothrow) Node{ reference, type, nullptr };
    CHECK(node != nullptr);
    Node* old = discovered_list.get(worker_index());
    do {
        node->next = old;
    } while (!__atomic_compare_exchange_n(discovered_list.addr(worker_index()), &old, node, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    discovered_count.get(worker_index())[TypeIndex(type)]++;
}

bool ReferenceProcessor::DiscoverReference(BaseObject* reference, ReferenceType type)
{
    encountered_count.get(worker_index())[TypeIndex(type)]++;
    if (!should_discover(reference, type)) {
        return false;
    }
    discover(reference, type);
    return true;
}

void ReferenceProcessor::process_worker_discovered_list(Node* list)
{
    Node* keep_head = nullptr;
    Node* keep_tail = nullptr;
    for (Node* node = list; node != nullptr;) {
        Node* next = node->next;
        node->next = nullptr;
        if (try_make_inactive(node->reference, node->type)) {
            enqueued_count.get(worker_index())[TypeIndex(node->type)]++;
            list_append(keep_head, keep_tail, node);
        } else {
            delete node;
        }
        node = next;
        SuspendibleThreadSet::yield();
    }
    if (keep_head != nullptr) {
        Node* old_pending = __atomic_exchange_n(pending_list.addr(), keep_head, __ATOMIC_ACQ_REL);
        keep_tail->next = old_pending;
        if (old_pending == nullptr) {
            pending_list_tail = keep_tail;
        }
    }
}

void ReferenceProcessor::work()
{
    SuspendibleThreadSetJoiner stsJoiner;
    ZPerWorkerIterator<Node*> iter(&discovered_list);
    for (Node** start; iter.next(&start);) {
        Node* list = __atomic_exchange_n(start, nullptr, __ATOMIC_ACQ_REL);
        if (list != nullptr) {
            process_worker_discovered_list(list);
        }
    }
}

void ReferenceProcessor::verify_empty() const
{
#ifdef ASSERT
    ZPerWorkerIterator<Node*> iter(const_cast<ZPerWorker<Node*>*>(&discovered_list));
    for (Node** list; iter.next(&list);) {
        CHECK(*list == nullptr);
    }
    CHECK(pending_list.get() == nullptr);
#endif
}

void ReferenceProcessor::reset_statistics()
{
    verify_empty();
    ZPerWorkerIterator<Counters> iter_encountered(&encountered_count);
    for (Counters* counters; iter_encountered.next(&counters);) {
        for (size_t i = 0; i < REFERENCE_TYPE_COUNT; ++i) {
            (*counters)[i] = 0;
        }
    }
    ZPerWorkerIterator<Counters> iter_discovered(&discovered_count);
    for (Counters* counters; iter_discovered.next(&counters);) {
        for (size_t i = 0; i < REFERENCE_TYPE_COUNT; ++i) {
            (*counters)[i] = 0;
        }
    }
    ZPerWorkerIterator<Counters> iter_enqueued(&enqueued_count);
    for (Counters* counters; iter_enqueued.next(&counters);) {
        for (size_t i = 0; i < REFERENCE_TYPE_COUNT; ++i) {
            (*counters)[i] = 0;
        }
    }
}

void ReferenceProcessor::collect_statistics()
{
    Counters encountered = {};
    Counters discovered = {};
    Counters enqueued = {};
    ZPerWorkerConstIterator<Counters> iter_encountered(&encountered_count);
    for (const Counters* counters; iter_encountered.next(&counters);) {
        for (size_t i = 0; i < REFERENCE_TYPE_COUNT; ++i) {
            encountered[i] += (*counters)[i];
        }
    }
    ZPerWorkerConstIterator<Counters> iter_discovered(&discovered_count);
    for (const Counters* counters; iter_discovered.next(&counters);) {
        for (size_t i = 0; i < REFERENCE_TYPE_COUNT; ++i) {
            discovered[i] += (*counters)[i];
        }
    }
    ZPerWorkerConstIterator<Counters> iter_enqueued(&enqueued_count);
    for (const Counters* counters; iter_enqueued.next(&counters);) {
        for (size_t i = 0; i < REFERENCE_TYPE_COUNT; ++i) {
            enqueued[i] += (*counters)[i];
        }
    }
    ZStatReferences::set_soft(encountered[0], discovered[0], enqueued[0]);
    ZStatReferences::set_weak(encountered[1], discovered[1], enqueued[1]);
    ZStatReferences::set_final(encountered[2], discovered[2], enqueued[2]);
    ZStatReferences::set_phantom(encountered[3], discovered[3], enqueued[3]);
}

void ReferenceProcessor::soft_reference_update_clock() {}

class ZReferenceProcessorTask : public ZTask {
private:
    ReferenceProcessor* const reference_processor;
public:
    explicit ZReferenceProcessorTask(ReferenceProcessor* processor)
        : ZTask("ZReferenceProcessorTask"), reference_processor(processor) {}
    void work() override { reference_processor->work(); }
};

void ReferenceProcessor::process_references()
{
    ZStatTimerOld timer(ZSubPhaseConcurrentReferencesProcess);
    ZReferenceProcessorTask task(this);
        workers->run(&task);
    soft_reference_update_clock();
    collect_statistics();
}

void ReferenceProcessor::ProcessReferences(const IsStronglyLive& isStronglyLive)
{
    isStronglyLiveFn = isStronglyLive;
    observeWeakFinalFn = {};
    process_references();
    isStronglyLiveFn = {};
}

#if defined(MRT_TESTABLE_INTERNALS)
void ReferenceProcessor::ProcessReferences(const IsStronglyLive& isStronglyLive, const ObserveWeakFinal& observe)
{
    isStronglyLiveFn = isStronglyLive;
    observeWeakFinalFn = observe;
    process_references();
    isStronglyLiveFn = {};
    observeWeakFinalFn = {};
}

void ReferenceProcessor::SetBeforeWeakCleanCasForTest(std::function<void()> hook)
{
    g_beforeWeakCleanCasForTest = std::move(hook);
}
#endif

void ReferenceProcessor::verify_pending_references()
{
#ifdef ASSERT
    SuspendibleThreadSetJoiner stsJoiner;
    for (Node* current = pending_list.get(); current != nullptr; current = current->next) {
        SuspendibleThreadSet::yield();
    }
#endif
}

void ReferenceProcessor::EnqueueReferences(const EnqueueFinal& enqueueFinal)
{
    ZStatTimerOld timer(ZSubPhaseConcurrentReferencesEnqueue);
    verify_pending_references();
    Node* list = __atomic_exchange_n(pending_list.addr(), nullptr, __ATOMIC_ACQ_REL);
    pending_list_tail = nullptr;
    while (list != nullptr) {
        Node* node = list;
        list = list->next;
        bool accepted = false;
        if (node->type == ReferenceType::WEAK) {
            accepted = true;
        } else if (node->type == ReferenceType::FINAL) {
            accepted = enqueueFinal(node->reference);
        }
        (void)accepted;
        delete node;
    }
}

size_t ReferenceProcessor::Encountered(ReferenceType type) const
{
    size_t sum = 0;
    ZPerWorkerConstIterator<Counters> iter(&encountered_count);
    for (const Counters* counters; iter.next(&counters);) {
        sum += (*counters)[TypeIndex(type)];
    }
    return sum;
}

size_t ReferenceProcessor::Discovered(ReferenceType type) const
{
    size_t sum = 0;
    ZPerWorkerConstIterator<Counters> iter(&discovered_count);
    for (const Counters* counters; iter.next(&counters);) {
        sum += (*counters)[TypeIndex(type)];
    }
    return sum;
}

size_t ReferenceProcessor::Enqueued(ReferenceType type) const
{
    size_t sum = 0;
    ZPerWorkerConstIterator<Counters> iter(&enqueued_count);
    for (const Counters* counters; iter.next(&counters);) {
        sum += (*counters)[TypeIndex(type)];
    }
    return sum;
}

bool ReferenceProcessor::Empty() const
{
    ZPerWorkerIterator<Node*> iter(const_cast<ZPerWorker<Node*>*>(&discovered_list));
    for (Node** list; iter.next(&list);) {
        if (*list != nullptr) {
            return false;
        }
    }
    return pending_list.get() == nullptr;
}

constexpr U32 DEFAULT_FINALIZER_TIMEOUT_MS = 2000;
#if defined(MRT_TESTABLE_INTERNALS)
namespace {
FinalizerProcessor::BeforeFinalizableIdleCheck g_beforeFinalizableIdleCheckForTest;
}
#endif

static BaseObject* LoadFinalizerGood(NativeSlot& slot)
{
    // FinalizerProcessor is part of the mutator set. Route this retained root through the public
    // runtime load exit so resolution, root healing and the fail-closed postcondition stay one path.
    return ZBarrier::ReadStaticRef(slot);
}

// Note: can only be called by FinalizerProcessor thread
extern "C" MRT_EXPORT void* MRT_ProcessFinalizers(void* arg)
{
#ifdef __APPLE__
    CHECK_PTHREAD_CALL(pthread_setname_np, ("gc-helper"), "finalizer-processor thread setname");
#elif defined(__linux__) || defined(hongmeng)
    CHECK_PTHREAD_CALL(prctl, (PR_SET_NAME, "gc-helper"), "finalizer-processor thread setname");
#endif
    reinterpret_cast<FinalizerProcessor*>(arg)->Run();
    return nullptr;
}

void FinalizerProcessor::Start()
{
    Heap::GetHeap().GetAllocator().GetUncommitter().Start();
    pthread_t thread;
    pthread_attr_t attr;
    size_t stackSize = CangjieRuntime::GetConcurrencyParam().thStackSize * KB; // default 1MB stacksize
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    // PTHREAD_STACK_MIN is not supported in Windows.
    const size_t minStackSize = static_cast<size_t>(PTHREAD_STACK_MIN);
    if (stackSize < minStackSize) {
        stackSize = minStackSize;
    }
#endif
    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "init pthread attr");
    CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_JOINABLE), "set pthread joinable");
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stackSize), "set pthread stacksize");
    CHECK_PTHREAD_CALL(pthread_create, (&thread, &attr, MRT_ProcessFinalizers, this),
                       "create finalizer-process thread");
#ifdef __WIN64
    CHECK_PTHREAD_CALL(pthread_setname_np, (thread, "gc-helper"), "finalizer-processor thread setname");
#endif
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "destroy pthread attr");
    threadHandle = thread;

    WaitStarted();
}

// Stop FinalizerProcessor is only invoked at Fork or Runtime finliazaiton
// Should only invoke once.
void FinalizerProcessor::Stop()
{
    CHECK_DETAIL(running.load(std::memory_order_acquire), "invalid finalizerProcessor status");
    Heap::GetHeap().GetAllocator().GetUncommitter().Stop();
    running.store(false, std::memory_order_release);
    Notify();
    WaitStop();
}

FinalizerProcessor::FinalizerProcessor()
{
    started = false;
    running.store(false, std::memory_order_relaxed);
    iterationWaitTime = DEFAULT_FINALIZER_TIMEOUT_MS;
    timeProcessorBegin = 0;
    timeProcessUsed = 0;
    timeCurrentProcessBegin = 0;
    shouldReclaimHeapGarbage.store(false, std::memory_order_relaxed);
    shouldFeedHungryBuffers.store(false, std::memory_order_relaxed);
}

void FinalizerProcessor::Run()
{
    Init();
    NotifyStarted();
    while (running.load(std::memory_order_acquire)) {
        bool hasPendingFinalizableJob = false;
        bool hasPendingReclaimHeapGarbage = false;
        bool hasPendingFeedHungryBuffers = false;
        {
            ZStatTimer zstatTimer(PFinalizerProcessorWaittingTime);
            while (running.load(std::memory_order_acquire)) {
                hasPendingFinalizableJob = HasFinalizableJob();
                hasPendingReclaimHeapGarbage =
                    shouldReclaimHeapGarbage.exchange(false, std::memory_order_acq_rel);
                hasPendingFeedHungryBuffers =
                    shouldFeedHungryBuffers.exchange(false, std::memory_order_acq_rel);
                if (hasPendingFinalizableJob || hasPendingReclaimHeapGarbage || hasPendingFeedHungryBuffers) {
                    break;
                }
                Wait(iterationWaitTime);
            }
        }

        if (!running.load(std::memory_order_acquire)) {
            break;
        }

        if (UNLIKELY(!finalizerCJThreadInitialized)) {
            // Delay finalizer CJThread creation until the worker really has something to do,
            // but make all finalizer-side job types share the same one-time initialization.
            InitFinalizerCJThread();
        }

        if (hasPendingFinalizableJob) {
            ProcessFinalizables();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
            LogAfterProcess();
#endif
        }

        if (hasPendingFeedHungryBuffers) {
            FeedHungryBuffers();
        }

        if (hasPendingReclaimHeapGarbage) {
            ReclaimHeapGarbage();
        }

    }
    Fini();
}

void FinalizerProcessor::Init()
{
    // Only start the finalizer worker thread here. Its CJThread identity is created on demand
    // so it does not consume the earliest CJThread id before any finalize work exists.
    MutatorManager::Instance().MutatorManagementRLock();
    fpMutator = nullptr;
    MutatorManager::Instance().MutatorManagementRUnlock();
    running.store(true, std::memory_order_release);
    timeProcessorBegin = TimeUtil::MicroSeconds();
    timeProcessUsed = 0;
    LOG(RTLOG_INFO, "FinalizerProcessor thread started");
}

void FinalizerProcessor::Fini()
{
    MutatorManager::Instance().MutatorManagementRLock();
    fpMutator = nullptr;
    MutatorManager::Instance().MutatorManagementRUnlock();
    // Finalizer may exit without ever running a finalize task, so only tear down the CJThread
    // context if it was really materialized.
    if (finalizerCJThreadInitialized) {
        EndFinalizerCJThread();
        finalizerCJThreadInitialized = false;
    }
    LOG(RTLOG_INFO, "FinalizerProcessor thread stopped");
}

void FinalizerProcessor::WaitStop()
{
    pthread_t thread = threadHandle;
    int tmpResult = ::pthread_join(thread, nullptr);
    CHECK_DETAIL(tmpResult == 0, "::pthread_join() in FinalizerProcessor::WaitStop() return %d rather than 0. ",
                 tmpResult);
    started = false;
    threadHandle = 0;
}

void FinalizerProcessor::Notify()
{
    std::lock_guard<std::mutex> lock(wakeLock);
    wakeCondition.notify_one();
}

void FinalizerProcessor::Wait()
{
    std::unique_lock<std::mutex> lock(wakeLock);
    while (running.load(std::memory_order_acquire) &&
           !HasFinalizableJob() &&
           !shouldReclaimHeapGarbage.load(std::memory_order_acquire) &&
           !shouldFeedHungryBuffers.load(std::memory_order_acquire)) {
        lock.unlock();
        if (MutatorManager::Instance().MarkFlushHandshakeActive()) {
            (void)MutatorManager::Instance().AcknowledgeMarkFlushForCurrentThread();
        }
        lock.lock();
        wakeCondition.wait_for(lock, std::chrono::milliseconds(1), [this] {
            return !running.load(std::memory_order_acquire) ||
                HasFinalizableJob() ||
                shouldReclaimHeapGarbage.load(std::memory_order_acquire) ||
                shouldFeedHungryBuffers.load(std::memory_order_acquire);
        });
    }
}

void FinalizerProcessor::Wait(U32 timeoutMilliSeconds)
{
    std::unique_lock<std::mutex> lock(wakeLock);
    lock.unlock();
    if (MutatorManager::Instance().MarkFlushHandshakeActive()) {
        (void)MutatorManager::Instance().AcknowledgeMarkFlushForCurrentThread();
    }
    lock.lock();
    std::chrono::milliseconds epoch(timeoutMilliSeconds);
    wakeCondition.wait_for(lock, epoch);
}

void FinalizerProcessor::NotifyStarted()
{
    {
        std::unique_lock<std::mutex> lock(startedLock);
        CHECK_DETAIL(started != true, "unpexcted true, FinalizerProcessor might not wait stopped");
        started = true;
    }
    startedCondition.notify_all();
}

void FinalizerProcessor::WaitStarted()
{
    std::unique_lock<std::mutex> lock(startedLock);
    if (started) {
        return;
    }
    startedCondition.wait(lock, [this] { return started; });
}

bool FinalizerProcessor::EnqueueFinalizableReference(BaseObject* candidate)
{
    std::lock_guard<std::mutex> l(listLock);
    auto it = finalizers.begin();
    while (it != finalizers.end()) {
        BaseObject* obj = LoadFinalizerGood(*it);
        if (obj == nullptr || HeapFiller::IsFiller(obj)) {
            weakStorage.Release(&*it);
            it = finalizers.erase(it);
            continue;
        }
        if (obj != candidate) {
            ++it;
            continue;
        }
        NativeSlot* strong = strongStorage.Allocate();
        strong->StoreColoured(it->GetFieldValue(), std::memory_order_relaxed);
        finalizables.push_back(strong);
        weakStorage.Release(&*it);
        finalizers.erase(it);
        hasFinalizableJob = true;
        VLOG(REPORT, "enqueued finalizer %p", candidate);
        return true;
    }
    return false;
}

void FinalizerProcessor::ProcessReferences(const ReferenceProcessor::IsStronglyLive& isStronglyLive)
{
    referenceProcessor.ProcessReferences(isStronglyLive);
}

void FinalizerProcessor::EnqueueReferences()
{
    bool enqueued = false;
    referenceProcessor.EnqueueReferences(
        [this, &enqueued](BaseObject* obj) {
            const bool accepted = EnqueueFinalizableReference(obj);
            enqueued = accepted || enqueued;
            return accepted;
        });
    if (enqueued) {
        Notify();
    }
}

bool FinalizerProcessor::HasFinalizableJob()
{
    std::lock_guard<std::mutex> l(listLock);
    CHECK_DETAIL(hasFinalizableJob == !finalizables.empty(),
                 "finalizable job predicate must match queue state");
    return hasFinalizableJob;
}

void FinalizerProcessor::FinishFinalizableBatch()
{
#if defined(MRT_TESTABLE_INTERNALS)
    if (g_beforeFinalizableIdleCheckForTest) {
        g_beforeFinalizableIdleCheckForTest();
    }
#endif
    std::lock_guard<std::mutex> l(listLock);
    hasFinalizableJob = !finalizables.empty();
}

// Process finalizable list
// 1. always process list head
// 2. Leave safe region (calling in finalizerProcessor thread)
// 3. Invoke finalize method
// 4. remove processed finalizables
void FinalizerProcessor::ProcessFinalizableList()
{
    auto itor = workingFinalizables.begin();
    while (itor != workingFinalizables.end() && running.load(std::memory_order_acquire)) {
        // keep GC thread from visiting roots when workingFinalizables list is updating
        ScopedObjectAccess soa;
        CHECK_DETAIL(ExceptionManager::GetPendingException() == nullptr, "should not exist pending exception");
        BaseObject* finalizeObjAddr = LoadFinalizerGood(*itor);
        if (finalizeObjAddr == nullptr || HeapFiller::IsFiller(finalizeObjAddr)) {
            std::lock_guard<std::mutex> l(listLock);
            strongStorage.Release(&*itor);
            itor = workingFinalizables.erase(itor);
            continue;
        }

        TypeInfo* classInfo = reinterpret_cast<MObject*>(finalizeObjAddr)->GetTypeInfo();
        FuncRef finalizerMethod = classInfo->GetFinalizeMethod();
        Mutator* mutator = ThreadLocal::GetMutator();

        CHECK_DETAIL(finalizerMethod != nullptr, "%p has no finalize method", finalizeObjAddr);
        void (*finalizer)(BaseObject*, TypeInfo*) = reinterpret_cast<void (*)(BaseObject*, TypeInfo*)>(finalizerMethod);
        // finalize method return void, (moving) gc may take place here
        mutator->SetManagedContext(true);
        DLOG(FINALIZE, "tid %u finalize object %p", tid, finalizeObjAddr);
        uintptr_t threadData = MapleRuntime::MRT_GetThreadLocalData();
        ExecuteCangjieStub(finalizeObjAddr, finalizeObjAddr->GetTypeInfo(), 0, reinterpret_cast<void*>(finalizer),
                           reinterpret_cast<void*>(threadData), 0);
        mutator->SetManagedContext(false);

        if (ExceptionManager::HasFatalException()) {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
            ExceptionManager::DumpException();
#endif
            LOG(RTLOG_FATAL, "FatalException happened in finalizer");
        }
        ExceptionManager::ClearPendingException();
        {
            std::lock_guard<std::mutex> l(listLock);
            strongStorage.Release(&*itor);
            itor = workingFinalizables.erase(itor);
        }
    }
}

void FinalizerProcessor::ProcessFinalizables()
{
    ZStatTimer zstatTimer(PFinalizer);
    {
        // we leave saferegion to avoid GC visit those changing queues.
        ScopedObjectAccess soa;
        std::lock_guard<std::mutex> l(listLock);
        // FP will not come here before cleaning up workingFinalizables
        // workingFinalizables is expected empty, thus we could use std::swap here
        workingFinalizables.swap(finalizables);
    }
    DLOG(FINALIZE, "finalizer: working size %zu", workingFinalizables.size());
    ProcessFinalizableList();
    FinishFinalizableBatch();
}

#if defined(MRT_TESTABLE_INTERNALS)
void FinalizerProcessor::SetBeforeFinalizableIdleCheckForTest(BeforeFinalizableIdleCheck hook)
{
    g_beforeFinalizableIdleCheckForTest = std::move(hook);
}

void FinalizerProcessor::EnqueueFinalizableForTest(BaseObject* obj)
{
    NativeSlot root(zpointer::null);
    ZBarrier::WriteStaticRef(root, obj);
    {
        std::lock_guard<std::mutex> l(listLock);
        NativeSlot* slot = strongStorage.Allocate();
        slot->StoreColoured(root.GetFieldValue(), std::memory_order_relaxed);
        finalizables.push_back(slot);
        hasFinalizableJob = true;
    }
    Notify();
}

void FinalizerProcessor::FinishFinalizableBatchForTest()
{
    FinishFinalizableBatch();
}

bool FinalizerProcessor::HasFinalizableJobForTest()
{
    return HasFinalizableJob();
}
#endif

void FinalizerProcessor::InitFinalizerCJThread()
{
    // Bind the existing finalizer OS thread to a CJThread/scheduler so operations like
    // CJThreadPark can work, but delay this until there is real finalizer-side work to do.
    void* cjthread = NewFinalizerCJThread();
    CHECK_DETAIL(cjthread != nullptr, "create finalizer cjthread failed");
    Mutator* mutator = ThreadLocal::GetMutator();
    CHECK_DETAIL(mutator != nullptr, "create finalizer mutator failed");
    // NewFinalizerCJThread prepares the thread for managed execution. We immediately re-enter
    // saferegion here because the finalizer main loop itself stays idle most of the time and
    // only leaves saferegion again through ScopedObjectAccess when work is processed.
    (void)mutator->EnterSaferegion(true);
    tid = mutator->GetTid();

    MutatorManager::Instance().MutatorManagementRLock();
    fpMutator = mutator;
    MutatorManager::Instance().MutatorManagementRUnlock();
    finalizerCJThreadInitialized = true;
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void FinalizerProcessor::LogAfterProcess()
{
    if (!ENABLE_LOG(FINALIZE)) {
        return;
    }
    uint64_t timeNow = TimeUtil::MicroSeconds();
    uint64_t timeConsumed = timeNow - timeCurrentProcessBegin;
    uint64_t totalTimePassed = timeNow - timeProcessorBegin;
    timeProcessUsed += timeConsumed;
    constexpr float percentageDivend = 100.0f;
    float percentage = (static_cast<float>(TIME_FACTOR * timeProcessUsed) / totalTimePassed) / percentageDivend;
    DLOG(FINALIZE, "[FinalizerProcessor] End (%luus [%luus] [%.2f%%])", timeConsumed, timeProcessUsed, percentage);
}
#endif

NativeSlot* FinalizerProcessor::AllocateFinalizerHandle(BaseObject* obj)
{
    std::lock_guard<std::mutex> l(listLock);
    NativeSlot* slot = weakStorage.Allocate();
    ZBarrier::WriteStaticRef(*slot, obj);
    return slot;
}

void FinalizerProcessor::RegisterFinalizer(BaseObject* obj)
{
    std::lock_guard<std::mutex> l(listLock);
    NativeSlot* slot = weakStorage.Allocate();
    ZBarrier::WriteStaticRef(*slot, obj);
    finalizers.push_back(slot);
}

void FinalizerProcessor::RegisterFinalizers(NativeRootHandles& objs)
{
    if (objs.empty()) {
        return;
    }
    std::lock_guard<std::mutex> l(listLock);
    // Transfer native slots, not uncolored values. Re-storing after mark-start
    // would manufacture current mark-good metadata for an unmarked referent.
    // ZGC's OopStorage keeps the slot originally published by NativeAccess.
    finalizers.splice(finalizers.end(), objs);
}

void FinalizerProcessor::ReclaimHeapGarbage()
{
    Heap::GetHeap().GetAllocator().ReclaimGarbageMemory(false);
}

void FinalizerProcessor::FeedHungryBuffers()
{
    Heap::GetHeap().GetAllocator().FeedHungryBuffers();
}
} // namespace MapleRuntime

