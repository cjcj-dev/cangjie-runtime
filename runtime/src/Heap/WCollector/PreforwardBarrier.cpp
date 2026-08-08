// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "PreforwardBarrier.h"

#include "Base/SysCall.h"
#include "Common/ScopedObjectLock.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
BaseObject* PreforwardBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    // Bound kSelfHealAttempts: colour writers can re-tag the same slot (ATOMIC_READ_PROTOCOL Q2).
    for (int attempts = 0;;) {
        RefField<> oldField(field);
        BaseObject* oldTarget = to_object(oldField.GetTargetObject());
        if (oldTarget == nullptr || LIKELY(theCollector.is_load_good(oldField))) {
            return oldTarget;
        }

        BaseObject* loadGood = oldTarget;
        if (!theCollector.IsUnmovableFromObject(oldTarget)) {
            loadGood = theCollector.make_load_good(oldField);
            if (theCollector.IsGhostFromObject(loadGood)) {
                loadGood = theCollector.ForwardObject(loadGood);
            }
        }
        // relroroot / rostatic: non-heap targets (static constants under GNU_RELRO) are never
        // evacuated. Colouring + CAS into those slots faults (si_code=2 ACCERR). Skip write-back.
        if (loadGood != nullptr && !Heap::IsHeapAddress(loadGood)) {
            return loadGood;
        }

        RefField<> goodField = theCollector.GetAndTryTagRefField(loadGood);
        // OpenJDK ZBarrier::self_heal (zBarrier.inline.hpp:72-107): retain the exact
        // observed value as the CAS expected value and retry after a concurrent update.
        if (field.CompareExchange(oldField.GetFieldValue(), goodField.GetFieldValue())) {
            return loadGood;
        }
        if (++attempts >= kSelfHealAttempts) {
            return loadGood;
        }
    }
}

BaseObject* PreforwardBarrier::ReadStaticRef(RootSlot& field) const { return Barrier::ReadStaticRef(field); }

BaseObject* PreforwardBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    return ReadReference(obj, field);
}

void PreforwardBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    if (obj != nullptr) {
        // note fix/untag dst would be better.
        obj->ForEachRefInStruct(
            [this, obj](RefField<false>& field) {
                RefField<> oldField(field);
                BaseObject* target = ReadReference(obj, field);
                (void)target;
            },
            src, src + size);
    }

    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK,
                 "read struct memcpy_s failed");
}

void PreforwardBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK,
                 "read struct memcpy_s failed");
    gctib.ForEachBitmapWord(dst, [=](RefField<>& field) {
        RefField<> oldField(field);
        BaseObject* target = ReadReference(nullptr, field);
        (void)target;
        DLOG(FIX, "read static ref-field(in struct)@%p: 0x%zx -> %p", &field, raw(oldField.GetFieldValue()), target);
    });
}

BaseObject* PreforwardBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    // Bound kSelfHealAttempts: colour writers can re-tag the same slot (ATOMIC_READ_PROTOCOL Q2).
    for (int attempts = 0;;) {
        RefField<false> oldField(field.GetFieldValue(order));
        BaseObject* oldTarget = to_object(oldField.GetTargetObject());
        if (oldTarget == nullptr || LIKELY(theCollector.is_load_good(oldField))) {
            DLOG(PBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()), oldTarget);
            return oldTarget;
        }

        BaseObject* loadGood = oldTarget;
        if (!theCollector.IsUnmovableFromObject(oldTarget)) {
            loadGood = theCollector.make_load_good(oldField);
            if (theCollector.IsGhostFromObject(loadGood)) {
                loadGood = theCollector.ForwardObject(loadGood);
            }
        }
        // relroroot / rostatic: non-heap targets under GNU_RELRO — skip colour CAS write-back.
        if (loadGood != nullptr && !Heap::IsHeapAddress(loadGood)) {
            return loadGood;
        }

        RefField<> goodField = theCollector.GetAndTryTagRefField(loadGood);
        // Replaces the old "not old-tag" assertion with the colour-era self-heal invariant.
        CHECK(theCollector.is_load_good(goodField));
        if (field.CompareExchange(oldField.GetFieldValue(), goodField.GetFieldValue())) {
            DLOG(PBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()), loadGood);
            return loadGood;
        }
        if (++attempts >= kSelfHealAttempts) {
            return loadGood;
        }
    }
}

void PreforwardBarrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                             MemoryOrder order) const
{
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    field.StoreColoured(newField.GetFieldValue(), order);
    if (obj != nullptr) {
        DLOG(PBARRIER, "atomic write obj %p<%p>(%zu) ref@%p: %#zx", obj, obj->GetTypeInfo(), obj->GetSize(), &field,
             raw(newField.GetFieldValue()));
    } else {
        DLOG(PBARRIER, "atomic write static ref@%p: %#zx", &field, raw(newField.GetFieldValue()));
    }
}

BaseObject* PreforwardBarrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                                   MemoryOrder order) const
{
    RefField<> coloured = theCollector.GetAndTryTagRefField(newRef);
    MAddress oldValue = raw(field.Exchange(coloured.GetFieldValue(), order));
    RefField<> oldField(oldValue);
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    DLOG(BARRIER, "atomic swap obj %p<%p>(%zu) ref@%p: old %#zx(%p), new %#zx(%p)", obj, obj->GetTypeInfo(),
         obj->GetSize(), &field, oldValue, oldRef, raw(field.GetFieldValue()), newRef);
    return oldRef;
}

bool PreforwardBarrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                                BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    MAddress oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
    RefField<false> oldField(oldFieldValue);
    BaseObject* oldVersion = ReadReference(nullptr, oldField);
    // Bound kCasAttempts: colour self-heal can keep raw expected bits moving (c3179214).
    for (int attempt = 0; attempt < kCasAttempts && oldVersion == oldRef; ++attempt) {
        RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
        if (field.CompareExchange(to_zpointer(oldFieldValue), newField.GetFieldValue(), succOrder, failOrder)) {
            return true;
        }
        oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
        RefField<false> tmp(oldFieldValue);
        oldVersion = ReadReference(nullptr, tmp);
    }

    return false;
}

void PreforwardBarrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                        MAddress srcField, MIndex srcSize) const
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (!(static_cast<MArray*>(dstObj)->GetComponentTypeInfo()->IsStructType())) {
        LOG(RTLOG_FATAL, "array %p type is not struct type", dstObj);
        return;
    }
#endif

    if (!dstObj->HasRefField()) {
        CHECK(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) == EOK);
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), dstSize);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
        return;
    }

    MArray* srcArray = static_cast<MArray*>(srcObj);
    RefFieldVisitor srcVisitor = [this, srcArray](RefField<false>& field) { (void)ReadReference(srcArray, field); };
    srcArray->ForEachRefFieldInRange(srcVisitor, srcField, srcField + srcSize);

    CHECK(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) == EOK);

    // R9 bulk：堆 dst 补色（与 Idle/base 同形）。
    if (dstObj != nullptr && Heap::IsHeapAddress(dstObj) && dstObj->HasRefField()) {
        RefFieldVisitor recolour = [this](RefField<false>& field) {
            RefField<> oldField(field);
            MAddress oldValue = raw(oldField.GetFieldValue());
            BaseObject* latest = ReadReference(nullptr, oldField);
            RefField<> newField = theCollector.GetAndTryTagRefField(latest);
            if (oldValue != raw(newField.GetFieldValue())) {
                field.CompareExchange(to_zpointer(oldValue), newField.GetFieldValue());
            }
        };
        static_cast<MArray*>(dstObj)->ForEachRefFieldInRange(recolour, dstField, dstField + srcSize);
    }

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), dstSize);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
}
} // namespace MapleRuntime
