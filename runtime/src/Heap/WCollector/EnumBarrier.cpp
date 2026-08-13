// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "EnumBarrier.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#include "Collector/CopyCollector.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
BaseObject* EnumBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
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

BaseObject* EnumBarrier::ReadStaticRef(ReadOnlyRootSlot& field) const { return Barrier::ReadStaticRef(field); }

BaseObject* EnumBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
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

void EnumBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
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

void EnumBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    CHECK(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK);
    if (!Heap::IsHeapAddress(dst)) {
        FixupNonHeapStaticStructRefs(dst, src, size, gctib);
        return;
    }
    LocalRefFieldContainer refFields;
    gctib.ForEachBitmapWordInRange(
        src,
        [&refFields, dst, src](RefField<>& srcField) {
            MAddress offset = reinterpret_cast<MAddress>(&srcField) - src;
            refFields.Push(&HeapSlotAt<>(dst + offset));
        },
        src, src + size);
    refFields.VisitRefField([this](RefField<>& dstRef) {
        (void)ReadReference(nullptr, dstRef);
    });
}

void EnumBarrier::WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    RefField<> tmpField(field);
    BaseObject* remeberedObject = nullptr;
    if (theCollector.IsOldPointer(tmpField)) {
        BaseObject* toVersion = nullptr;
        if (theCollector.TryUpdateRefField(obj, tmpField, toVersion)) {
            remeberedObject = toVersion;
        } else {
            remeberedObject = to_object(field.GetTargetObject());
        }
    } else {
        remeberedObject = to_object(tmpField.GetTargetObject());
    }
    Mutator* mutator = Mutator::GetMutator();
    if (remeberedObject != nullptr) {
        mutator->RememberObjectInSatbBuffer(remeberedObject);
    }
    if (ref != nullptr) {
        mutator->RememberObjectInSatbBuffer(ref);
    }
    DLOG(BARRIER, "write obj %p ref@%p: 0x%zx -> %p", obj, &field, remeberedObject, ref);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    RefField<> newField = theCollector.GetAndTryTagRefField(ref);
    field.StoreColoured(newField.GetFieldValue());
}

void EnumBarrier::WriteStaticRef(RootSlot& field, BaseObject* ref) const
{
    BaseObject* rememberedObject = ReadStaticRef(field);
    Mutator* mutator = Mutator::GetMutator();
    if (rememberedObject != nullptr) {
        mutator->RememberObjectInSatbBuffer(rememberedObject);
    }
    if (ref != nullptr) {
        mutator->RememberObjectInSatbBuffer(ref);
    }
    DLOG(BARRIER, "write static ref@%p: %p -|> %p", &field, rememberedObject, ref);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    StorePlain(field, from_object(ref));
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    RecordCrossGenEdge(nullptr, reinterpret_cast<MAddress>(&field), ref);
#endif
}

void EnumBarrier::WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    if (obj != nullptr) {
        MRT_ASSERT(dst > reinterpret_cast<MAddress>(obj), "WriteStruct struct addr is less than obj!");
        Mutator* mutator = Mutator::GetMutator();
        obj->ForEachRefInStruct(
            [=](RefField<>& dstField) {
                mutator->RememberObjectInSatbBuffer(ReadReference(obj, dstField));
                MAddress offset = reinterpret_cast<MAddress>(&dstField) - dst;
                HeapSlot<> srcField(HeapSlotAt<>(src + offset));
                mutator->RememberObjectInSatbBuffer(ReadReference(nullptr, srcField));
            },
            dst, dst + srcLen);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK,
                 "memcpy_s failed");

    if (obj != nullptr) {
        obj->ForEachRefInStruct(
            [=](RefField<>& refField) {
                RefField<> oldField(refField);
                RefField<> toBeUpdated(oldField);
                BaseObject* untagged = ReadReference(nullptr, toBeUpdated);
                RefField<> newField = theCollector.GetAndTryTagRefField(untagged);
                if (oldField.GetFieldValue() != newField.GetFieldValue()) {
                    refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue());
                }
            },
            dst, dst + dstLen);
    }

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

void EnumBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const
{
    Mutator* mutator = Mutator::GetMutator();
    gctib.ForEachBitmapWord(dst, [=](RefField<>& dstField) {
        mutator->RememberObjectInSatbBuffer(ReadReference(nullptr, dstField));
        uint32_t offset = reinterpret_cast<MAddress>(&dstField) - dst;
        HeapSlot<> srcField(HeapSlotAt<>(src + offset));
        mutator->RememberObjectInSatbBuffer(ReadReference(nullptr, srcField));
    });
    std::atomic_thread_fence(std::memory_order_seq_cst);
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK,
                 "memcpy_s failed");

    ResolveStaticStructRoots(dst, gctib);
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    gctib.ForEachBitmapWord(dst, [this](RefField<>& field) {
        RecordCrossGenEdge(nullptr, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
    });
#endif

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

BaseObject* EnumBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    BaseObject* target = nullptr;
    RefField<false> oldField(field.GetFieldValue(order));
    if (theCollector.IsCurrentPointer(oldField)) {
        target = to_object(oldField.GetTargetObject());
        DLOG(EBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()), target);
        return target;
    }

    target = ReadReference(nullptr, oldField);
    DLOG(EBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()), target);
    return target;
}

BaseObject* EnumBarrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                             MemoryOrder order) const
{
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    MAddress oldValue = raw(field.Exchange(newField.GetFieldValue(), order));
    RefField<> oldField(oldValue);
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    Mutator* mutator = Mutator::GetMutator();
    mutator->RememberObjectInSatbBuffer(oldRef);
    mutator->RememberObjectInSatbBuffer(newRef);
    DLOG(BARRIER, "atomic swap obj %p<%p>(%zu) ref@%p: old %#zx(%p), new %#zx(%p)", obj, obj->GetTypeInfo(),
         obj->GetSize(), &field, oldValue, oldRef, raw(field.GetFieldValue()), newRef);
    return oldRef;
}

void EnumBarrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                       MemoryOrder order) const
{
    RefField<> oldField(field.GetFieldValue(order));
    MAddress oldValue = raw(oldField.GetFieldValue());
    (void)oldValue;
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    field.StoreColoured(newField.GetFieldValue(), order);
    Mutator* mutator = Mutator::GetMutator();
    mutator->RememberObjectInSatbBuffer(oldRef);
    mutator->RememberObjectInSatbBuffer(newRef);
    if (obj != nullptr) {
        DLOG(EBARRIER, "atomic write obj %p<%p>(%zu) ref@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(), obj->GetSize(),
             &field, oldValue, raw(newField.GetFieldValue()));
    } else {
        DLOG(EBARRIER, "atomic write static ref@%p: %#zx -> %#zx", &field, oldValue, raw(newField.GetFieldValue()));
    }
}

bool EnumBarrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                          BaseObject* newRef, MemoryOrder sOrder, MemoryOrder fOrder) const
{
    MAddress oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
    RefField<false> oldField(oldFieldValue);
    BaseObject* oldVersion = ReadReference(nullptr, oldField);

    // Bound kCasAttempts: colour self-heal can keep raw expected bits moving (c3179214).
    for (int attempt = 0; attempt < kCasAttempts && oldVersion == oldRef; ++attempt) {
        RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
        if (field.CompareExchange(to_zpointer(oldFieldValue), newField.GetFieldValue(), sOrder, fOrder)) {
            Mutator* mutator = Mutator::GetMutator();
            mutator->RememberObjectInSatbBuffer(oldRef);
            mutator->RememberObjectInSatbBuffer(newRef);
            return true;
        }
        oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
        RefField<false> tmp(oldFieldValue);
        oldVersion = ReadReference(nullptr, tmp);
    }

    return false;
}

void EnumBarrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                  MAddress srcField, MIndex srcSize) const
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (!(static_cast<MArray*>(dstObj)->GetComponentTypeInfo()->IsStructType())) {
        LOG(RTLOG_FATAL, "array %p type is not struct type", dstObj);
        return;
    }
#endif
    if (!dstObj->HasRefField()) {
        CHECK_DETAIL(
            memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) == EOK,
            "memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), dstSize);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
        return;
    }

    Mutator* mutator = Mutator::GetMutator();
    RefFieldVisitor srcVisitor = [this, mutator](RefField<false>& field) {
        RefField<> oldField(field);
        RefField<> toBeUpdated(oldField);
        BaseObject* target = ReadReference(nullptr, toBeUpdated);
        mutator->RememberObjectInSatbBuffer(target);
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

void EnumBarrier::WriteGenericImpl(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
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

void EnumBarrier::ReadGenericImpl(const ObjectPtr dstObj, ObjectPtr obj, void* fieldPtr, size_t size) const
{
    if (!Heap::IsHeapAddress(dstObj) && !Heap::IsHeapAddress(obj)) {
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(dstObj) + TYPEINFO_PTR_SIZE), size,
                              fieldPtr, size) == EOK,
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
