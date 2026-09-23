// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#include "Heap/z/zAccess.hpp"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zResurrection.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include <algorithm>

namespace MapleRuntime {

bool HeapIteratorBitMap::try_set_bit(size_t index)
{
    const size_t bits = sizeof(uintptr_t) * 8;
    auto& word = words[index / bits];
    const uintptr_t mask = uintptr_t(1) << (index % bits);
    uintptr_t old = word.load(std::memory_order_relaxed);
    while ((old & mask) == 0) {
        if (word.compare_exchange_weak(old, old | mask, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

HeapIterator::HeapIterator(bool visitWeaks, bool forVerify, unsigned nworkers)
    : visitWeaks(visitWeaks), forVerify(forVerify), nworkers(nworkers == 0 ? 1 : nworkers)
{
    workerQueues.resize(this->nworkers);
    workerArrayQueues.resize(this->nworkers);
}

bool HeapIterator::try_set_bit(BaseObject* object)
{
    constexpr uintptr_t granule = 2 * 1024 * 1024;
    constexpr size_t objectAlign = 8;
    const uintptr_t address = reinterpret_cast<uintptr_t>(object);
    const uintptr_t key = address / granule;
    std::lock_guard<std::mutex> lock(bitmapLock);
    auto found = objectBitmaps.find(key);
    if (found == objectBitmaps.end()) {
        found = objectBitmaps.emplace(key, std::make_unique<HeapIteratorBitMap>(granule / objectAlign)).first;
    }
    return found->second->try_set_bit((address % granule) / objectAlign);
}

void HeapIteratorContext::visit_object(BaseObject* object) const
{
    if (objectVisitor != nullptr && *objectVisitor) {
        (*objectVisitor)(object);
    }
}

void HeapIteratorContext::push(BaseObject* object) const
{
    if (object != nullptr && iter.try_set_bit(object)) {
        if (iter.forVerify) {
            visit_object(object);
        }
        queue.push_back(object);
    }
}

void HeapIteratorContext::push_array_chunk(const HeapIterator::ObjArrayTask& array) const
{
    arrayQueue.push_back(array);
}

bool HeapIteratorContext::pop(BaseObject*& object) const
{
    if (queue.empty()) {
        return false;
    }
    object = queue.back();
    queue.pop_back();
    return true;
}

bool HeapIteratorContext::pop_array_chunk(HeapIterator::ObjArrayTask& array) const
{
    if (arrayQueue.empty()) {
        return false;
    }
    array = arrayQueue.back();
    arrayQueue.pop_back();
    return true;
}

void HeapIterator::Push(BaseObject* object, const ObjectVisitor& objectVisitor)
{
    HeapIteratorContext context(*this, &objectVisitor, nullptr, 0);
    context.push(object);
}

template <bool VisitReferents>
BaseObject* HeapIterator::OopClosure<VisitReferents>::load_oop(RefField<>* field)
{
    DCHECK(Heap::IsHeapAddress(field));
    if constexpr (VisitReferents) {
        return HeapAccess<AS_NO_KEEPALIVE | ON_UNKNOWN_OOP_REF>::oop_load_at(
            base, BaseObject::FieldOffset(base, field));
    }
    return HeapAccess<AS_NO_KEEPALIVE>::oop_load(field);
}

template <bool VisitReferents>
void HeapIterator::OopClosure<VisitReferents>::do_oop(RefField<>* field)
{
    if (context.fieldVisitor != nullptr && *context.fieldVisitor) {
        (*context.fieldVisitor)(base, field, raw(field->GetFieldValue()));
    }
    context.push(load_oop(field));
}

template <bool VisitReferents>
void HeapIterator::follow_object(const HeapIteratorContext& context, BaseObject* object)
{
    OopClosure<VisitReferents> closure(this, context, object);
    ZIterator::oop_iterate(object, &closure);
}

void HeapIterator::follow_array(const HeapIteratorContext& context, MArray* object)
{
    context.push_array_chunk({ object, 0 });
}

void HeapIterator::follow_array_chunk(const HeapIteratorContext& context, const ObjArrayTask& array)
{
    constexpr MIndex strideLimit = 2048;
    const MIndex length = array.object->GetLength();
    const MIndex start = array.index;
    const MIndex end = start + std::min(length - start, strideLimit);
    if (end < length) {
        context.push_array_chunk({ array.object, end });
    }
    OopClosure<false> closure(this, context, array.object);
    ZIterator::oop_iterate_elements_range(array.object, &closure, start, end);
}

template <bool VisitWeaks>
void HeapIterator::follow(const HeapIteratorContext& context, BaseObject* object)
{
    if (object->GetTypeInfo()->IsRawArray() && object->GetComponentTypeInfo()->IsRef()) {
        follow_array(context, static_cast<MArray*>(object));
    } else {
        follow_object<VisitWeaks>(context, object);
    }
}

template <bool VisitWeaks>
void HeapIterator::visit_and_follow(const HeapIteratorContext& context, BaseObject* object)
{
    DCHECK(object->IsValidObject());
    if (!forVerify) {
        context.visit_object(object);
    }
    follow<VisitWeaks>(context, object);
}

template <bool Weak>
BaseObject* HeapIterator::ColoredRootOopClosure<Weak>::load_oop(NativeSlot* root)
{
    if constexpr (Weak) {
        return NativeAccess<AS_NO_KEEPALIVE | ON_PHANTOM_OOP_REF>::oop_load(root);
    }
    return NativeAccess<AS_NO_KEEPALIVE>::oop_load(root);
}

template <bool Weak>
void HeapIterator::ColoredRootOopClosure<Weak>::do_root(NativeSlot& root)
{
    if (context.fieldVisitor != nullptr && *context.fieldVisitor) {
        (*context.fieldVisitor)(nullptr, &root, raw(root.GetFieldValue()));
    }
    context.push(load_oop(&root));
}

void HeapIterator::UncoloredRootOopClosure::do_root(ObjectRef& root)
{
    if (context.fieldVisitor != nullptr && *context.fieldVisitor) {
        (*context.fieldVisitor)(nullptr, &root, raw(root.LoadPlain()));
    }
    context.push(to_object(safe(root.LoadPlain())));
}

void HeapIterator::push_strong_roots(const HeapIteratorContext& context)
{
    ColoredRootOopClosure<false> colored(*this, context);
    RootsIteratorStrongColored().Apply([&](NativeSlot& root) { colored.do_root(root); });
    UncoloredRootOopClosure uncolored(*this, context);
    ZMark::VisitStrongPlainRoots([&](ObjectRef& root) { uncolored.do_root(root); }, [&](Mutator& mutator) {
        mutator.VisitMutatorRoots([&](ObjectRef& root) { mutator.VisitHeapRootSlots(root, [&](ObjectRef& slot) {
            uncolored.do_root(slot);
        }); }, [](ObjectRef&) {});
    });
}

void HeapIterator::push_weak_roots(const HeapIteratorContext& context)
{
    if (!visitWeaks) {
        return;
    }
    ColoredRootOopClosure<true> colored(*this, context);
    RootsIteratorWeakColored().Apply([&](NativeSlot& root) { colored.do_root(root); });
}

template <bool VisitWeaks>
void HeapIterator::drain(const HeapIteratorContext& context)
{
    BaseObject* object = nullptr;
    ObjArrayTask array { nullptr, 0 };
    do {
        while (context.pop(object)) {
            visit_and_follow<VisitWeaks>(context, object);
        }
        if (context.pop_array_chunk(array)) {
            follow_array_chunk(context, array);
        }
    } while (!context.is_drained());
}

void HeapIterator::steal(const HeapIteratorContext& context)
{
    const uint32_t self = context.worker_id();
    for (unsigned i = 0; i < nworkers; ++i) {
        const unsigned other = (self + 1 + i) % nworkers;
        if (other == self) {
            continue;
        }
        auto& q = workerQueues[other];
        if (!q.empty()) {
            BaseObject* object = q.back();
            q.pop_back();
            context.queue.push_back(object);
            return;
        }
        auto& aq = workerArrayQueues[other];
        if (!aq.empty()) {
            ObjArrayTask array = aq.back();
            aq.pop_back();
            context.arrayQueue.push_back(array);
            return;
        }
    }
}

template <bool VisitWeaks>
void HeapIterator::drain_and_steal(const HeapIteratorContext& context)
{
    do {
        drain<VisitWeaks>(context);
        steal(context);
    } while (!context.is_drained());
}

template <bool VisitWeaks>
void HeapIterator::object_iterate_inner(const HeapIteratorContext& context)
{
    if (context.worker_id() == 0) {
        push_strong_roots(context);
        if constexpr (VisitWeaks) {
            push_weak_roots(context);
        }
    }
    drain_and_steal<VisitWeaks>(context);
}

void HeapIterator::object_iterate(const ObjectVisitor& objectVisitor, uint32_t worker_id)
{
    object_and_field_iterate(objectVisitor, {}, worker_id);
}

void HeapIterator::object_and_field_iterate(const ObjectVisitor& objectVisitor, const EdgeVisitor& fieldVisitor,
                                            uint32_t worker_id)
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    DCHECK(!ZResurrection::is_blocked());
    HeapIteratorContext context(*this, &objectVisitor, fieldVisitor ? &fieldVisitor : nullptr, worker_id);
    if (visitWeaks) {
        object_iterate_inner<true>(context);
    } else {
        object_iterate_inner<false>(context);
    }
}

void HeapIterator::Iterate(const ObjectVisitor& objectVisitor, const EdgeVisitor& fieldVisitor)
{
    object_and_field_iterate(objectVisitor, fieldVisitor, 0);
}

} // namespace MapleRuntime
