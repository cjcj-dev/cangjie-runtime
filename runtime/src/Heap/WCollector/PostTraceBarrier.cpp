// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "PostTraceBarrier.h"

#include "Mutator/Mutator.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
BaseObject* PostTraceBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    RefField<> tmpField(field);
    // E-class = !IsOldPointer (zcolor9 dual was too strong; see ForwardUpdateRawRef).
    CHECK(!theCollector.IsOldPointer(tmpField));
    return to_object(tmpField.GetTargetObject());
}

BaseObject* PostTraceBarrier::ReadStaticRef(ReadOnlyRootSlot& field) const { return Barrier::ReadStaticRef(field); }

BaseObject* PostTraceBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    RefField<> tmpField(field);
    // E-class = !IsOldPointer (same restoration as ReadReference).
    CHECK(!theCollector.IsOldPointer(tmpField));
    BaseObject* referent = to_object(tmpField.GetTargetObject());
    if (referent == nullptr) {
        return nullptr;
    }
    RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(referent));
    bool isMarked = regionInfo->IsMarkedObject(referent);
    if (!isMarked) { // skip live referents
        void** referentAddr = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(obj) + TYPEINFO_PTR_SIZE);
        DLOG(BARRIER, "update referent@%p: 0x%zx -> %p", referentAddr, *referentAddr, nullptr);
        *referentAddr = nullptr; // set referent field as null
        return nullptr;
    }
    return to_object(tmpField.GetTargetObject());
}

void PostTraceBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    // Heap-src: E-class ReadReference (assertion-only). Non-heap dst: StorePlain, never coloured heal.
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

void PostTraceBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    gctib.ForEachBitmapWordInRange(
        src,
        [this](RefField<>& srcField) {
            (void)ReadReference(nullptr, srcField);
        },
        src, src + size);

    CHECK(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK);
    if (!Heap::IsHeapAddress(dst)) {
        FixupNonHeapStaticStructRefs(dst, src, size, gctib);
        return;
    }
    // Heap dst residual path (should be rare for ReadStaticStruct): peel via ReadReference only.
    gctib.ForEachBitmapWord(dst, [this](RefField<>& dstRef) {
        (void)ReadReference(nullptr, dstRef);
    });
}

void PostTraceBarrier::WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    RefField<> tmpField(field);
    // E-class = !IsOldPointer (zcolor9 dual was too strong).
    CHECK(!theCollector.IsOldPointer(tmpField));
    DLOG(BARRIER, "write obj %p ref-field@%p: %#zx -> %p", obj, &field, raw(tmpField.GetFieldValue()), ref);
    RefField<> newField = theCollector.GetAndTryTagRefField(ref);
    field.StoreColoured(newField.GetFieldValue());
}

void PostTraceBarrier::WriteStaticRef(RootSlot& field, BaseObject* ref) const
{
    StorePlain(field, from_object(ref));
    RecordCrossGenEdge(nullptr, reinterpret_cast<MAddress>(&field), ref);
}

void PostTraceBarrier::WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    CHECK(obj != nullptr);
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

void PostTraceBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                         const GCTib gctib) const
{
    CHECK(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK);

    gctib.ForEachBitmapWord(dst, [=](RefField<>& refField) {
        RefField<> oldField(refField);
        MAddress oldValue = raw(oldField.GetFieldValue());
        BaseObject* untagged = ReadReference(nullptr, oldField);
        RefField<> newField = theCollector.GetAndTryTagRefField(untagged);
        if (oldValue != raw(newField.GetFieldValue())) {
            refField.CompareExchange(to_zpointer(oldValue), newField.GetFieldValue());
        }
    });
    RecordStaticCrossGenEdges(dst, gctib);
    DLOG(TRACE, "write static struct@[%#zx, %#zx) with [%#zx, %#zx)", dst, dst + dstLen, src, src + srcLen);

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

BaseObject* PostTraceBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    // Colour self-heal on the real slot (OpenJDK ZBarrier::self_heal, zBarrier.inline.hpp:72-107).
    // Exact observed value is the CAS expected; concurrent updates win and force a reload.
    // Bound kSelfHealAttempts: no colour lattice here (ATOMIC_READ_PROTOCOL Q2).
    for (int attempts = 0;;) {
        RefField<false> oldField(field.GetFieldValue(order));
        BaseObject* oldTarget = to_object(oldField.GetTargetObject());
        if (oldTarget == nullptr || LIKELY(theCollector.is_load_good(oldField))) {
            DLOG(TBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()), oldTarget);
            return oldTarget;
        }

        BaseObject* loadGood = theCollector.make_load_good(oldField);
        // relroroot / rostatic: non-heap targets under GNU_RELRO — skip colour CAS write-back.
        if (loadGood != nullptr && !Heap::IsHeapAddress(loadGood)) {
            return loadGood;
        }
        RefField<> goodField = theCollector.GetAndTryTagRefField(loadGood);
        DCHECK(theCollector.is_load_good(goodField));
        if (field.CompareExchange(oldField.GetFieldValue(), goodField.GetFieldValue())) {
            DLOG(TBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()), loadGood);
            return loadGood;
        }
        if (++attempts >= kSelfHealAttempts) {
            return loadGood;
        }
    }
}

void PostTraceBarrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                            MemoryOrder order) const
{
    RefField<> oldField(field.GetFieldValue(order));
    MAddress oldValue = raw(oldField.GetFieldValue());
    (void)oldValue;
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    field.StoreColoured(newField.GetFieldValue(), order);
    if (obj != nullptr) {
        DLOG(TBARRIER, "atomic write obj %p<%p>(%zu) ref@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(), obj->GetSize(),
             &field, oldValue, raw(newField.GetFieldValue()));
    } else {
        DLOG(TBARRIER, "atomic write static ref@%p: %#zx -> %#zx", &field, oldValue, raw(newField.GetFieldValue()));
    }
}

BaseObject* PostTraceBarrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                                  MemoryOrder order) const
{
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    MAddress oldValue = raw(field.Exchange(newField.GetFieldValue(), order));
    RefField<> oldField(oldValue);
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    DLOG(TRACE, "atomic swap obj %p<%p>(%zu) ref-field@%p: old %#zx(%p), new %#zx(%p)", obj, obj->GetTypeInfo(),
         obj->GetSize(), &field, oldValue, oldRef, raw(field.GetFieldValue()), newRef);
    return oldRef;
}

bool PostTraceBarrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
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

void PostTraceBarrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
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
    bool inHeap = Heap::IsHeapAddress(srcObj);
    RefFieldVisitor srcVisitor = [this](RefField<false>& field) {
        RefField<> oldField(field);
        RefField<> toBeUpdated(oldField);
        BaseObject* target = ReadReference(nullptr, toBeUpdated);
        RefField<> newField = theCollector.GetAndTryTagRefField(target);
        if (newField.GetFieldValue() != oldField.GetFieldValue()) {
            field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue());
        }
    };
    MArray* srcArray = static_cast<MArray*>(srcObj);
    if (!inHeap) {
        srcArray->ForEachRefFieldInRange(srcVisitor, srcField, srcField + srcSize);
    }
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) ==
                     EOK,
                 "memmove_s failed");

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), dstSize);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
}
} // namespace MapleRuntime
