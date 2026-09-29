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
    : workerQueues(nworkers), workerArrayQueues(nworkers),
      visitWeaks(visitWeaks), forVerify(forVerify), rootsColored(workerQueues.size()),
      rootsUncolored(), rootsWeakColored(workerQueues.size()), terminator(workerQueues.size(), &workerQueues),
      objectBitmaps(ZAddressOffsetMax)
{
    for (unsigned i = 0; i < workerQueues.size(); ++i) {
        workerQueues.register_queue(i, new ObjectQueue());
        workerArrayQueues.register_queue(i, new ArrayQueue());
    }
}

HeapIterator::~HeapIterator()
{
    ZGranuleMapIterator<HeapIteratorBitMap*, false> bitmaps(&objectBitmaps);
    for (HeapIteratorBitMap* bitmap; bitmaps.next(&bitmap);) {
        delete bitmap;
    }
    for (unsigned i = 0; i < workerQueues.size(); ++i) {
        delete workerQueues.queue(i);
        delete workerArrayQueues.queue(i);
    }
}

static size_t object_index_max()
{
    return ZGranuleSize / 8;
}

static size_t object_index(BaseObject* object)
{
    const zoffset offset = ZAddress::offset(to_zaddress_unsafe(reinterpret_cast<uintptr_t>(object)));
    return (untype(offset) & (ZGranuleSize - 1)) / 8;
}

// ZGC zHeapIterator.cpp:312-327: acquire lookup, locked recheck, release install.
HeapIteratorBitMap* HeapIterator::object_bitmap(BaseObject* object)
{
    const zoffset offset = ZAddress::offset(to_zaddress_unsafe(reinterpret_cast<uintptr_t>(object)));
    HeapIteratorBitMap* bitmap = objectBitmaps.get_acquire(offset);
    if (bitmap == nullptr) {
        ZLocker<ZLock> lock(&bitmapLock);
        bitmap = objectBitmaps.get(offset);
        if (bitmap == nullptr) {
            bitmap = new HeapIteratorBitMap(object_index_max());
            objectBitmaps.release_put(offset, bitmap);
        }
    }
    return bitmap;
}

bool HeapIterator::mark_object(BaseObject* object)
{
    if (object == nullptr) return false;
    return object_bitmap(object)->try_set_bit(object_index(object));
}

void HeapIteratorContext::visit_object(BaseObject* object) const
{
    if (objectVisitor != nullptr && *objectVisitor) {
        (*objectVisitor)(object);
    }
}

bool HeapIterator::should_visit_object_at_mark() const { return forVerify; }
bool HeapIterator::should_visit_object_at_follow() const { return !forVerify; }

void HeapIterator::mark_visit_and_push(const HeapIteratorContext& context, BaseObject* object)
{
    if (mark_object(object)) {
        if (should_visit_object_at_mark()) context.visit_object(object);
        context.push(object);
    }
}

void HeapIteratorContext::push(BaseObject* object) const
{
    queue->push(object);
}

void HeapIteratorContext::push_array_chunk(const HeapIterator::ObjArrayTask& array) const
{
    arrayQueue->push(array);
}

bool HeapIteratorContext::pop(BaseObject*& object) const
{
    return queue->pop_overflow(object) || queue->pop_local(object);
}

bool HeapIteratorContext::pop_array_chunk(HeapIterator::ObjArrayTask& array) const
{
    return arrayQueue->pop_overflow(array) || arrayQueue->pop_local(array);
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
    iter->mark_visit_and_push(context, load_oop(field));
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
    if (should_visit_object_at_follow()) {
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
    iter.mark_visit_and_push(context, load_oop(&root));
}

void HeapIterator::UncoloredRootOopClosure::do_root(ObjectRef& root)
{
    if (context.fieldVisitor != nullptr && *context.fieldVisitor) {
        (*context.fieldVisitor)(nullptr, &root, raw(root.LoadPlain()));
    }
    iter.mark_visit_and_push(context, to_object(safe(root.LoadPlain())));
}

void HeapIterator::push_strong_roots(const HeapIteratorContext& context)
{
    ColoredRootOopClosure<false> colored(*this, context);
    rootsColored.Apply([&](NativeSlot& root) { colored.do_root(root); });
    UncoloredRootOopClosure uncolored(*this, context);
    rootsUncolored.Apply([&] {
        ZMark::VisitStrongPlainRoots([&](ObjectRef& root) { uncolored.do_root(root); }, {});
    });
    rootsUncolored.ApplyThreads([&](Mutator& mutator) {
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
    rootsWeakColored.Apply([&](NativeSlot& root) { colored.do_root(root); });
}

template<bool VisitWeaks>
void HeapIterator::push_roots(const HeapIteratorContext& context)
{
    push_strong_roots(context);
    if constexpr (VisitWeaks) push_weak_roots(context);
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

template <bool VisitWeaks>
void HeapIterator::steal(const HeapIteratorContext& context)
{
    ObjArrayTask array { nullptr, 0 };
    BaseObject* object = nullptr;
    if (steal_array_chunk(context, array)) {
        follow_array_chunk(context, array);
    } else if (steal(context, object)) {
        visit_and_follow<VisitWeaks>(context, object);
    }
}

bool HeapIterator::steal(const HeapIteratorContext& context, BaseObject*& object)
{
    return workerQueues.steal(context.worker_id(), object);
}

bool HeapIterator::steal_array_chunk(const HeapIteratorContext& context, ObjArrayTask& array)
{
    return workerArrayQueues.steal(context.worker_id(), array);
}

template <bool VisitWeaks>
void HeapIterator::drain_and_steal(const HeapIteratorContext& context)
{
    do {
        drain<VisitWeaks>(context);
        steal<VisitWeaks>(context);
    } while (!context.is_drained() || !terminator.offer_termination());
}

template <bool VisitWeaks>
void HeapIterator::object_iterate_inner(const HeapIteratorContext& context)
{
    push_roots<VisitWeaks>(context);
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
