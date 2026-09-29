// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#ifndef MRT_HEAP_ITERATOR_H
#define MRT_HEAP_ITERATOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include "Heap/z/zGranuleMap.hpp"
#include <vector>
#include "Common/BaseObject.h"
#include "Heap/z/zIterator.hpp"
#include "Heap/z/zTaskTerminator.hpp"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class MArray;
class HeapIteratorContext;

class HeapIteratorBitMap {
    std::vector<std::atomic<uintptr_t>> words;
public:
    explicit HeapIteratorBitMap(size_t bits)
        : words((bits + (sizeof(uintptr_t) * 8 - 1)) / (sizeof(uintptr_t) * 8))
    {
        for (auto& word : words) {
            word.store(0, std::memory_order_relaxed);
        }
    }
    bool try_set_bit(size_t index);
};

class HeapIterator {
public:
    using ObjectVisitor = std::function<void(BaseObject*)>;
    using EdgeVisitor = std::function<void(BaseObject*, const void*, uintptr_t)>;
    struct ObjArrayTask {
        MArray* object;
        MIndex index;
    };
    explicit HeapIterator(bool visitWeaks, bool forVerify = false, unsigned nworkers = 1);
    ~HeapIterator();
    void Iterate(const ObjectVisitor& objectVisitor, const EdgeVisitor& fieldVisitor = {});
    void object_iterate(const ObjectVisitor& objectVisitor, uint32_t worker_id);
    void object_and_field_iterate(const ObjectVisitor& objectVisitor, const EdgeVisitor& fieldVisitor,
                                  uint32_t worker_id);
    void push_strong_roots(const HeapIteratorContext& context);
    void push_weak_roots(const HeapIteratorContext& context);
    template <bool VisitWeaks>
    void drain(const HeapIteratorContext& context);
    template <bool VisitWeaks>
    void steal(const HeapIteratorContext& context);
    bool steal(const HeapIteratorContext& context, BaseObject*& object);
    bool steal_array_chunk(const HeapIteratorContext& context, ObjArrayTask& array);
    template <bool VisitWeaks>
    void drain_and_steal(const HeapIteratorContext& context);
    bool try_set_bit(BaseObject* object);

    using ObjectQueue = OverflowTaskQueue<BaseObject*>;
    using ArrayQueue = OverflowTaskQueue<ObjArrayTask>;
    GenericTaskQueueSet<ObjectQueue> workerQueues;
    GenericTaskQueueSet<ArrayQueue> workerArrayQueues;

private:
    friend class HeapIteratorContext;
    template <bool Weak>
    class ColoredRootOopClosure {
        HeapIterator& iter;
        const HeapIteratorContext& context;
        BaseObject* load_oop(NativeSlot* root);
    public:
        ColoredRootOopClosure(HeapIterator& iter, const HeapIteratorContext& context) : iter(iter), context(context) {}
        void do_root(NativeSlot& root);
    };
    class UncoloredRootOopClosure {
        HeapIterator& iter;
        const HeapIteratorContext& context;
    public:
        UncoloredRootOopClosure(HeapIterator& iter, const HeapIteratorContext& context) : iter(iter), context(context)
        {
        }
        void do_root(ObjectRef& root);
    };

    void Push(BaseObject* object, const ObjectVisitor& objectVisitor);
    template <bool VisitReferents>
    class OopClosure : public OopIterateClosure {
        HeapIterator* const iter;
        const HeapIteratorContext& context;
        BaseObject* const base;
        BaseObject* load_oop(RefField<>* field);
    public:
        OopClosure(HeapIterator* iter, const HeapIteratorContext& context, BaseObject* base)
            : iter(iter), context(context), base(base) {}
        ReferenceIterationMode reference_iteration_mode() override
        {
            return VisitReferents ? DO_FIELDS : DO_FIELDS_EXCEPT_REFERENT;
        }
        void do_oop(RefField<>* field) override;
    };
    template <bool VisitReferents>
    void follow_object(const HeapIteratorContext& context, BaseObject* object);
    template <bool VisitWeaks>
    void follow(const HeapIteratorContext& context, BaseObject* object);
    template <bool VisitWeaks>
    void visit_and_follow(const HeapIteratorContext& context, BaseObject* object);
    template <bool VisitWeaks>
    void object_iterate_inner(const HeapIteratorContext& context);
    void follow_array(const HeapIteratorContext& context, MArray* object);
    void follow_array_chunk(const HeapIteratorContext& context, const ObjArrayTask& array);
    const bool visitWeaks;
    const bool forVerify;
    TaskTerminator terminator;
    std::mutex bitmapLock;
    HeapIteratorBitMap* object_bitmap(BaseObject* object);
    ZGranuleMap<HeapIteratorBitMap*> objectBitmaps;
};

class HeapIteratorContext {
public:
    HeapIteratorContext(HeapIterator& iter, const HeapIterator::ObjectVisitor* objectVisitor,
                        const HeapIterator::EdgeVisitor* fieldVisitor, uint32_t workerId)
        : iter(iter), objectVisitor(objectVisitor), fieldVisitor(fieldVisitor), workerId(workerId),
          queue(iter.workerQueues.queue(workerId)), arrayQueue(iter.workerArrayQueues.queue(workerId))
    {
    }
    uint32_t worker_id() const { return workerId; }
    void visit_object(BaseObject* object) const;
    void push(BaseObject* object) const;
    void push_array_chunk(const HeapIterator::ObjArrayTask& array) const;
    bool pop(BaseObject*& object) const;
    bool pop_array_chunk(HeapIterator::ObjArrayTask& array) const;
    bool is_drained() const { return queue->is_empty() && arrayQueue->is_empty(); }

    HeapIterator& iter;
    const HeapIterator::ObjectVisitor* objectVisitor;
    const HeapIterator::EdgeVisitor* fieldVisitor;
    uint32_t workerId;
    HeapIterator::ObjectQueue* queue;
    HeapIterator::ArrayQueue* arrayQueue;
};

} // namespace MapleRuntime
#endif
