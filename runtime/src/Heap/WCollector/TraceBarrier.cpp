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
    for (;;) {
        RefField<> oldField(field);
        BaseObject* oldTarget = to_object(oldField.GetTargetObject());
        if (oldTarget == nullptr ||
            LIKELY(!IsPlainNonNullSlotWord(static_cast<uintptr_t>(raw(oldField.GetFieldValue()))) &&
                   theCollector.is_load_good(oldField))) {
            BaseObject* resolved = ResolveFromCopyForMutator(oldTarget);
            if (resolved == oldTarget || resolved == nullptr) {
                return resolved;
            }
            if (!Heap::IsHeapAddress(resolved)) {
                return resolved;
            }
            RefField<> goodField = theCollector.GetAndTryTagRefField(resolved);
            return ZgcSelfHealLoadGood(field, oldField.GetFieldValue(), goodField.GetFieldValue(),
                                       HealSite::TraceReadReference);
        }

        BaseObject* loadGood = theCollector.make_load_good(oldField);
        // relroroot / rostatic: non-heap targets (static constants under GNU_RELRO) are never
        // evacuated. Colouring + CAS into those slots faults (si_code=2 ACCERR). Skip write-back.
        if (loadGood != nullptr && !Heap::IsHeapAddress(loadGood)) {
            return loadGood;
        }
        loadGood = ResolveFromCopyForMutator(loadGood);
        if (loadGood == nullptr) {
            return nullptr;
        }
        RefField<> goodField = theCollector.GetAndTryTagRefField(loadGood);
        // OpenJDK ZBarrier::self_heal (zBarrier.inline.hpp:72-107): the exact observed value is
        // the CAS expected value. A concurrent GC update therefore wins rather than being
        // overwritten; on failure, reload and apply the barrier to the newer value.
        return ZgcSelfHealLoadGood(field, oldField.GetFieldValue(), goodField.GetFieldValue(),
                                   HealSite::TraceReadReference);
    }
}

BaseObject* TraceBarrier::ReadStaticRef(RootSlot& field) const { return Barrier::ReadStaticRef(field); }

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
    if (!Heap::IsHeapAddress(dst)) {
        CopyStructPlainToNonHeap(dst, obj, src, size);
        return;
    }
    CHECK(obj != nullptr);
    CopyObjectStructColouredToHeap(obj, src, dst, size, src, size);
}

void TraceBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    if (!Heap::IsHeapAddress(dst)) {
        CopyStaticStructPlainToNonHeap(dst, src, size, gctib);
        return;
    }
    CopyStaticStructColouredToHeap(dst, size, src, size, gctib);
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
    // The paired StoreBarrierBuffer producer in Barrier::WriteReference owns
    // retirement of the overwritten value. Keeping a direct SATB enqueue here
    // would retire the same previous value twice during TRACE.
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
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    RecordCrossGenEdge(nullptr, reinterpret_cast<MAddress>(&field), ref);
#endif
}

void TraceBarrier::WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    CHECK(obj != nullptr);
    if (obj != nullptr) {
        MRT_ASSERT(dst > reinterpret_cast<MAddress>(obj), "WriteStruct struct addr is less than obj!");
        Mutator* mutator = Mutator::GetMutator();
        // Barrier::WriteStruct snapshots every overwritten field before this
        // implementation runs. Its paired buffer is the sole old-value
        // producer for heap struct writes during TRACE.
        obj->ForEachRefInStruct(
            [=](RefField<>& refField) {
                MAddress offset = reinterpret_cast<MAddress>(&refField) - dst;
                RefField<> srcField(HeapSlotAt<>(src + offset));
                RememberNewReference(mutator, ReadReference(nullptr, srcField));
            },
            dst, dst + srcLen);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    CopyObjectStructColouredToHeap(obj, dst, dst, dstLen, src, srcLen);

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
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    gctib.ForEachBitmapWord(dst, [this](RefField<>& field) {
        RecordCrossGenEdge(nullptr, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
    });
#endif
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
        target = ResolveFromCopyForMutator(to_object(oldField.GetTargetObject()));
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
    Mutator* mutator = Mutator::GetMutator();
    RememberNewReference(mutator, newRef);
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    field.StoreColoured(newField.GetFieldValue(), order);
    // Barrier::AtomicWriteReference captured oldField before dispatch and owns
    // retirement through the paired store buffer.
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
    // The wrapper captured the same pre-exchange word for paired retirement.
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
        if (HealSlot(field, to_zpointer(oldFieldValue), newField.GetFieldValue(),
                     HealSite::TraceCompareAndSwapReference, HealNull::Allow, succOrder, failOrder)) {
            // Successful CAS is retired once by Barrier::CompareAndSwapReference.
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
    if (dstObj == nullptr || !Heap::IsHeapAddress(dstObj)) {
        CopyStructArrayPlainToNonHeap(dstField, srcObj, srcField, srcSize);
        return;
    }
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
            HealSlot(field, oldField.GetFieldValue(), newField.GetFieldValue(),
                     HealSite::TraceCopyStructArrayRecolour);
        }
    };
    MArray* srcArray = static_cast<MArray*>(srcObj);
    srcArray->ForEachRefFieldInRange(srcVisitor, srcField, srcField + srcSize);

    // Barrier::CopyStructArray captured each destination word before dispatch;
    // the paired buffer is the sole old-value producer for this heap write.

    CopyStructArrayColouredToHeap(dstObj, dstField, dstSize, srcField, srcSize);

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), dstSize);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
}

void TraceBarrier::WriteGenericImpl(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
{
    ObjectPtr dst = obj;
    void* fp = fieldPtr;
    dst = RelocateHolderForWrite(dst, fp);
    ObjectPtr from = ResolveFromCopyForMutator(src);
    NoteZeroTip(dst, "TraceBarrier.WriteGenericImpl");
    if ((dst != nullptr && !dst->HasRefField()) || (!Heap::IsHeapAddress(dst) && !Heap::IsHeapAddress(from))) {
        CHECK_DETAIL(memcpy_s(fp, size,
                              reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(from) + TYPEINFO_PTR_SIZE),
                              size) == EOK,
                     "WriteGeneric memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        if (Heap::IsHeapAddress(from)) {
            Sanitizer::TsanReadMemoryRange(
                reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(from) + TYPEINFO_PTR_SIZE), size);
        }
        if (Heap::IsHeapAddress(dst)) {
            Sanitizer::TsanWriteMemoryRange(fp, size);
        }
#endif
    } else if (!Heap::IsHeapAddress(dst) && Heap::IsHeapAddress(from)) {
        MAddress dstAddr = reinterpret_cast<MAddress>(fp);
        MAddress srcAddr = reinterpret_cast<MAddress>(from) + TYPEINFO_PTR_SIZE;
        ReadStruct(dstAddr, from, srcAddr, size);
    } else if ((Heap::IsHeapAddress(dst) && !Heap::IsHeapAddress(from))) {
        MAddress dstAddr = reinterpret_cast<MAddress>(fp);
        MAddress srcAddr = reinterpret_cast<MAddress>(from) + TYPEINFO_PTR_SIZE;
        WriteStruct(dst, dstAddr, size, srcAddr, size);
    } else {
        MAddress dstAddr = reinterpret_cast<MAddress>(fp);
        MAddress srcAddr = reinterpret_cast<MAddress>(from) + TYPEINFO_PTR_SIZE;
        void* tmp = malloc(size);
        ReadStruct((MAddress)tmp, from, srcAddr, size);
        WriteStruct(dst, dstAddr, size, (MAddress)tmp, size);
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
