#include "Heap/z/zAccess.hpp"
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
#include "Heap/shared/collectedHeap.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MObject.h"
#include "CjScheduler.h"
#include "ObjectModel/MReference.h"
#include "Common/Handle.h"
#include "Heap/z/zRootsIterator.hpp"



namespace MapleRuntime {


static const ZStatSubPhase ZSubPhaseConcurrentReferencesProcess("Concurrent References Process",
                                                                ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentReferencesEnqueue("Concurrent References Enqueue",
                                                                ZGenerationId::old);



uint32_t ReferenceProcessor::worker_index()
{
    const uint32_t id = WorkerThread::worker_id();
    CHECK(id != UINT32_MAX);
    CHECK(id < ZPerWorkerStorage::count());
    return id;
}

static volatile zpointer* reference_referent_addr(BaseObject* reference)
{
    return reinterpret_cast<volatile zpointer*>(MReference::referent_addr(reference));
}
static zpointer reference_referent(BaseObject* reference) { return ZBarrier::load_atomic(reference_referent_addr(reference)); }
static BaseObject* reference_discovered(BaseObject* reference) { return MReference::discovered(reference); }
static void reference_set_discovered(BaseObject* reference, BaseObject* value) { MReference::set_discovered(reference, value); }
static BaseObject* reference_next(BaseObject* reference) { return MReference::next(reference); }
static void reference_set_next(BaseObject* reference, BaseObject* value) { MReference::set_next(reference, value); }

void ReferenceProcessor::list_append(BaseObject*& head, BaseObject*& tail, BaseObject* reference)
{
    if (head == nullptr) { head = reference; }
    else { reference_set_discovered(tail, reference); }
    tail = reference;
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

ReferenceProcessor::~ReferenceProcessor() = default;

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
    if (type == ReferenceType::FINAL) { return reference_next(reference) != nullptr; }
    return referent == nullptr;
}

bool ReferenceProcessor::is_strongly_live(BaseObject* referent) const
{
    // Cangjie stack objects are outside the moving heap and cannot be cleared.
    if (!Heap::IsHeapAddress(referent)) { return true; }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(referent));
    return region->IsYoungRegion() || region->is_object_strongly_live(from_object(referent));
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
    BaseObject* referent = to_object(ZBarrier::load_barrier_on_oop_field(reference_referent_addr(reference)));
    if (is_inactive(reference, referent, type)) { return false; }
    if (Heap::page(reinterpret_cast<MAddress>(reference))->IsYoungRegion()) { return false; }
    if (is_strongly_live(referent)) { return false; }
    if (is_softly_live(reference, type)) { return false; }
    return true;
}

bool ReferenceProcessor::try_make_inactive(BaseObject* reference, ReferenceType type) const
{
    const zpointer referent = reference_referent(reference);
    if (is_null_any(referent)) { return false; }
    auto* field = reference_referent_addr(reference);
    if (type == ReferenceType::WEAK) {
        return ZBarrier::clean_barrier_on_weak_oop_field(field);
    }
    if (type == ReferenceType::FINAL) {
        if (ZBarrier::clean_barrier_on_final_oop_field(field)) {
            DCHECK(reference_next(reference) == nullptr);
            reference_set_next(reference, reference);
            return true;
        }
    } else {
        LOG(RTLOG_FATAL, "unsupported Reference type");
    }
    return false;
}

void ReferenceProcessor::discover(BaseObject* reference, ReferenceType type)
{
    discovered_count.get(worker_index())[TypeIndex(type)]++;
    if (type == ReferenceType::FINAL) {
        ZBarrier::MarkBarrierOnOldOopField(*MReference::referent_addr(reference), true);
    }
    DCHECK(!Heap::page(reinterpret_cast<MAddress>(reference))->IsYoungRegion());
    DCHECK(reference_discovered(reference) == nullptr);
    BaseObject** list = discovered_list.addr(worker_index());
    reference_set_discovered(reference, *list);
    *list = reference;
}

bool ReferenceProcessor::discover_reference(BaseObject* reference, ReferenceType type)
{
    encountered_count.get(worker_index())[TypeIndex(type)]++;
    // SOFT/PHANTOM remain unsupported under #399; no surrogate policy.
    if (type != ReferenceType::WEAK && type != ReferenceType::FINAL) { return false; }
    if (!should_discover(reference, type)) {
        return false;
    }
    discover(reference, type);
    return true;
}

void ReferenceProcessor::process_worker_discovered_list(BaseObject* list)
{
    BaseObject* keep_head = nullptr;
    BaseObject* keep_tail = nullptr;
    for (BaseObject* reference = list; reference != nullptr;) {
        const ReferenceType type = MReference::reference_type(reference->GetTypeInfo());
        BaseObject* next = reference_discovered(reference);
        reference_set_discovered(reference, nullptr);
        if (try_make_inactive(reference, type)) {
            enqueued_count.get(worker_index())[TypeIndex(type)]++;
            list_append(keep_head, keep_tail, reference);
        }
        reference = next;
        SuspendibleThreadSet::yield();
    }
    if (keep_head != nullptr) {
        BaseObject* old = __atomic_exchange_n(pending_list.addr(), keep_head, __ATOMIC_ACQ_REL);
        reference_set_discovered(keep_tail, old);
        if (old == nullptr) { pending_list_tail = keep_tail; }
    }
}

void ReferenceProcessor::work()
{
    SuspendibleThreadSetJoiner stsJoiner;
    ZPerWorkerIterator<BaseObject*> iter(&discovered_list);
    for (BaseObject** start; iter.next(&start);) {
        BaseObject* list = __atomic_exchange_n(start, nullptr, __ATOMIC_ACQ_REL);
        if (list != nullptr) {
            process_worker_discovered_list(list);
        }
    }
}

void ReferenceProcessor::verify_empty() const
{
#ifdef ASSERT
    ZPerWorkerIterator<BaseObject*> iter(const_cast<ZPerWorker<BaseObject*>*>(&discovered_list));
    for (BaseObject** list; iter.next(&list);) {
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

void ReferenceProcessor::verify_pending_references()
{
#ifdef ASSERT
    SuspendibleThreadSetJoiner stsJoiner;
    for (BaseObject* current = pending_list.get(); current != nullptr; current = reference_discovered(current)) {
        BaseObject* referent = to_object(ZBarrier::load_barrier_on_oop_field(reference_referent_addr(current)));
        const ReferenceType type = MReference::reference_type(current->GetTypeInfo());
        DCHECK(is_inactive(current, referent, type));
        if (type == ReferenceType::FINAL) {
            DCHECK(ZPointer::is_marked_any_old(reference_referent(current)));
        }
        SuspendibleThreadSet::yield();
    }
#endif
}

void ReferenceProcessor::enqueue_references()
{
    ZStatTimerOld timer(ZSubPhaseConcurrentReferencesEnqueue);
    if (pending_list.get() == nullptr) { return; }
    verify_pending_references();
    auto& owner = Heap::GetHeap().GetFinalizerProcessor();
    {
        std::lock_guard<std::mutex> lock(owner.PendingLock());
        SuspendibleThreadSetJoiner stsJoiner;
        BaseObject* previous = owner.SwapPendingList(pending_list.get());
        reference_set_discovered(pending_list_tail, previous);
        owner.NotifyPending();
    }
    pending_list.set(nullptr);
    pending_list_tail = nullptr;
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
    ZPerWorkerIterator<BaseObject*> iter(const_cast<ZPerWorker<BaseObject*>*>(&discovered_list));
    for (BaseObject** list; iter.next(&list);) {
        if (*list != nullptr) {
            return false;
        }
    }
    return pending_list.get() == nullptr;
}

// The native threads provide Cangjie's managed-entry and shutdown boundary.
// All per-reference state, queues and finalizer registration live in std.core.
extern "C" MRT_EXPORT void* MRT_ProcessFinalizers(void* argument)
{
    static_cast<FinalizerProcessor*>(argument)->Run();
    return nullptr;
}
static void* ProcessReferenceHandler(void* argument)
{
    static_cast<FinalizerProcessor*>(argument)->RunReferenceHandler();
    return nullptr;
}

FinalizerProcessor::FinalizerProcessor(ZWorkers* workers)
    : referencePending(strongStorage.Allocate()), referenceProcessor(workers) {}

FinalizerProcessor::~FinalizerProcessor() { strongStorage.Release(referencePending); }

void FinalizerProcessor::Start()
{
    Heap::GetHeap().page_allocator().StartUncommitters();
    running.store(true, std::memory_order_release);
    pthread_attr_t attr;
    size_t stackSize = CangjieRuntime::GetConcurrencyParam().thStackSize * KB;
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    stackSize = std::max(stackSize, static_cast<size_t>(PTHREAD_STACK_MIN));
#endif
    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "init reference thread attr");
    CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_JOINABLE), "joinable reference threads");
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stackSize), "reference thread stack");
    CHECK_PTHREAD_CALL(pthread_create, (&referenceThread, &attr, ProcessReferenceHandler, this), "reference handler");
    CHECK_PTHREAD_CALL(pthread_create, (&finalizerThread, &attr, MRT_ProcessFinalizers, this), "finalizer thread");
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "destroy reference thread attr");
    WaitStarted();
}

void FinalizerProcessor::Stop()
{
    CHECK_DETAIL(running.load(std::memory_order_acquire), "invalid finalizerProcessor status");
    Heap::GetHeap().page_allocator().StopUncommitters();
    running.store(false, std::memory_order_release);
    Notify();
    WaitStop();
}

void FinalizerProcessor::NotifyStarted()
{
    std::lock_guard<std::mutex> lock(startedLock);
    ++started;
    startedCondition.notify_all();
}

void FinalizerProcessor::WaitStarted()
{
    std::unique_lock<std::mutex> lock(startedLock);
    startedCondition.wait(lock, [this] { return started == 2; });
}

void FinalizerProcessor::WaitStop()
{
    CHECK_PTHREAD_CALL(pthread_join, (referenceThread, nullptr), "join reference handler");
    CHECK_PTHREAD_CALL(pthread_join, (finalizerThread, nullptr), "join finalizer");
    referenceThread = 0;
    finalizerThread = 0;
    started = 0;
}

void FinalizerProcessor::Notify()
{
    std::lock_guard<std::mutex> lock(pendingLock);
    pendingCondition.notify_all();
}

void FinalizerProcessor::SetReferenceMethods(void* registration, void* handler, void* finalizer)
{
    std::lock_guard<std::mutex> lock(pendingLock);
    registerMethod = registration;
    handlerMethod = handler;
    finalizerMethod = finalizer;
    methodsReady.store(true, std::memory_order_release);
    pendingCondition.notify_all();
}

void FinalizerProcessor::InvokeManaged(void* entry, BaseObject* argument)
{
    Mutator* mutator = Mutator::GetMutator();
    const bool wasManaged = mutator->IsManagedContext();
    mutator->SetManagedContext(true);
    const uintptr_t data = MRT_GetThreadLocalData();
    uintptr_t unit = 0;
#if defined(__aarch64__)
    // The managed Unit result uses the AArch64 indirect-result register x8.
    ExecuteCangjieStub(argument, 0, 0, entry, reinterpret_cast<void*>(data), &unit);
#else
    // Unit is an sret parameter before the explicit managed arguments.
    ExecuteCangjieStub(&unit, argument, 0, entry, reinterpret_cast<void*>(data), 0);
#endif
    mutator->SetManagedContext(wasManaged);
}

void FinalizerProcessor::Run() { RunWorker(false); }
void FinalizerProcessor::RunReferenceHandler() { RunWorker(true); }

void FinalizerProcessor::RunWorker(bool handler)
{
    NotifyStarted();
    {
        std::unique_lock<std::mutex> lock(pendingLock);
        pendingCondition.wait(lock, [this] {
            return !IsRunning() || (firstPending && methodsReady.load(std::memory_order_acquire));
        });
    }
    if (!IsRunning()) { return; }
    (void)NewFinalizerCJThread();
    Mutator* mutator = Mutator::GetMutator();
    if (!handler) {
        MutatorManager::Instance().MutatorManagementRLock();
        fpMutator = mutator;
        tid = mutator->GetTid();
        MutatorManager::Instance().MutatorManagementRUnlock();
    }
    {
        ScopedObjectAccess access;
        InvokeManaged(handler ? handlerMethod : finalizerMethod, nullptr);
        CHECK_DETAIL(!ExceptionManager::HasPendingException(), "Reference service exited with an exception");
    }
    if (!handler) {
        MutatorManager::Instance().MutatorManagementRLock();
        fpMutator = nullptr;
        MutatorManager::Instance().MutatorManagementRUnlock();
    }
    EndFinalizerCJThread();
}

BaseObject* FinalizerProcessor::RegisterFinalizer(BaseObject* object)
{
    // instanceKlass.cpp:1919-1932: the argument remains an updatable root
    // throughout the managed allocation of its distinct FinalReference.
    BaseObject* result;
    {
        ScopedObjectAccess access;
        Mutator* mutator = Mutator::GetMutator();
        HandleMark mark(*mutator);
        Handle handle(mutator, object);
        // instanceKlass.cpp:1929 calls Universe::finalizer_register_method only after
        // java.base published it. An unpublished bridge must not invoke a null method;
        // the object stays unregistered and the caller observes that.
        if (!methodsReady.load(std::memory_order_acquire)) {
            return handle();
        }
        InvokeManaged(registerMethod, handle());
        result = handle();
    }
    if (ExceptionManager::HasPendingException()) {
        ExceptionManager::CheckAndThrowPendingException("Finalizer.register");
    }
    return result;
}

BaseObject* FinalizerProcessor::SwapPendingList(BaseObject* head)
{
    BaseObject* previous = NativeAccess<>::oop_load(referencePending);
    NativeAccess<>::oop_store(referencePending, head);
    return previous;
}

void FinalizerProcessor::NotifyPending()
{
    firstPending = true;
    pendingCondition.notify_all();
}

BaseObject* FinalizerProcessor::WaitPending()
{
    {
        ScopedEnterSaferegion safe(false);
        std::unique_lock<std::mutex> lock(pendingLock);
        while (IsRunning() && is_null_any(referencePending->GetFieldValue(std::memory_order_acquire))) {
            lock.unlock();
            if (MutatorManager::Instance().MarkFlushHandshakeActive()) {
                (void)MutatorManager::Instance().AcknowledgeMarkFlushForCurrentThread();
            }
            lock.lock();
            pendingCondition.wait_for(lock, std::chrono::milliseconds(1));
        }
    }
    ScopedObjectAccess access;
    std::lock_guard<std::mutex> lock(pendingLock);
    return SwapPendingList(nullptr);
}

void FinalizerProcessor::InvokeFinalize(BaseObject* object)
{
    {
        ScopedObjectAccess access;
        Mutator* mutator = Mutator::GetMutator();
        HandleMark mark(*mutator);
        Handle handle(mutator, object);
        TypeInfo* klass = handle()->GetTypeInfo();
        FuncRef method = klass->GetFinalizeMethod();
        CHECK_DETAIL(method != nullptr, "FinalReference referent has no finalize method");
        const bool wasManaged = mutator->IsManagedContext();
        mutator->SetManagedContext(true);
        ExecuteCangjieStub(handle(), klass, 0, reinterpret_cast<void*>(method),
                           reinterpret_cast<void*>(MRT_GetThreadLocalData()), 0);
        mutator->SetManagedContext(wasManaged);
    }
    if (ExceptionManager::HasPendingException()) {
        ExceptionManager::CheckAndThrowPendingException("Finalizer.invokeFinalize");
    }
}

extern "C" MRT_EXPORT void CJ_MCC_SetReferenceMethods(void* registration, void* handler, void* finalizer)
{
    Heap::GetHeap().GetFinalizerProcessor().SetReferenceMethods(registration, handler, finalizer);
}
extern "C" MRT_EXPORT BaseObject* CJ_MCC_ReferenceWaitPending()
{
    return Heap::GetHeap().GetFinalizerProcessor().WaitPending();
}
extern "C" MRT_EXPORT bool CJ_MCC_ReferenceRuntimeRunning()
{
    return Heap::GetHeap().GetFinalizerProcessor().IsRunning();
}
extern "C" MRT_EXPORT int64_t CJ_MCC_ReferenceNanoTime() { return TimeUtil::NanoSeconds(); }
extern "C" MRT_EXPORT void CJ_MCC_ReferenceInvokeFinalize(BaseObject* object)
{
    FinalizerProcessor::InvokeFinalize(object);
}
} // namespace MapleRuntime
