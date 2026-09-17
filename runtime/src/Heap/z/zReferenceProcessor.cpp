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

namespace MapleRuntime {

static const ZStatSubPhase ZSubPhaseConcurrentReferencesProcess("Concurrent References Process",
                                                                ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentReferencesEnqueue("Concurrent References Enqueue",
                                                                ZGenerationId::old);

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::function<void()> g_beforeWeakCleanCasForTest;
}
#endif

namespace {
size_t g_statEncountered[4] = {};
size_t g_statDiscovered[4] = {};
size_t g_statEnqueued[4] = {};
}

void ZStatReferences::set_soft(size_t encountered, size_t discovered, size_t enqueued)
{
    g_statEncountered[0] = encountered;
    g_statDiscovered[0] = discovered;
    g_statEnqueued[0] = enqueued;
}
void ZStatReferences::set_weak(size_t encountered, size_t discovered, size_t enqueued)
{
    g_statEncountered[1] = encountered;
    g_statDiscovered[1] = discovered;
    g_statEnqueued[1] = enqueued;
}
void ZStatReferences::set_final(size_t encountered, size_t discovered, size_t enqueued)
{
    g_statEncountered[2] = encountered;
    g_statDiscovered[2] = discovered;
    g_statEnqueued[2] = enqueued;
}
void ZStatReferences::set_phantom(size_t encountered, size_t discovered, size_t enqueued)
{
    g_statEncountered[3] = encountered;
    g_statDiscovered[3] = discovered;
    g_statEnqueued[3] = enqueued;
}

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
    if (workers != nullptr) {
        workers->run(&task);
    } else {
        work();
    }
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

} // namespace MapleRuntime
