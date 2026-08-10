// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Barrier.inline.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/IdleEdgeDiag.h"
#include "Heap/Verify/RemsetPhaseProbe.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
namespace {
#if defined(MRT_GENERATIONAL_BARRIER_PROBE)
std::atomic<uint64_t> generationalBarrierFastPathHits { 0 };
std::atomic<uint64_t> generationalBarrierRegionLookups { 0 };
#endif

inline bool HasYoungRegionsForRecording()
{
    bool hasYoungRegions = RegionInfo::HasYoungRegions();
#if defined(MRT_GENERATIONAL_BARRIER_PROBE)
    if (hasYoungRegions) {
        generationalBarrierRegionLookups.fetch_add(1, std::memory_order_relaxed);
    } else {
        generationalBarrierFastPathHits.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    return hasYoungRegions;
}
} // namespace

#if defined(MRT_GENERATIONAL_BARRIER_PROBE)
void Barrier::ResetGenerationalBarrierProbe()
{
    generationalBarrierFastPathHits.store(0, std::memory_order_relaxed);
    generationalBarrierRegionLookups.store(0, std::memory_order_relaxed);
}

uint64_t Barrier::GetGenerationalBarrierFastPathHits()
{
    return generationalBarrierFastPathHits.load(std::memory_order_relaxed);
}

uint64_t Barrier::GetGenerationalBarrierRegionLookups()
{
    return generationalBarrierRegionLookups.load(std::memory_order_relaxed);
}
#endif

void Barrier::WriteI8(BaseObject* obj, Field<int8_t>& field, int8_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteI16(BaseObject* obj, Field<int16_t>& field, int16_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteI32(BaseObject* obj, Field<int32_t>& field, int32_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteI64(BaseObject* obj, Field<int64_t>& field, int64_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteF32(BaseObject* obj, Field<float>& field, float val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteF64(BaseObject* obj, Field<double>& field, double val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    WriteReferenceImpl(obj, field, ref);
    RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
}

void Barrier::WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    DLOG(BARRIER, "write obj %p ref-field@%p: %p => %p", obj, &field, to_object(field.GetTargetObject()), ref);
    // COLOUR_WRITEBACK_AUDIT R3/批 A：规范色写回，禁 plain 灌堆。
    RefField<> newField = theCollector.GetAndTryTagRefField(ref);
    field.StoreColoured(newField.GetFieldValue());
}

void Barrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    WriteStructImpl(obj, dst, dstLen, src, srcLen);
    RecordCrossGenEdgesInStruct(obj, dst, dstLen);
}

void Barrier::WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    // R9 bulk：memcpy 会把栈上 plain 整块灌进堆；post-copy 补色环模板 = PostTraceBarrier.cpp:117-128。
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK,
                 "memcpy_s failed");
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
    CHECK_EQ(srcLen, dstLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

void Barrier::WriteStaticRef(RootSlot& field, BaseObject* ref) const
{
    DLOG(BARRIER, "write (barrier) static ref@%p: %p", &field, ref);
    StorePlain(field, from_object(ref));
    // Static/global slots are visited and fixed as roots in every minor collection.
    // RecordCrossGenEdge retains a validation-only coverage oracle for this path.
    RecordCrossGenEdge(nullptr, reinterpret_cast<MAddress>(&field), ref);
}

void Barrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const
{
    // R9 bulk：静态槽 barrier 可见；post-copy 补色 = EnumBarrier.cpp:192-200。
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK,
                 "memcpy_s failed");
    gctib.ForEachBitmapWord(dst, [=](RefField<>& refField) {
        RefField<> oldField(refField);
        MAddress oldValue = raw(oldField.GetFieldValue());
        BaseObject* untagged = ReadReference(nullptr, oldField);
        RefField<> newField = theCollector.GetAndTryTagRefField(untagged);
        if (oldValue != raw(newField.GetFieldValue())) {
            refField.CompareExchange(to_zpointer(oldValue), newField.GetFieldValue());
        }
    });
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstLen < srcLen ? dstLen : srcLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), copyLen);
#endif
    RecordStaticCrossGenEdges(dst, gctib);
}

BaseObject* Barrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    BaseObject* toVersion = nullptr;
    if (theCollector.TryUpdateRefField(obj, field, toVersion)) {
        return toVersion;
    } else {
        BaseObject* target = to_object(field.GetTargetObject());
        return target;
    }
}

BaseObject* Barrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    BaseObject* toVersion = nullptr;
    if (theCollector.TryUpdateRefField(obj, field, toVersion)) {
        return toVersion;
    } else {
        BaseObject* target = to_object(field.GetTargetObject());
        return target;
    }
}

BaseObject* Barrier::ReadStaticRef(RootSlot& field) const
{
    zaddress_unsafe observed = field.LoadPlain();
    if (is_null(observed)) {
        return nullptr;
    }
    // Decode any legacy colour bits at an external ABI boundary without exposing
    // the RootSlot storage as a HeapSlot.
    HeapSlot<> observedBits(to_zpointer(raw(observed)));
    BaseObject* target = to_object(observedBits.GetTargetObject());
    if (target != nullptr && Heap::IsHeapAddress(target) && theCollector.IsGhostFromObject(target)) {
        target = theCollector.FindLatestVersion(target);
        HealRoot(field, from_object(target));
    }
    return target;
}

// barrier for atomic operation.
void Barrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const
{
    AtomicWriteReferenceImpl(obj, field, ref, order);
    RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
}

void Barrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const
{
    RefField<> newField = theCollector.GetAndTryTagRefField(ref);
    if (obj != nullptr) {
        DLOG(BARRIER, "atomic write obj %p<%p>(%zu) ref@%p: %#zx -> %p", obj, obj->GetTypeInfo(), obj->GetSize(),
             &field, raw(field.GetFieldValue()), ref);
    } else {
        DLOG(BARRIER, "atomic write static ref@%p: %#zx -> %p", &field, raw(field.GetFieldValue()), ref);
    }

    field.StoreColoured(newField.GetFieldValue(), order);
}

BaseObject* Barrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                         MemoryOrder order) const
{
    BaseObject* oldRef = AtomicSwapReferenceImpl(obj, field, newRef, order);
    RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
    return oldRef;
}

BaseObject* Barrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                             MemoryOrder order) const
{
    RefField<> coloured = theCollector.GetAndTryTagRefField(newRef);
    MAddress oldValue = raw(field.Exchange(coloured.GetFieldValue(), order));
    RefField<> oldField(oldValue);
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    DLOG(BARRIER, "atomic swap obj %p<%p>(%zu) ref-field@%p: old %#zx(%p), new %#zx(%p)", obj, obj->GetTypeInfo(),
         obj->GetSize(), &field, oldValue, oldRef, raw(field.GetFieldValue()), newRef);
    return oldRef;
}

BaseObject* Barrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    RefField<false> tmpField(field.GetFieldValue(order));
    if (theCollector.IsOldPointer(tmpField)) {
        BaseObject* toVersion = ReadReference(nullptr, tmpField);
        // R10：治愈写也必须带规范色，禁 plain SetTargetObject。
        RefField<> healed = theCollector.GetAndTryTagRefField(toVersion);
        field.StoreColoured(healed.GetFieldValue());
        DLOG(BARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(tmpField.GetFieldValue()), toVersion);
        return toVersion;
    }

    BaseObject* target = to_object(tmpField.GetTargetObject());
    DLOG(BARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(tmpField.GetFieldValue()), target);
    return target;
}

bool Barrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                      MemoryOrder succOrder, MemoryOrder failOrder) const
{
    bool success = CompareAndSwapReferenceImpl(obj, field, oldRef, newRef, succOrder, failOrder);
    if (success) {
        RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
    }
    return success;
}

bool Barrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                          BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    // Compare on decoded object identity; CAS on observed raw bits (colour-aware).
    // Shape matches EnumBarrier.cpp:259-280 / IdleBarrier.cpp:121-138. Plain expected vs
    // coloured slot bits always fail (COLOUR_WRITEBACK_AUDIT R1).
    // Retries are bounded (kCasAttempts in ColourMask.h). ZGC terminates a self-healing
    // retry because its colours form a monotone lattice; ours do not -- a reader may
    // self-heal this very slot on every load, so the observed bits keep changing while the
    // decoded identity stays oldRef, and an unbounded loop never lands the exchange.
    // natural_wave spun 47 minutes of user time in two spinning threads before this bound
    // existed. Exhausting the budget reports failure, which CAS callers already handle.
    MAddress oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
    RefField<false> oldField(oldFieldValue);
    BaseObject* oldVersion = ReadReference(nullptr, oldField);

    for (int attempt = 0; attempt < kCasAttempts && oldVersion == oldRef; ++attempt) {
        // Recolour per attempt: a phase may flip mid-retry, and writing last epoch's colour
        // would hand the next reader a value its mask calls bad.
        RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
        if (field.CompareExchange(to_zpointer(oldFieldValue), newField.GetFieldValue(), succOrder, failOrder)) {
            DLOG(BARRIER, "cas 1 for obj %p reffield@%p: old %#zx->%p, expect %p, new %p", obj, &field,
                 oldFieldValue, oldVersion, oldRef, newRef);
            return true;
        }
        oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
        RefField<false> tmp(oldFieldValue);
        oldVersion = ReadReference(nullptr, tmp);
    }
    DLOG(BARRIER, "cas 0 for obj %p reffield@%p: old %#zx->%p, expect %p, new %p", obj, &field, oldFieldValue,
         oldVersion, oldRef, newRef);
    return false;
}

void Barrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj, MAddress srcField,
                           MIndex srcSize) const
{
    CopyRefArrayImpl(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
    RecordCrossGenEdgesInRefArray(dstObj, dstField, dstSize);
}

void Barrier::CopyRefArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                               MAddress srcField, MIndex srcSize) const
{
    (void)srcObj;
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) ==
                     EOK,
                 "memmove_s failed");
    // R9：堆 dst 上 memmove 可能灌入栈 plain；逐槽补色。非堆 dst = Y5 保持 plain。
    // heap→heap 已有色时 GetAndTryTagRefField 幂等（Y6 不新增 plain，补色也无害）。
    if (dstObj != nullptr && Heap::IsHeapAddress(dstObj)) {
        MAddress end = dstField + dstSize;
        for (MAddress cur = dstField; cur + sizeof(HeapSlot<>) <= end; cur += sizeof(HeapSlot<>)) {
            HeapSlot<>& refField = HeapSlotAt<>(cur);
            RefField<> oldField(refField);
            MAddress oldValue = raw(oldField.GetFieldValue());
            BaseObject* latest = ReadReference(nullptr, oldField);
            RefField<> newField = theCollector.GetAndTryTagRefField(latest);
            if (oldValue != raw(newField.GetFieldValue())) {
                refField.CompareExchange(to_zpointer(oldValue), newField.GetFieldValue());
            }
        }
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

void Barrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                              MAddress srcField, MIndex srcSize) const
{
    CopyStructArrayImpl(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
    RecordCrossGenEdgesInStruct(dstObj, dstField, dstSize);
}

void Barrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                  MAddress srcField, MIndex srcSize) const
{
    (void)srcObj;
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) ==
                     EOK,
                 "memmove_s failed");
    // R9 bulk：struct 数组 memmove 后对堆 dst 引用槽补色（模板 PostTrace WriteStruct post-copy）。
    if (dstObj != nullptr && dstObj->HasRefField() && Heap::IsHeapAddress(dstObj)) {
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
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

void Barrier::FixupNonHeapStructRefs(MAddress dst, BaseObject* srcObj, MAddress src, size_t size) const
{
    // Heap→heap must keep coloured slots; only non-heap (stack sret / root buffer) goes plain.
    if (srcObj == nullptr || Heap::IsHeapAddress(dst)) {
        return;
    }
    srcObj->ForEachRefInStruct(
        [this, srcObj, dst, src, size](RefField<false>& field) {
            MAddress fieldAddr = reinterpret_cast<MAddress>(&field);
            if (fieldAddr < src || fieldAddr >= (src + size)) {
                return;
            }
            // ReadReference may self-heal the heap source (colour stays on heap).
            BaseObject* target = ReadReference(srcObj, field);
            StorePlain(RootSlotAt(dst + (fieldAddr - src)), from_object(target));
        },
        src, src + size);
}

void Barrier::FixupNonHeapStaticStructRefs(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    if (Heap::IsHeapAddress(dst)) {
        return;
    }
    gctib.ForEachBitmapWordInRange(
        src,
        [this, dst, src](RefField<>& srcField) {
            MAddress offset = reinterpret_cast<MAddress>(&srcField) - src;
            BaseObject* target = ReadReference(nullptr, srcField);
            StorePlain(RootSlotAt(dst + offset), from_object(target));
        },
        src, src + size);
}

void Barrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    size_t dstSize = size;
    size_t srcSize = size;
    if (obj != nullptr) {
        obj->ForEachRefInStruct(
            [this, obj](RefField<false>& field) {
                // MAddress bias = reinterpret_cast<MAddress>(&field) - reinterpret_cast<MAddress>(src);
                // The destination reference slot starts at dst + bias.
                BaseObject* fromVersion = to_object(field.GetTargetObject());
                (void)fromVersion;
                BaseObject* toVersion = nullptr;
                theCollector.TryUpdateRefField(obj, field, toVersion);
            },
            src, src + size);
    }

    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstSize, reinterpret_cast<void*>(src), srcSize) == EOK,
                 "read struct memcpy_s failed");
    // Non-heap dst: overwrite ref slots with plain (STACK_ROOTS_STAY_PLAIN).
    FixupNonHeapStructRefs(dst, obj, src, size);
}

void Barrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    size_t dstSize = size;
    size_t srcSize = size;
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstSize, reinterpret_cast<void*>(src), srcSize) == EOK,
                 "read struct memcpy_s failed");
    if (!Heap::IsHeapAddress(dst)) {
        FixupNonHeapStaticStructRefs(dst, src, size, gctib);
        return;
    }
    gctib.ForEachBitmapWord(dst, [this](RefField<>& refField) {
        BaseObject* toVersion = nullptr;
        theCollector.TryUpdateRefField(nullptr, refField, toVersion);
    });
}

void Barrier::WriteGeneric(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
{
    WriteGenericImpl(obj, fieldPtr, src, size);
    RecordCrossGenEdgesInStruct(obj, reinterpret_cast<MAddress>(fieldPtr), size);
}

void Barrier::WriteGenericImpl(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
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
    } else if ((Heap::IsHeapAddress(obj) && !Heap::IsHeapAddress(src))||
        (Heap::IsHeapAddress(obj) && Heap::IsHeapAddress(src))) {
        MAddress dstAddr = reinterpret_cast<MAddress>(fieldPtr);
        MAddress srcAddr = reinterpret_cast<MAddress>(src) + TYPEINFO_PTR_SIZE;
        WriteStruct(obj, dstAddr, size, srcAddr, size);
    }
}
void Barrier::ReadGeneric(const ObjectPtr dstObj, ObjectPtr obj, void* fieldPtr, size_t size) const
{
    ReadGenericImpl(dstObj, obj, fieldPtr, size);
    if (Heap::IsHeapAddress(dstObj)) {
        RecordCrossGenEdgesInStruct(dstObj, reinterpret_cast<MAddress>(dstObj) + TYPEINFO_PTR_SIZE, size);
    }
}

void Barrier::ReadGenericImpl(const ObjectPtr dstObj, ObjectPtr obj, void* fieldPtr, size_t size) const
{
    if (!Heap::IsHeapAddress(dstObj) && !Heap::IsHeapAddress(obj)) {
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(dstObj) + TYPEINFO_PTR_SIZE),
                              size, fieldPtr, size) == EOK,
                     "ReadGeneric memcpy_s failed");
    } else if (!Heap::IsHeapAddress(dstObj) && Heap::IsHeapAddress(obj)) {
        MAddress dstAddr = reinterpret_cast<MAddress>(dstObj) + TYPEINFO_PTR_SIZE;
        MAddress srcAddr = reinterpret_cast<MAddress>(fieldPtr);
        ReadStruct(dstAddr, obj, srcAddr, size);
    } else if ((Heap::IsHeapAddress(dstObj) && !Heap::IsHeapAddress(obj))||
        (Heap::IsHeapAddress(dstObj) && Heap::IsHeapAddress(obj))) {
        MAddress dstAddr = reinterpret_cast<MAddress>(dstObj) + TYPEINFO_PTR_SIZE;
        MAddress srcAddr = reinterpret_cast<MAddress>(fieldPtr);
        WriteStruct(dstObj, dstAddr, size, srcAddr, size);
    }
}

void Barrier::RecordCrossGenEdge(BaseObject* obj, MAddress fieldAddress, BaseObject* ref) const
{
    using namespace RemsetPhaseProbe;
    // promoteedge gen codes: 0=unknown 1=young 2=old 3=nonheap
    constexpr uint8_t kGenUnknown = 0;
    constexpr uint8_t kGenYoung = 1;
    constexpr uint8_t kGenOld = 2;
    constexpr uint8_t kGenNonHeap = 3;
    auto genOfAddr = [](MAddress addr) -> uint8_t {
        if (addr == 0 || !Heap::IsHeapAddress(addr)) {
            return kGenNonHeap;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(addr);
        if (region == nullptr) {
            return kGenUnknown;
        }
        return region->IsYoungRegion() ? kGenYoung : kGenOld;
    };
    // idlewrite: also stamp object-header gen so field-addr vs obj mismatch is visible.
    const uint8_t holderObjGen =
        (obj != nullptr && Heap::IsHeapAddress(obj)) ? genOfAddr(reinterpret_cast<MAddress>(obj)) : kGenUnknown;
    const bool probeOn = Enabled();
    const bool idleEdgeOn = IdleEdgeDiag::Enabled();
    const bool forceRecord = ForceRecordEnabled();
    GCPhase phase = GCPhase::GC_PHASE_UNDEF;
    if (probeOn || idleEdgeOn) {
        phase = Heap::GetHeap().GetGCPhase();
    }

    if (!HasYoungRegionsForRecording() && !forceRecord) {
        if (probeOn) {
            NoteWrite(fieldAddress, phase, REASON_NO_YOUNG, false);
        }
        if (idleEdgeOn) {
            IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, genOfAddr(fieldAddress),
                                              genOfAddr(reinterpret_cast<MAddress>(ref)),
                                              static_cast<uint8_t>(REASON_NO_YOUNG), holderObjGen);
        }
        return;
    }
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        if (probeOn) {
            NoteWrite(fieldAddress, phase, REASON_REF_NULL_OR_NONHEAP, false);
        }
        if (idleEdgeOn) {
            IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, genOfAddr(fieldAddress), kGenNonHeap,
                                              static_cast<uint8_t>(REASON_REF_NULL_OR_NONHEAP), holderObjGen);
        }
        return;
    }
    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(ref));
    if (!targetRegion->IsYoungRegion()) {
        if (probeOn) {
            NoteWrite(fieldAddress, phase, REASON_REF_NOT_YOUNG, false);
        }
        if (idleEdgeOn) {
            IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, genOfAddr(fieldAddress), kGenOld,
                                              static_cast<uint8_t>(REASON_REF_NOT_YOUNG), holderObjGen);
        }
        return;
    }
    // Heap holder: only record old→young (source not young).
    if (Heap::IsHeapAddress(fieldAddress)) {
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
            if (probeOn) {
                NoteWrite(fieldAddress, phase, REASON_HOLDER_NULL_OR_NONHEAP, false);
            }
            if (idleEdgeOn) {
                IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, kGenUnknown, kGenYoung,
                                                  static_cast<uint8_t>(REASON_HOLDER_NULL_OR_NONHEAP), holderObjGen);
            }
            return;
        }
        RegionInfo* sourceRegion = RegionInfo::GetRegionInfoAt(fieldAddress);
        if (sourceRegion->IsYoungRegion()) {
            if (probeOn) {
                NoteWrite(fieldAddress, phase, REASON_HOLDER_YOUNG, false);
            }
            if (idleEdgeOn) {
                IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, kGenYoung, kGenYoung,
                                                  static_cast<uint8_t>(REASON_HOLDER_YOUNG), holderObjGen);
            }
            return;
        }
        theRememberedSet.Record(fieldAddress);
        if (probeOn) {
            NoteWrite(fieldAddress, phase, REASON_RECORDED, true);
        }
        if (idleEdgeOn) {
            IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, true, kGenOld, kGenYoung,
                                              static_cast<uint8_t>(REASON_RECORDED), holderObjGen);
        }
        return;
    }
    // Non-heap field (static/global/value temporary): it cannot consume a
    // heap-region bitmap bit. Retain exact slot identity in the separately locked
    // external double buffer.
    (void)obj;
    theRememberedSet.RecordExternal(fieldAddress);
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    theRememberedSet.RecordStaticForCrossCheck(
        fieldAddress, reinterpret_cast<MAddress>(__builtin_return_address(0)));
#endif
    if (probeOn) {
        NoteWrite(fieldAddress, phase, REASON_HOLDER_NULL_OR_NONHEAP, false);
    }
    if (idleEdgeOn) {
        IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, kGenNonHeap, kGenYoung,
                                          static_cast<uint8_t>(REASON_HOLDER_NULL_OR_NONHEAP), holderObjGen);
    }
}

void Barrier::RecordCrossGenEdgesInStruct(BaseObject* obj, MAddress start, size_t size) const
{
    if (!HasYoungRegionsForRecording()) {
        return;
    }
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return;
    }
    obj->ForEachRefInStruct(
        [this, obj](RefField<>& field) {
            RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
        },
        start, start + size);
}

void Barrier::RecordCrossGenEdgesInRefArray(BaseObject* obj, MAddress start, size_t size) const
{
    if (!HasYoungRegionsForRecording()) {
        return;
    }
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return;
    }
    MAddress end = start + size;
    for (MAddress current = start; current + sizeof(HeapSlot<>) <= end; current += sizeof(HeapSlot<>)) {
        HeapSlot<>& field = HeapSlotAt<>(current);
        RecordCrossGenEdge(obj, current, to_object(field.GetTargetObject()));
    }
}

void Barrier::RecordStaticCrossGenEdges(MAddress start, const GCTib gctib) const
{
    if (!HasYoungRegionsForRecording()) {
        return;
    }
    gctib.ForEachBitmapWord(start, [this](RefField<>& field) {
        RecordCrossGenEdge(nullptr, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
    });
}

} // namespace MapleRuntime
