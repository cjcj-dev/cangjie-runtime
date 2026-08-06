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
    for (;;) {
        RefField<> oldField(field);
        BaseObject* oldTarget = oldField.GetTargetObject();
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

        RefField<> goodField = theCollector.GetAndTryTagRefField(loadGood);
        // OpenJDK ZBarrier::self_heal (zBarrier.inline.hpp:72-107): retain the exact
        // observed value as the CAS expected value and retry after a concurrent update.
        if (field.CompareExchange(oldField.GetFieldValue(), goodField.GetFieldValue())) {
            return loadGood;
        }
    }
}

BaseObject* PreforwardBarrier::ReadStaticRef(RefField<false>& field) const { return ReadReference(nullptr, field); }

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
        DLOG(FIX, "read static ref-field(in struct)@%p: 0x%zx -> %p", &field, oldField.GetFieldValue(), target);
    });
}

BaseObject* PreforwardBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    for (;;) {
        RefField<false> oldField(field.GetFieldValue(order));
        BaseObject* oldTarget = oldField.GetTargetObject();
        if (oldTarget == nullptr || LIKELY(theCollector.is_load_good(oldField))) {
            DLOG(PBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, oldField.GetFieldValue(), oldTarget);
            return oldTarget;
        }

        BaseObject* loadGood = oldTarget;
        if (!theCollector.IsUnmovableFromObject(oldTarget)) {
            loadGood = theCollector.make_load_good(oldField);
            if (theCollector.IsGhostFromObject(loadGood)) {
                loadGood = theCollector.ForwardObject(loadGood);
            }
        }

        RefField<> goodField = theCollector.GetAndTryTagRefField(loadGood);
        // Replaces the old "not old-tag" assertion with the colour-era self-heal invariant.
        CHECK(theCollector.is_load_good(goodField));
        if (field.CompareExchange(oldField.GetFieldValue(), goodField.GetFieldValue())) {
            DLOG(PBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, oldField.GetFieldValue(), loadGood);
            return loadGood;
        }
    }
}

void PreforwardBarrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                             MemoryOrder order) const
{
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    field.SetFieldValue(newField.GetFieldValue(), order);
    if (obj != nullptr) {
        DLOG(PBARRIER, "atomic write obj %p<%p>(%zu) ref@%p: %#zx", obj, obj->GetTypeInfo(), obj->GetSize(), &field,
             newField.GetFieldValue());
    } else {
        DLOG(PBARRIER, "atomic write static ref@%p: %#zx", &field, newField.GetFieldValue());
    }
}

BaseObject* PreforwardBarrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                                   MemoryOrder order) const
{
    RefField<> coloured = theCollector.GetAndTryTagRefField(newRef);
    MAddress oldValue = field.Exchange(coloured.GetFieldValue(), order);
    RefField<> oldField(oldValue);
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    DLOG(BARRIER, "atomic swap obj %p<%p>(%zu) ref@%p: old %#zx(%p), new %#zx(%p)", obj, obj->GetTypeInfo(),
         obj->GetSize(), &field, oldValue, oldRef, field.GetFieldValue(), newRef);
    return oldRef;
}

bool PreforwardBarrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                                BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    MAddress oldFieldValue = field.GetFieldValue(std::memory_order_seq_cst);
    RefField<false> oldField(oldFieldValue);
    BaseObject* oldVersion = ReadReference(nullptr, oldField);
    while (oldVersion == oldRef) {
        if (field.CompareExchange(oldFieldValue, newField.GetFieldValue(), succOrder, failOrder)) {
            return true;
        }
        oldFieldValue = field.GetFieldValue(std::memory_order_seq_cst);
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
            MAddress oldValue = oldField.GetFieldValue();
            BaseObject* latest = ReadReference(nullptr, oldField);
            RefField<> newField = theCollector.GetAndTryTagRefField(latest);
            if (oldValue != newField.GetFieldValue()) {
                field.CompareExchange(oldValue, newField.GetFieldValue());
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
