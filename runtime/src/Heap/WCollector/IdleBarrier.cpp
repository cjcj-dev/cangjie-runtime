// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "IdleBarrier.h"

#include "Mutator/Mutator.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "securec.h"

namespace MapleRuntime {
BaseObject* IdleBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    // Bound kSelfHealAttempts: no colour lattice here (ATOMIC_READ_PROTOCOL Q2).
    // A bare `do { ... } while (true)` with no progress path livelocks the mutator
    // (no safepoint) and GC then spins forever in EnsurePhaseTransition(IDLE) —
    // deadlock2_gcfloor388_0010: mutator in IdleBarrier::ReadReference vs gc-main
    // in EnsurePhaseTransition(GC_PHASE_IDLE). Sibling barriers already bounded
    // (Enum/Trace/Forward/Preforward @ e0824f25); Idle was the leftover.
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
            DLOG(BARRIER, "heal obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()), loadGood);
            return loadGood;
        }
        if (++attempts >= kSelfHealAttempts) {
            return loadGood;
        }
    }
}

BaseObject* IdleBarrier::ReadStaticRef(RootSlot& field) const { return Barrier::ReadStaticRef(field); }

BaseObject* IdleBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    return ReadReference(obj, field);
}

BaseObject* IdleBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    // TRUST_STATE_KILL_PLAN Phase 1: retire TryUntagRefField plain-CAS from the read path.
    // Match Forward/Preforward/PostTrace: load-good test + make_load_good + observed-raw CAS
    // self-heal with current colour (not heap-slot untag-to-plain).
    for (int attempts = 0;;) {
        RefField<false> oldField(field.GetFieldValue(order));
        BaseObject* oldTarget = to_object(oldField.GetTargetObject());
        if (oldTarget == nullptr || LIKELY(theCollector.is_load_good(oldField))) {
            DLOG(BARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()),
                 oldTarget);
            return oldTarget;
        }

        BaseObject* loadGood = theCollector.make_load_good(oldField);
        // relroroot / rostatic: non-heap targets under GNU_RELRO — skip colour CAS write-back.
        if (loadGood != nullptr && !Heap::IsHeapAddress(loadGood)) {
            return loadGood;
        }
        RefField<> goodField = theCollector.GetAndTryTagRefField(loadGood);
        if (field.CompareExchange(oldField.GetFieldValue(), goodField.GetFieldValue())) {
            DLOG(BARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()),
                 loadGood);
            return loadGood;
        }
        if (++attempts >= kSelfHealAttempts) {
            return loadGood;
        }
    }
}

void IdleBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    if (obj != nullptr) {
        // Heap-src self-heal stays on the heap field; non-heap dst is StorePlain below.
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
    FixupNonHeapStructRefs(dst, obj, src, size);
}

void IdleBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK,
                 "read struct memcpy_s failed");
    if (!Heap::IsHeapAddress(dst)) {
        FixupNonHeapStaticStructRefs(dst, src, size, gctib);
        return;
    }
    gctib.ForEachBitmapWord(dst, [=](RefField<>& field) {
        BaseObject* target = ReadReference(nullptr, field);
        (void)target;
    });
}

void IdleBarrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                       MemoryOrder order) const
{
    RefField<> coloured = theCollector.GetAndTryTagRefField(newRef);
    if (obj != nullptr) {
        DLOG(BARRIER, "atomic write obj %p<%p>(%zu) ref@%p: %p", obj, obj->GetTypeInfo(), obj->GetSize(), &field,
             newRef);
    } else {
        DLOG(BARRIER, "atomic write static ref@%p: %p", &field, newRef);
    }
    field.StoreColoured(coloured.GetFieldValue(), order);
}

BaseObject* IdleBarrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                             MemoryOrder order) const
{
    // newRef must be the latest versions; colour it so the slot never holds plain.
    RefField<> coloured = theCollector.GetAndTryTagRefField(newRef);
    MAddress oldValue = raw(field.Exchange(coloured.GetFieldValue(), order));
    RefField<> oldField(oldValue);
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    DLOG(BARRIER, "atomic swap obj %p<%p>(%zu) ref@%p: old %#zx(%p), new %#zx(%p)", obj, obj->GetTypeInfo(),
         obj->GetSize(), &field, oldValue, oldRef, raw(field.GetFieldValue(order)), newRef);
    return oldRef;
}

bool IdleBarrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                          BaseObject* newRef, MemoryOrder sOrder, MemoryOrder fOrder) const
{
    // R8：expected 已是 observed-raw；新值上规范色（模板 EnumBarrier.cpp:262-269）。
    // ⛔ R1 base CAS 假失配由 casfix 独占，本处不碰 Barrier.cpp:217-229。
    MAddress oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
    RefField<false> oldField(oldFieldValue);
    BaseObject* oldVersion = ReadReference(nullptr, oldField);

    // oldRef and newRef must be the latest versions. Bound kCasAttempts: colour self-heal
    // can keep the raw expected bits moving while identity stays oldRef (c3179214).
    for (int attempt = 0; attempt < kCasAttempts && oldVersion == oldRef; ++attempt) {
        RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
        if (field.CompareExchange(to_zpointer(oldFieldValue), newField.GetFieldValue(), sOrder, fOrder)) {
            return true;
        }
        oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
        RefField<false> tmp(oldFieldValue);
        oldVersion = ReadReference(nullptr, tmp);
    }
    return false;
}

void IdleBarrier::WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    DLOG(BARRIER, "write obj %p ref@%p: %p => %p", obj, &field, to_object(field.GetTargetObject()), ref);
    // R3 人口最大：Idle 墙钟写一律规范色。
    RefField<> newField = theCollector.GetAndTryTagRefField(ref);
    field.StoreColoured(newField.GetFieldValue());
}

void IdleBarrier::WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    // R9 bulk：Idle 墙钟 memcpy 灌 plain；post-copy 补色环 = PostTraceBarrier.cpp:117-128。
    CHECK(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK);
    if (obj != nullptr) {
        obj->ForEachRefInStruct(
            [=](RefField<>& refField) {
                RefField<> oldField(refField);
                MAddress oldValue = raw(oldField.GetFieldValue());
                BaseObject* latest = ReadReference(nullptr, oldField);
                RefField<> newField = theCollector.GetAndTryTagRefField(latest);
                if (oldValue != raw(newField.GetFieldValue())) {
                    refField.CompareExchange(to_zpointer(oldValue), newField.GetFieldValue());
                }
            },
            dst, dst + dstLen);
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    CHECK(srcLen == dstLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

void IdleBarrier::WriteStaticRef(RootSlot& field, BaseObject* ref) const
{
    Barrier::WriteStaticRef(field, ref);
}

void IdleBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const
{
    // R9：静态槽 barrier 可见，不能只走 WriteStructImpl(nullptr)（那边 obj==null 跳过补色）。
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
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

void IdleBarrier::CopyRefArrayImpl(BaseObject* dstObj, MAddress dst, MIndex dstSize, BaseObject* srcObj, MAddress src,
                               MIndex srcSize) const
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (!dstObj->HasRefField()) {
        LOG(RTLOG_FATAL, "array %p doesn't have class-type element\n", dstObj);
        return;
    }
    if (!static_cast<MArray*>(dstObj)->GetComponentTypeInfo()->IsObjectType() &&
        !static_cast<MArray*>(dstObj)->GetComponentTypeInfo()->IsInterface() &&
        !static_cast<MArray*>(dstObj)->GetComponentTypeInfo()->IsArrayType()) {
        LOG(RTLOG_FATAL, "array %p type is not class", dstObj);
        return;
    }
#endif
    if (dst == src) {
        return;
    }
    bool inHeap = Heap::IsHeapAddress(dstObj);
    if (dst < src) {
        MAddress currentDst = dst;
        MAddress currentSrc = src;
        MAddress fieldBound = dst + dstSize;
        while (currentDst < fieldBound) {
            HeapSlot<false>& currentDstField = HeapSlotAt<false>(currentDst);
            HeapSlot<false>& currentSrcField = HeapSlotAt<false>(currentSrc);
            BaseObject* newRef = ReadReference(srcObj, currentSrcField);
            if (inHeap) {
                WriteReference(dstObj, currentDstField, newRef);
            } else {
                StorePlain(RootSlotAt(currentDst), from_object(newRef));
            }
            currentDst += sizeof(RefField<false>);
            currentSrc += sizeof(RefField<false>);
        }
    } else {
        MAddress currentDst = dst + dstSize - sizeof(RefField<>);
        MAddress currentSrc = src + srcSize - sizeof(RefField<>);
        MAddress fieldBound = dst;
        while (currentDst >= fieldBound) {
            HeapSlot<false>& currentDstField = HeapSlotAt<false>(currentDst);
            HeapSlot<false>& currentSrcField = HeapSlotAt<false>(currentSrc);
            BaseObject* newRef = ReadReference(srcObj, currentSrcField);
            if (inHeap) {
                WriteReference(dstObj, currentDstField, newRef);
            } else {
                StorePlain(RootSlotAt(currentDst), from_object(newRef));
            }
            currentDst -= sizeof(RefField<false>);
            currentSrc -= sizeof(RefField<false>);
        }
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstSize);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcSize);
#endif
}

void IdleBarrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
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
    MArray* srcArray = static_cast<MArray*>(srcObj);
    RefFieldVisitor srcVisitor = [this, srcArray](RefField<false>& field) { (void)ReadReference(srcArray, field); };
    srcArray->ForEachRefFieldInRange(srcVisitor, srcField, srcField + srcSize);

    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) ==
                     EOK,
                 "memmove_s failed");

    // R9 bulk：堆 dst 上 memmove 后补规范色（栈源恒 plain）。非堆 dst = Y5。
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
