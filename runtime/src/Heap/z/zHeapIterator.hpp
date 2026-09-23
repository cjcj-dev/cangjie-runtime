// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#ifndef MRT_HEAP_ITERATOR_H
#define MRT_HEAP_ITERATOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "Common/BaseObject.h"
#include "Heap/z/zIterator.hpp"
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
    using FieldVisitor = std::function<void(BaseObject*, RefField<>&)>;
    using EdgeVisitor = std::function<void(BaseObject*, const void*, uintptr_t)>;
    struct ObjArrayTask {
        MArray* object;
        MIndex index;
    };
    explicit HeapIterator(bool visitWeaks, bool forVerify = false, unsigned nworkers = 1);
    void Iterate(const ObjectVisitor& objectVisitor, const EdgeVisitor& fieldVisitor = {});
    void object_iterate(const ObjectVisitor& objectVisitor, uint32_t worker_id);
    void object_and_field_iterate(const ObjectVisitor& objectVisitor, const EdgeVisitor& fieldVisitor,
                                  uint32_t worker_id);
    void push_strong_roots(const HeapIteratorContext& context);
    void push_weak_roots(const HeapIteratorContext& context);
    void drain(const HeapIteratorContext& context);
    void steal(const HeapIteratorContext& context);
    void drain_and_steal(const HeapIteratorContext& context);
    bool try_set_bit(BaseObject* object);

    std::vector<std::vector<BaseObject*>> workerQueues;
    std::vector<std::vector<ObjArrayTask>> workerArrayQueues;

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
    void Follow(BaseObject* object, const FieldVisitor& visitor);
    void FollowArray(MArray* object);
    void FollowArrayChunk(const ObjArrayTask& array, const FieldVisitor& visitor);
    const bool visitWeaks;
    const bool forVerify;
    const unsigned nworkers;
    std::mutex bitmapLock;
    std::unordered_map<uintptr_t, std::unique_ptr<HeapIteratorBitMap>> objectBitmaps;
};

class HeapIteratorContext {
public:
    HeapIteratorContext(HeapIterator& iter, const HeapIterator::ObjectVisitor* objectVisitor,
                        const HeapIterator::EdgeVisitor* fieldVisitor, uint32_t workerId)
        : iter(iter), objectVisitor(objectVisitor), fieldVisitor(fieldVisitor), workerId(workerId),
          queue(iter.workerQueues[workerId]), arrayQueue(iter.workerArrayQueues[workerId])
    {
    }
    uint32_t worker_id() const { return workerId; }
    void visit_object(BaseObject* object) const;
    void push(BaseObject* object) const;
    void push_array_chunk(const HeapIterator::ObjArrayTask& array) const;
    bool pop(BaseObject*& object) const;
    bool pop_array_chunk(HeapIterator::ObjArrayTask& array) const;
    bool is_drained() const { return queue.empty() && arrayQueue.empty(); }

    HeapIterator& iter;
    const HeapIterator::ObjectVisitor* objectVisitor;
    const HeapIterator::EdgeVisitor* fieldVisitor;
    uint32_t workerId;
    std::vector<BaseObject*>& queue;
    std::vector<HeapIterator::ObjArrayTask>& arrayQueue;
};

} // namespace MapleRuntime
#endif
