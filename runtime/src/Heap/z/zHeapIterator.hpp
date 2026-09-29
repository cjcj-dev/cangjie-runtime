// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#ifndef MRT_HEAP_ITERATOR_H
#define MRT_HEAP_ITERATOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include "Base/BitMap.h"
#include "Heap/z/zGranuleMap.hpp"
#include <vector>
#include "Common/BaseObject.h"
#include "Heap/z/zIterator.hpp"
#include "Heap/z/zTaskTerminator.hpp"
#include "Heap/z/zRootsIterator.hpp"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class MArray;
class HeapIteratorContext;

class HeapIteratorBitMap {
    CHeapBitMap bitmap;
public:
    explicit HeapIteratorBitMap(size_t bits) : bitmap(bits) {}
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
    template<bool VisitWeaks>
    void push_roots(const HeapIteratorContext& context);
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
    bool mark_object(BaseObject* object);
    void mark_visit_and_push(const HeapIteratorContext& context, BaseObject* object);
    bool should_visit_object_at_mark() const;
    bool should_visit_object_at_follow() const;

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
    RootsIteratorStrongColored rootsColored;
    RootsIteratorStrongUncolored rootsUncolored;
    RootsIteratorWeakColored rootsWeakColored;
    TaskTerminator terminator;
    ZLock bitmapLock;
    HeapIteratorBitMap* object_bitmap(BaseObject* object);
    ZGranuleMap<HeapIteratorBitMap*> objectBitmaps;
};

class HeapIteratorContext {
public:
    HeapIteratorContext(const HeapIterator::ObjectVisitor* objectVisitor,
                        const HeapIterator::EdgeVisitor* fieldVisitor, uint32_t workerId,
                        HeapIterator::ObjectQueue* queue, HeapIterator::ArrayQueue* arrayQueue)
        : objectVisitor(objectVisitor), fieldVisitor(fieldVisitor), workerId(workerId),
          queue(queue), arrayQueue(arrayQueue)
    {
    }
    uint32_t worker_id() const { return workerId; }
    void visit_object(BaseObject* object) const;
    void push(BaseObject* object) const;
    void push_array_chunk(const HeapIterator::ObjArrayTask& array) const;
    bool pop(BaseObject*& object) const;
    bool pop_array_chunk(HeapIterator::ObjArrayTask& array) const;
    bool is_drained() const { return queue->is_empty() && arrayQueue->is_empty(); }

    const HeapIterator::ObjectVisitor* objectVisitor;
    const HeapIterator::EdgeVisitor* fieldVisitor;
    uint32_t workerId;
    HeapIterator::ObjectQueue* queue;
    HeapIterator::ArrayQueue* arrayQueue;
};

} // namespace MapleRuntime
#endif
