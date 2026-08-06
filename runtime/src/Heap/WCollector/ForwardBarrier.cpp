// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "ForwardBarrier.h"

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
BaseObject* ForwardBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
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

BaseObject* ForwardBarrier::ReadStaticRef(RefField<false>& field) const { return ReadReference(nullptr, field); }

BaseObject* ForwardBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    return ReadReference(obj, field);
}

void ForwardBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    CHECK(!Heap::IsHeapAddress(dst));
    if (obj != nullptr) {
        obj->ForEachRefInStruct(
            [this, obj](RefField<false>& field) {
                BaseObject* target = ReadReference(obj, field);
                (void)target;
            },
            src, src + size);
    }
    CHECK(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK);
}

void ForwardBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    CHECK(!Heap::IsHeapAddress(src));
    CHECK(!Heap::IsHeapAddress(dst));
    gctib.ForEachBitmapWord(src, [=](RefField<>& srcField) {
        BaseObject* target = ReadReference(nullptr, srcField);
        (void)target;
    });
    CHECK(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK);
}

BaseObject* ForwardBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    for (;;) {
        RefField<false> oldField(field.GetFieldValue(order));
        BaseObject* oldTarget = oldField.GetTargetObject();
        if (oldTarget == nullptr || LIKELY(theCollector.is_load_good(oldField))) {
            DLOG(FBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, oldField.GetFieldValue(), oldTarget);
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
        DCHECK(theCollector.is_load_good(goodField));
        if (field.CompareExchange(oldField.GetFieldValue(), goodField.GetFieldValue())) {
            DLOG(FBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, oldField.GetFieldValue(), loadGood);
            return loadGood;
        }
    }
}

void ForwardBarrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                          MemoryOrder order) const
{
    RefField<> newField(newRef);
    field.SetFieldValue(newField.GetFieldValue(), order);
    if (obj != nullptr) {
        DLOG(FBARRIER, "atomic write obj %p<%p>(%zu) ref@%p: %#zx", obj, obj->GetTypeInfo(), obj->GetSize(), &field,
             newField.GetFieldValue());
    } else {
        DLOG(FBARRIER, "atomic write static ref@%p: %#zx", &field, newField.GetFieldValue());
    }
}

BaseObject* ForwardBarrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                                MemoryOrder order) const
{
    MAddress oldValue = field.Exchange(newRef, order);
    RefField<> oldField(oldValue);
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    DLOG(BARRIER, "atomic swap obj %p<%p>(%zu) ref-field@%p: old %#zx(%p), new %#zx(%p)", obj, obj->GetTypeInfo(),
         obj->GetSize(), &field, oldValue, oldRef, field.GetFieldValue(), newRef);
    return oldRef;
}

bool ForwardBarrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                             BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    MAddress oldFieldValue = field.GetFieldValue(std::memory_order_seq_cst);
    RefField<false> oldField(oldFieldValue);
    BaseObject* oldVersion = ReadReference(nullptr, oldField);
    while (oldVersion == oldRef) {
        RefField<> newField(newRef);
        if (field.CompareExchange(oldFieldValue, newField.GetFieldValue(), succOrder, failOrder)) {
            return true;
        }
        oldFieldValue = field.GetFieldValue(std::memory_order_seq_cst);
        RefField<false> tmp(oldFieldValue);
        oldVersion = ReadReference(nullptr, tmp);
    }

    return false;
}

void ForwardBarrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
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

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), dstSize);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
}
} // namespace MapleRuntime
