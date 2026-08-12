// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "TraceBarrier.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#include "Collector/CopyCollector.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
namespace {
void RememberNewReference(Mutator* mutator, BaseObject* ref)
{
    if (mutator == nullptr || ref == nullptr) {
        return;
    }
    // Keep the tracing closure for references inserted after their owner may have been scanned.
    // ShouldEnqueue filters trace-region and already marked objects and deduplicates the rest.
    mutator->RememberObjectInSatbBuffer(ref);
}
} // namespace

BaseObject* TraceBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    // Bound kSelfHealAttempts: no colour lattice here (ATOMIC_READ_PROTOCOL Q2).
    for (int attempts = 0;;) {
        RefField<> oldField(field);
        BaseObject* oldTarget = to_object(oldField.GetTargetObject());
        if (oldTarget == nullptr || LIKELY(theCollector.is_load_good(oldField))) {
            return oldTarget;
        }

        BaseObject* loadGood = theCollector.make_load_good(oldField);
        // relroroot / rostatic: non-heap targets (static constants under GNU_RELRO) are never
        // evacuated. Colouring + CAS into those slots faults (si_code=2 ACCERR). Skip write-back.
        if (loadGood != nullptr && !Heap::IsHeapAddress(loadGood)) {
            return loadGood;
        }
        RefField<> goodField = theCollector.GetAndTryTagRefField(loadGood);
        // OpenJDK ZBarrier::self_heal (zBarrier.inline.hpp:72-107): the exact observed value is
        // the CAS expected value. A concurrent GC update therefore wins rather than being
        // overwritten; on failure, reload and apply the barrier to the newer value.
        if (field.CompareExchange(oldField.GetFieldValue(), goodField.GetFieldValue())) {
            return loadGood;
        }
        if (++attempts >= kSelfHealAttempts) {
            return loadGood;
        }
    }
}

BaseObject* TraceBarrier::ReadStaticRef(ReadOnlyRootSlot& field) const { return Barrier::ReadStaticRef(field); }

BaseObject* TraceBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    BaseObject* target = ReadReference(obj, field);
    DLOG(BARRIER, "read weakref obj %p ref@%p: 0x%zx", obj, &field, target);
    if (target != nullptr) {
        Mutator* mutator = Mutator::GetMutator();
        mutator->RememberObjectInSatbBuffer(target);
        // remark the referent because it may be used later.
    }
    return target;
}

void TraceBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    // Heap-src SATB / self-heal via ReadReference; non-heap dst gets StorePlain.
    if (obj != nullptr) {
        obj->ForEachRefInStruct(
            [this, obj](RefField<false>& field) {
                (void)ReadReference(obj, field);
            },
            src, src + size);
    }

    CHECK(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK);
    FixupNonHeapStructRefs(dst, obj, src, size);
}

void TraceBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    CHECK(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK);
    if (!Heap::IsHeapAddress(dst)) {
        FixupNonHeapStaticStructRefs(dst, src, size, gctib);
        return;
    }
    LocalRefFieldContainer refFields;
    gctib.ForEachBitmapWordInRange(src, [&refFields, dst, src](RefField<>& srcField) {
        MAddress offset = reinterpret_cast<MAddress>(&srcField) - src;
        refFields.Push(&HeapSlotAt<>(dst + offset));
    }, src, src + size);
    refFields.VisitRefField([this](RefField<>& dstRef) {
        (void)ReadReference(nullptr, dstRef);
    });
}

void TraceBarrier::WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    RefField<> tmpField(field);
    BaseObject* rememberedObject = nullptr;
    if (theCollector.IsOldPointer(tmpField)) {
        BaseObject* toVersion = nullptr;
        if (theCollector.TryUpdateRefField(obj, tmpField, toVersion)) {
            rememberedObject = toVersion;
        } else {
            rememberedObject = to_object(field.GetTargetObject());
        }
    } else {
        rememberedObject = to_object(tmpField.GetTargetObject());
    }

    Mutator* mutator = Mutator::GetMutator();
    if (rememberedObject != nullptr) {
        mutator->RememberObjectInSatbBuffer(rememberedObject);
    }
    RememberNewReference(mutator, ref);
    DLOG(BARRIER, "write obj %p ref-field@%p: %#zx -> %p", obj, &field, rememberedObject, ref);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    RefField<> newField = theCollector.GetAndTryTagRefField(ref);
    field.StoreColoured(newField.GetFieldValue());
}

void TraceBarrier::WriteStaticRef(RootSlot& field, BaseObject* ref) const
{
    // youngstatic / ZGC SATB: snapshot-at-beginning must retain the *previous* root target.
    // EnumBarrier::WriteStaticRef already does this; TRACE previously only enqueued `ref`
    // (RememberNewReference), so a concurrent overwrite of a static root could drop the
    // pre-write young object from the mark closure while FixMinor still VisitStaticRoots
    // the slot and ForwardObject-null → HealRoot(null).
    Mutator* mutator = Mutator::GetMutator();
    BaseObject* rememberedObject = ReadStaticRef(field);
    if (rememberedObject != nullptr && mutator != nullptr) {
        mutator->RememberObjectInSatbBuffer(rememberedObject);
    }
    RememberNewReference(mutator, ref);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    StorePlain(field, from_object(ref));
    RecordCrossGenEdge(nullptr, reinterpret_cast<MAddress>(&field), ref);
}

void TraceBarrier::WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    CHECK(obj != nullptr);
    if (obj != nullptr) {
        MRT_ASSERT(dst > reinterpret_cast<MAddress>(obj), "WriteStruct struct addr is less than obj!");
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr) {
            obj->ForEachRefInStruct(
                [=](RefField<>& refField) {
                    mutator->RememberObjectInSatbBuffer(ReadReference(obj, refField));
                },
                dst, dst + dstLen);
        }
        obj->ForEachRefInStruct(
            [=](RefField<>& refField) {
                MAddress offset = reinterpret_cast<MAddress>(&refField) - dst;
                RefField<> srcField(HeapSlotAt<>(src + offset));
                RememberNewReference(mutator, ReadReference(nullptr, srcField));
            },
            dst, dst + srcLen);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    CHECK(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK);

    if (obj != nullptr) {
        obj->ForEachRefInStruct(
            [=](RefField<>& refField) {
                RefField<> oldField(refField);
                MAddress oldValue = raw(oldField.GetFieldValue());
                BaseObject* latestVerison = ReadReference(nullptr, oldField);
                RefField<> newField = theCollector.GetAndTryTagRefField(latestVerison);
                if (oldValue != raw(newField.GetFieldValue())) {
                    refField.CompareExchange(to_zpointer(oldValue), newField.GetFieldValue());
                }
            },
            dst, dst + dstLen);
    }

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

void TraceBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const
{
    Mutator* mutator = Mutator::GetMutator();
    // youngstatic: SATB previous static-struct slots before overwrite (EnumBarrier shape).
    gctib.ForEachBitmapWord(dst, [=](RefField<>& dstField) {
        if (mutator != nullptr) {
            mutator->RememberObjectInSatbBuffer(ReadReference(nullptr, dstField));
        }
        MAddress offset = reinterpret_cast<MAddress>(&dstField) - dst;
        RefField<> srcField(HeapSlotAt<>(src + offset));
        RememberNewReference(mutator, ReadReference(nullptr, srcField));
    });
    std::atomic_thread_fence(std::memory_order_seq_cst);
    CHECK(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK);

    ResolveStaticStructRoots(dst, gctib);
    RecordStaticCrossGenEdges(dst, gctib);
    DLOG(TRACE, "write static struct@[%#zx, %#zx) with [%#zx, %#zx)", dst, dst + dstLen, src, src + srcLen);

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

BaseObject* TraceBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    BaseObject* target = nullptr;
    RefField<false> oldField(field.GetFieldValue(order));
    if (theCollector.IsCurrentPointer(oldField)) {
        target = to_object(oldField.GetTargetObject());
        DLOG(TBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()), target);
        return target;
    }

    target = ReadReference(nullptr, oldField);
    DLOG(TBARRIER, "katomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()), target);
    return target;
}

void TraceBarrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                        MemoryOrder order) const
{
    RefField<> oldField(field.GetFieldValue(order));
    MAddress oldValue = raw(oldField.GetFieldValue());
    (void)oldValue;
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    Mutator* mutator = Mutator::GetMutator();
    RememberNewReference(mutator, newRef);
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    field.StoreColoured(newField.GetFieldValue(), order);
    mutator->RememberObjectInSatbBuffer(oldRef);
    if (obj != nullptr) {
        DLOG(TBARRIER, "atomic write obj %p<%p>(%zu) ref@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(), obj->GetSize(),
             &field, oldValue, raw(newField.GetFieldValue()));
    } else {
        DLOG(TBARRIER, "atomic write static ref@%p: %#zx -> %#zx", &field, oldValue, raw(newField.GetFieldValue()));
    }
}

BaseObject* TraceBarrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                              MemoryOrder order) const
{
    Mutator* mutator = Mutator::GetMutator();
    RememberNewReference(mutator, newRef);
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    MAddress oldValue = raw(field.Exchange(newField.GetFieldValue(), order));
    RefField<> oldField(oldValue);
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    mutator->RememberObjectInSatbBuffer(oldRef);
    DLOG(TRACE, "atomic swap obj %p<%p>(%zu) ref-field@%p: old %#zx(%p), new %#zx(%p)", obj, obj->GetTypeInfo(),
         obj->GetSize(), &field, oldValue, oldRef, raw(field.GetFieldValue()), newRef);
    return oldRef;
}

bool TraceBarrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                           BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    MAddress oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
    RefField<false> oldField(oldFieldValue);
    BaseObject* oldVersion = ReadReference(nullptr, oldField);
    Mutator* mutator = Mutator::GetMutator();
    RememberNewReference(mutator, newRef);

    // Bound kCasAttempts: colour self-heal can keep raw expected bits moving (c3179214).
    for (int attempt = 0; attempt < kCasAttempts && oldVersion == oldRef; ++attempt) {
        RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
        if (field.CompareExchange(to_zpointer(oldFieldValue), newField.GetFieldValue(), succOrder, failOrder)) {
            mutator->RememberObjectInSatbBuffer(oldRef);
            return true;
        }
        oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
        RefField<false> tmp(oldFieldValue);
        oldVersion = ReadReference(nullptr, tmp);
    }

    return false;
}

void TraceBarrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                   MAddress srcField, MIndex srcSize) const
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (!dstObj->HasRefField()) {
        LOG(RTLOG_FATAL, "array %p doesn't have class-type element\n", dstObj);
        return;
    }
    if (!(static_cast<MArray*>(dstObj)->GetComponentTypeInfo()->IsStructType())) {
        LOG(RTLOG_FATAL, "array %p type is not struct type", dstObj);
        return;
    }
#endif
    Mutator* mutator = Mutator::GetMutator();
    RefFieldVisitor srcVisitor = [this, mutator](RefField<false>& field) {
        RefField<> oldField(field);
        RefField<> toBeUpdated(oldField);
        BaseObject* target = ReadReference(nullptr, toBeUpdated);
        RememberNewReference(mutator, target);
        RefField<> newField = theCollector.GetAndTryTagRefField(target);
        if (newField.GetFieldValue() != oldField.GetFieldValue()) {
            field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue());
        }
    };
    MArray* srcArray = static_cast<MArray*>(srcObj);
    srcArray->ForEachRefFieldInRange(srcVisitor, srcField, srcField + srcSize);

    RefFieldVisitor dstVisitor = [this, mutator](RefField<false>& field) {
        RefField<> oldField(field);
        BaseObject* target = ReadReference(nullptr, oldField);
        mutator->RememberObjectInSatbBuffer(target);
    };
    MArray* dstArray = static_cast<MArray*>(dstObj);
    dstArray->ForEachRefFieldInRange(dstVisitor, dstField, dstField + srcSize);

    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) ==
                     EOK,
                 "memmove_s failed");

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), dstSize);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
}

void TraceBarrier::WriteGenericImpl(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
{
    if ((obj != nullptr && !obj->HasRefField()) || (!Heap::IsHeapAddress(obj) && !Heap::IsHeapAddress(src))) {
        CHECK_DETAIL(memcpy_s(fieldPtr, size,
                              reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(src) + TYPEINFO_PTR_SIZE),
                              size) == EOK,
                     "WriteGeneric memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        if (Heap::IsHeapAddress(src)) {
            Sanitizer::TsanReadMemoryRange(
                reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(src) + TYPEINFO_PTR_SIZE), size);
        }
        if (Heap::IsHeapAddress(obj)) {
            Sanitizer::TsanWriteMemoryRange(fieldPtr, size);
        }
#endif
    } else if (!Heap::IsHeapAddress(obj) && Heap::IsHeapAddress(src)) {
        MAddress dstAddr = reinterpret_cast<MAddress>(fieldPtr);
        MAddress srcAddr = reinterpret_cast<MAddress>(src) + TYPEINFO_PTR_SIZE;
        ReadStruct(dstAddr, src, srcAddr, size);
    } else if ((Heap::IsHeapAddress(obj) && !Heap::IsHeapAddress(src))) {
        MAddress dstAddr = reinterpret_cast<MAddress>(fieldPtr);
        MAddress srcAddr = reinterpret_cast<MAddress>(src) + TYPEINFO_PTR_SIZE;
        WriteStruct(obj, dstAddr, size, srcAddr, size);
    } else {
        MAddress dstAddr = reinterpret_cast<MAddress>(fieldPtr);
        MAddress srcAddr = reinterpret_cast<MAddress>(src) + TYPEINFO_PTR_SIZE;
        void* tmp = malloc(size);
        ReadStruct((MAddress)tmp, src, srcAddr, size);
        WriteStruct(obj, dstAddr, size, (MAddress)tmp, size);
        free(tmp);
    }
}

void TraceBarrier::ReadGenericImpl(const ObjectPtr dstObj, ObjectPtr obj, void* fieldPtr, size_t size) const
{
    if (!Heap::IsHeapAddress(dstObj) && !Heap::IsHeapAddress(obj)) {
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(dstObj) + TYPEINFO_PTR_SIZE),
                              size, fieldPtr, size) == EOK,
                     "ReadGeneric memcpy_s failed");
    } else if (!Heap::IsHeapAddress(dstObj) && Heap::IsHeapAddress(obj)) {
        MAddress dstAddr = reinterpret_cast<MAddress>(dstObj) + TYPEINFO_PTR_SIZE;
        MAddress srcAddr = reinterpret_cast<MAddress>(fieldPtr);
        ReadStruct(dstAddr, obj, srcAddr, size);
    } else if ((Heap::IsHeapAddress(dstObj) && !Heap::IsHeapAddress(obj))) {
        MAddress dstAddr = reinterpret_cast<MAddress>(dstObj) + TYPEINFO_PTR_SIZE;
        MAddress srcAddr = reinterpret_cast<MAddress>(fieldPtr);
        WriteStruct(dstObj, dstAddr, size, srcAddr, size);
    } else {
        MAddress dstAddr = reinterpret_cast<MAddress>(dstObj) + TYPEINFO_PTR_SIZE;
        MAddress srcAddr = reinterpret_cast<MAddress>(fieldPtr);
        void* tmp = malloc(size);
        ReadStruct((MAddress)tmp, obj, srcAddr, size);
        WriteStruct(dstObj, dstAddr, size, (MAddress)tmp, size);
        free(tmp);
    }
}
} // namespace MapleRuntime
