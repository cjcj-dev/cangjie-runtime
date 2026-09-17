#ifndef MRT_REFERENCE_PROCESSOR_H
#define MRT_REFERENCE_PROCESSOR_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Common/TypeDef.h"
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
    bool uses_clear_all_soft_reference_policy;
    ZPerWorker<Counters> encountered_count;
    ZPerWorker<Counters> discovered_count;
    ZPerWorker<Counters> enqueued_count;
    ZPerWorker<Node*> discovered_list;
    ZContended<Node*> pending_list;
    Node* pending_list_tail;
    IsStronglyLive isStronglyLiveFn;
    ObserveWeakFinal observeWeakFinalFn;
};

} // namespace MapleRuntime
#endif
