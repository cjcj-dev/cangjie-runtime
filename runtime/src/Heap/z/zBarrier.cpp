// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Base/Macros.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace MapleRuntime {
#include "Heap/Barrier/BarrierTestObservations.h"
static_assert(!std::is_polymorphic<ZBarrier>::value, "Barrier must not regain virtual dispatch");

// ZZBarrier::assert_transition_monotonicity, zBarrier.inline.hpp:40-70.
void AssertBarrierTransitionMonotonicity(zpointer oldPtr, zpointer newPtr)
{
    const uintptr_t oldRaw = raw(oldPtr), newRaw = raw(newPtr);
    CHECK(!ZPointer::is_load_good(to_zpointer(oldRaw)) ||
          ZPointer::is_load_good(to_zpointer(newRaw)));
    CHECK(!ZPointer::is_mark_good(to_zpointer(oldRaw)) ||
          ZPointer::is_mark_good(to_zpointer(newRaw)));
    CHECK(!ZPointer::is_store_good(to_zpointer(oldRaw)) ||
          ZPointer::is_store_good(to_zpointer(newRaw)));
    if (!(!is_null_any(to_zpointer(newRaw)))) {
        return;
    }
    CHECK(!ZPointer::is_marked_young(to_zpointer(oldRaw)) ||
          ZPointer::is_marked_young(to_zpointer(newRaw)));
    CHECK(!ZPointer::is_marked_old(to_zpointer(oldRaw)) ||
          ZPointer::is_marked_old(to_zpointer(newRaw)));
    CHECK(!ZPointer::is_marked_finalizable(to_zpointer(oldRaw)) ||
          ZPointer::is_marked_finalizable(to_zpointer(newRaw)) ||
          ZPointer::is_marked_old(to_zpointer(newRaw)));
}

namespace {
enum class CopySlotKind { Heap, Native, Uncolored };

        void CopyReferenceSlots(
                             MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                             std::vector<size_t> offsets, CopySlotKind sourceKind, CopySlotKind destinationKind)
{
    CHECK_DETAIL(srcLen <= dstLen, "full-colour copy source does not fit: dstLen=%zu srcLen=%zu", dstLen, srcLen);
    if (srcLen == 0) {
        return;
    }
    std::vector<uint8_t> snapshot(srcLen);
    CHECK_DETAIL(memcpy_s(snapshot.data(), snapshot.size(), reinterpret_cast<void*>(src), srcLen) == EOK,
                 "full-colour source snapshot failed");
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    size_t cursor = 0;
    for (size_t offset : offsets) {
        CHECK_DETAIL(offset >= cursor && offset + sizeof(HeapSlot<>) <= srcLen,
                     "full-colour ref offset outside copy: offset=%zu cursor=%zu srcLen=%zu", offset, cursor, srcLen);
        if (offset > cursor) {
            const size_t gap = offset - cursor;
            CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst + cursor), gap,
                                  snapshot.data() + cursor, gap) == EOK,
                         "full-colour primitive-gap copy failed");
        }
        uintptr_t sourceWord = 0;
        std::memcpy(&sourceWord, snapshot.data() + offset, sizeof(sourceWord));
        BaseObject* target;
        if (sourceKind == CopySlotKind::Uncolored) {
            // Mutator-local values are load-good after the shared root protocol.
            // They are not zpointer words and must never enter a color fast path.
            target = to_object(safe(to_zaddress_unsafe(sourceWord)));
        } else {
            HeapSlot<> source(to_zpointer(sourceWord));
            target = sourceKind == CopySlotKind::Native
                ? ZBarrier::ReadStaticRef(source) : ZBarrier::ReadReference(nullptr, source);
        }
        if (destinationKind == CopySlotKind::Heap) {
            ZBarrier::WriteReference(nullptr, HeapSlotAt<>(dst + offset), target);
        } else if (destinationKind == CopySlotKind::Native) {
            ZBarrier::WriteStaticRef(NativeSlotAt(dst + offset), target);
        } else {
            StorePlain(RootSlotAt(dst + offset), from_object(target));
        }
        cursor = offset + sizeof(HeapSlot<>);
    }
    if (cursor < srcLen) {
        const size_t tail = srcLen - cursor;
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst + cursor), tail,
                              snapshot.data() + cursor, tail) == EOK,
                     "full-colour primitive-tail copy failed");
    }
}
} // namespace

// Value-type ABI entries carry their GC layout even when the optional holder
// is null. Process the same slots as object-layout copies without guessing a base.
void ZBarrier::WriteStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, GCTib gctib)
{
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(src, [&offsets, src](RefField<>& field) {
        offsets.push_back(reinterpret_cast<MAddress>(&field) - src);
    }, src, src + srcLen);
    CopyReferenceSlots( dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Heap);
}

void ZBarrier::ReadStruct(MAddress dst, MAddress src, size_t size, GCTib gctib)
{
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(src, [&offsets, src](RefField<>& field) {
        offsets.push_back(reinterpret_cast<MAddress>(&field) - src);
    }, src, src + size);
    CopyReferenceSlots( dst, size, src, size, std::move(offsets),
        CopySlotKind::Heap, CopySlotKind::Uncolored);
}

// ZZBarrier::store_barrier_on_heap_oop_field, zBarrier.inline.hpp:695-706.
// Atomic operations heal before attempting the exchange, ordinary stores buffer prev.
template<bool atomic>
void ZBarrier::StoreBarrier(BaseObject* obj, RefField<atomic>& field, bool heal,
                            ReferenceStrength strength)
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    RefField<> previous(observed);
    auto fastPath = [heal, strength](zpointer word) {
        RefField<> value(word);
        return ZPointer::is_store_good(value.GetFieldValue()) ||
            (strength == ReferenceStrength::Strong && !heal && is_null(word));
    };
    if (fastPath(observed)) {
        return;
    }
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, obj, &field };
    BaseObject* target = Heap::GetHeap().GetCollector().make_load_good(previous, provenance);
    if (strength != ReferenceStrength::Strong) {
        // ZZBarrier::no_keep_alive_heap_store_slow_path (zBarrier.cpp:266-270).
        const MAddress address = reinterpret_cast<MAddress>(&field);
        if (Heap::IsHeapAddress(address) && !RegionInfo::GetRegionInfoAt(address)->IsYoungRegion()) {
            Heap::GetHeap().GetRememberedSet().Record(address, true);
        }
        return;
    }
    if (heal) {
        // ZZBarrier::heap_store_slow_path(..., heal=true) does not buffer.
        Heap::GetHeap().GetCollector().MarkObjectIfActive(target);
        const MAddress address = reinterpret_cast<MAddress>(&field);
        if (Heap::IsHeapAddress(address) && !RegionInfo::GetRegionInfoAt(address)->IsYoungRegion()) {
            Heap::GetHeap().GetRememberedSet().Record(address, true);
        }
        const zpointer good = to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(target)))));
        ZgcSelfHeal(field, observed, good, fastPath, HealSite::BarrierReadReference);
    } else {
        RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), target, observed);
    }
}

void ZBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref)
{
    const bool weakReferent = obj != nullptr && Heap::IsHeapAddress(obj) && obj->IsWeakRef() &&
        reinterpret_cast<MAddress>(&field) == reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    StoreBarrier(obj, field, false, weakReferent ? ReferenceStrength::Weak : ReferenceStrength::Strong);
    WriteReferenceImpl(obj, field, ref);
}

void ZBarrier::PostWriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref, zpointer prev)
{
    RefField<> previous(prev);
    const MAddress address = reinterpret_cast<MAddress>(&field);
    const bool weakReferent = obj != nullptr && Heap::IsHeapAddress(obj) && obj->IsWeakRef() &&
        address == reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    // ZZBarrier::no_keep_alive_store_barrier_on_heap_oop_field uses store-good,
    // including raw null in the slow path so that remember(p) is not skipped.
    if (!ZPointer::is_store_good(previous.GetFieldValue()) && (weakReferent || !is_null(prev))) {
        if (weakReferent) {
            if (!RegionInfo::GetRegionInfoAt(address)->IsYoungRegion()) {
                Heap::GetHeap().GetRememberedSet().Record(address, true);
            }
        } else {
            RecordCrossGenEdge(obj, address, ref, prev);
        }
    }
}

void ZBarrier::WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref)
{
    field.StoreColoured(to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(ref))))));
}

void ZBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen)
{
    WriteStructImpl(obj, dst, dstLen, src, srcLen);
}

void ZBarrier::WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen)
{

    if (obj != nullptr && Heap::IsHeapAddress(obj)) {
        CopyObjectStructColouredToHeap(obj, dst, dst, dstLen, src, srcLen);
    } else {
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK,
                     "plain non-heap struct copy failed");
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    CHECK_EQ(srcLen, dstLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

// ZZBarrier::store_barrier_on_native_oop_field, zBarrier.inline.hpp:709.
// Native slots carry color but have no heap remembered-set obligation.
template<bool atomic>
void ZBarrier::NativeStoreBarrier(RefField<atomic>& field, bool heal)
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    auto fastPath = [heal](zpointer word) {
        RefField<> value(word);
        return ZPointer::is_store_good(value.GetFieldValue()) || (!heal && is_null(word));
    };
    if (fastPath(observed)) {
        return;
    }
    RefField<> previous(observed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &field };
    BaseObject* target = Heap::GetHeap().GetCollector().make_load_good(previous, provenance);
    Heap::GetHeap().GetCollector().MarkObjectIfActive(target);
    if (heal) {
        const zpointer good = to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(target)))));
        ZgcSelfHeal(field, observed, good, fastPath, HealSite::BarrierReadReference);
    }
}

void ZBarrier::WriteStaticRef(NativeSlot& field, BaseObject* ref)
{
    NativeStoreBarrier(field, false);
    WriteReferenceImpl(nullptr, field, ref);
}

BaseObject* ZBarrier::ReadStaticRef(NativeSlot& field)
{
    const zpointer observed = field.GetFieldValue();
    return LoadBarrier(nullptr, field, observed, ReferenceStrength::Strong);
}

// ZZBarrier::mark_from_young_slow_path, zBarrier.cpp:158-183.
zaddress ZBarrier::MarkFromYoungSlowPath(zaddress address)
{
    auto& young = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG);
    CHECK(young.IsPhaseMark());
    if (is_null(address)) return address;
    if (RegionInfo::GetRegionInfoAt(raw(address))->IsYoungRegion()) {
        young.MarkObject<false, true, true, false>(address);
        return address;
    }
    if (young.IsMajorRoots()) {
        Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD).MarkObject<false, true, true, false>(address);
        return address;
    }
    return address;
}

// ZZBarrier::mark_from_old_slow_path, zBarrier.cpp:185-203.
zaddress ZBarrier::MarkFromOldSlowPath(zaddress address)
{
    auto& old = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD);
    CHECK(old.IsPhaseMark());
    if (is_null(address)) return address;
    if (!RegionInfo::GetRegionInfoAt(raw(address))->IsYoungRegion()) {
        old.MarkObject<false, true, true, false>(address);
        return address;
    }
    return zaddress::null;
}

// ZZBarrier::mark_finalizable_slow_path, zBarrier.cpp:218-232.
zaddress ZBarrier::MarkFinalizableSlowPath(zaddress address)
{
    auto& old = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD);
    auto& young = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG);
    CHECK(old.IsPhaseMark() || young.IsPhaseMark());
    if (is_null(address)) return address;
    if (!RegionInfo::GetRegionInfoAt(raw(address))->IsYoungRegion()) {
        old.MarkObject<false, true, true, true>(address);
        return address;
    }
    young.MarkObjectIfActive<false, true, true, false>(address);
    return address;
}

// ZZBarrier::mark_finalizable_from_old_slow_path, zBarrier.cpp:234-250.
zaddress ZBarrier::MarkFinalizableFromOldSlowPath(zaddress address)
{
    auto& old = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD);
    CHECK(old.IsPhaseMark() || Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG).IsPhaseMark());
    if (is_null(address)) return address;
    if (!RegionInfo::GetRegionInfoAt(raw(address))->IsYoungRegion()) {
        old.MarkObject<false, true, true, true>(address);
        return address;
    }
    return zaddress::null;
}

// ZZBarrier::mark_young_slow_path, zBarrier.cpp:206-215.
zaddress ZBarrier::MarkYoungSlowPath(zaddress address)
{
    if (is_null(address)) {
        return address;
    }
    MarkIfYoung(address);
    return address;
}

void ZBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib)
{
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(src, [&offsets, src](RefField<>& field) {
        offsets.push_back(reinterpret_cast<MAddress>(&field) - src);
    }, src, src + srcLen);
    CopyReferenceSlots( dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Native);
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), srcLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

// ZZBarrier::barrier and weak/phantom slow paths, zBarrier.inline.hpp:319-343,484-565.
template<bool atomic>
BaseObject* ZBarrier::LoadBarrier(BaseObject* obj, RefField<atomic>& field, zpointer observed,
                                 ReferenceStrength strength)
{
    auto fastPath = [strength](zpointer word) {
        RefField<> value(word);
        return strength == ReferenceStrength::Strong
            ? ZPointer::is_load_good_or_null(to_zpointer(raw(word)))
            : ZPointer::is_mark_good(value.GetFieldValue());
    };
    RefField<> value(observed);
    if (fastPath(observed)) {
        return to_object(value.GetTargetObject());
    }
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, obj, &field };
    BaseObject* target = Heap::GetHeap().GetCollector().make_load_good(value, provenance);
    CHECK_DETAIL(target != nullptr || !(!is_null_any(to_zpointer(raw(observed)))),
                 "load barrier relocation must preserve a non-null reference");
    if (strength != ReferenceStrength::Strong && target != nullptr && Heap::IsHeapAddress(target)) {
        // Only the shared resurrection rendezvous publishes the blocked window.
        if (Heap::GetHeap().GetCollectorResources().IsResurrectionBlocked()) {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (region->IsYoungRegion()) {
                Heap::GetHeap().GetCollector().MarkYoungObjectIfActive(target);
            } else {
                const zaddress targetAddr = from_object(target);
                const bool stronglyLive = region->is_object_strongly_live(targetAddr);
                const bool live = stronglyLive || (strength == ReferenceStrength::Phantom &&
                                                   region->is_object_live(targetAddr));
                if (!live) {
                    return nullptr; // A load never clears a referent (ZZBarrier::self_heal).
                }
            }
        } else {
            Heap::GetHeap().GetCollector().MarkObjectIfActive(target);
        }
    }
    const zpointer healed = strength == ReferenceStrength::Strong
        ? ZAddress::load_good(from_object(target), observed)
        : ZAddress::mark_good(from_object(target), observed);
    ZgcSelfHeal(field, observed, healed, fastPath, HealSite::BarrierReadReference);
    return target;
}

BaseObject* ZBarrier::ReadReference(BaseObject* obj, RefField<false>& field)
{
    return LoadBarrier(obj, field, field.GetFieldValue(), ReferenceStrength::Strong);
}

BaseObject* ZBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field)
{
    return LoadBarrier(obj, field, field.GetFieldValue(), ReferenceStrength::Weak);
}

BaseObject* ZBarrier::ReadPhantomRef(BaseObject* obj, RefField<false>& field)
{
    return LoadBarrier(obj, field, field.GetFieldValue(), ReferenceStrength::Phantom);
}

// barrier for atomic operation.
void ZBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order)
{
    if (!Heap::IsHeapAddress(&field)) {
        NativeStoreBarrier(field, true);
        AtomicWriteReferenceImpl(obj, field, ref, order);
        return;
    }
    StoreBarrier(obj, field, true);
    AtomicWriteReferenceImpl(obj, field, ref, order);
}

void ZBarrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order)
{
    field.StoreColoured(to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(ref))))), order);
}

BaseObject* ZBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                         MemoryOrder order)
{
    if (!Heap::IsHeapAddress(&field)) {
        NativeStoreBarrier(field, true);
        return AtomicSwapReferenceImpl(obj, field, newRef, order);
    }
    StoreBarrier(obj, field, true);
    return AtomicSwapReferenceImpl(obj, field, newRef, order);
}

BaseObject* ZBarrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                             MemoryOrder order)
{
    const zpointer desired = to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(newRef)))));
    RefField<> previous(field.Exchange(desired, order));
    return to_object(previous.GetTargetObject());
}

BaseObject* ZBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order)
{
    return LoadBarrier(obj, field, field.GetFieldValue(order), ReferenceStrength::Strong);
}

bool ZBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                      MemoryOrder succOrder, MemoryOrder failOrder)
{
    if (!Heap::IsHeapAddress(&field)) {
        NativeStoreBarrier(field, true);
        return CompareAndSwapReferenceImpl(obj, field, oldRef, newRef, succOrder, failOrder);
    }
    StoreBarrier(obj, field, true);
    return CompareAndSwapReferenceImpl(obj, field, oldRef, newRef, succOrder, failOrder);
}

bool ZBarrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                          BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder)
{
    const zpointer expected = to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(oldRef)))));
    const zpointer desired = to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(newRef)))));
    return HealSlot(field, expected, desired, HealSite::BarrierCompareAndSwapReference,
                    HealNull::Allow, succOrder, failOrder);
}

void ZBarrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj, MAddress srcField,
                           MIndex srcSize)
{
    CopyRefArrayImpl(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
}

void ZBarrier::CopyRefArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                               MAddress srcField, MIndex srcSize)
{

    if (dstObj == nullptr || !Heap::IsHeapAddress(dstObj)) {
        CopyRefArrayPlainToNonHeap(dstField, srcObj, srcField, dstSize, srcSize);
        return;
    }
    (void)srcObj;
    CopyRefArrayColouredToHeap(dstField, dstSize, srcField, srcSize);
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

void ZBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                              MAddress srcField, MIndex srcSize)
{
    CopyStructArrayImpl(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
}

void ZBarrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                  MAddress srcField, MIndex srcSize)
{

    if (dstObj == nullptr || !Heap::IsHeapAddress(dstObj)) {
        CopyStructArrayPlainToNonHeap(dstField, srcObj, srcField, srcSize);
        return;
    }
    (void)srcObj;
    CopyStructArrayColouredToHeap(dstObj, dstField, dstSize, srcField, srcSize);
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

void ZBarrier::CopyStructPlainToNonHeap(MAddress dst, BaseObject* srcObj, MAddress src, size_t size)
{
    // STACK_ROOTS_STAY_PLAIN: never memcpy a coloured heap word onto the stack.
    // Walk GC pointer slots in address order, copy the primitive gap, then StorePlain.
    CHECK(!Heap::IsHeapAddress(dst));
    if (size == 0) {
        return;
    }
    if (!Heap::IsHeapAddress(src) && dst < src + size && src < dst + size) {
        CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK,
                     "read struct overlap memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), size);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), size);
#endif
        return;
    }
    MAddress cursor = src;
    const MAddress srcEnd = src + size;
    if (srcObj != nullptr) {
        srcObj->ForEachRefInStruct(
            [srcObj, dst, src, srcEnd, &cursor](RefField<false>& field) {
                MAddress fieldAddr = reinterpret_cast<MAddress>(&field);
                if (fieldAddr < cursor || fieldAddr >= srcEnd) {
                    return;
                }
                if (fieldAddr > cursor) {
                    size_t gap = static_cast<size_t>(fieldAddr - cursor);
                    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst + (cursor - src)), gap,
                                          reinterpret_cast<void*>(cursor), gap) == EOK,
                                 "read struct gap memcpy_s failed");
                }
                BaseObject* target = Heap::IsHeapAddress(&field) ? ReadReference(srcObj, field)
                : to_object(safe(RootSlotAt(static_cast<void*>(&field)).LoadPlain()));
                StorePlain(RootSlotAt(dst + (fieldAddr - src)), from_object(target));
                cursor = fieldAddr + sizeof(RefField<>);
            },
            src, srcEnd);
    }
    if (cursor < srcEnd) {
        size_t tail = static_cast<size_t>(srcEnd - cursor);
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst + (cursor - src)), tail,
                              reinterpret_cast<void*>(cursor), tail) == EOK,
                     "read struct tail memcpy_s failed");
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), size);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), size);
#endif
}



void ZBarrier::CopyObjectStructColouredToHeap(BaseObject* layoutObj, MAddress layoutStart,
                                              MAddress dst, size_t dstLen,
                                              MAddress src, size_t srcLen)
{
    CHECK(layoutObj != nullptr && Heap::IsHeapAddress(dst));
    std::vector<size_t> offsets;
    layoutObj->ForEachRefInStruct(
        [&offsets, layoutStart, srcLen](RefField<>& field) {
            MAddress address = reinterpret_cast<MAddress>(&field);
            if (address >= layoutStart && address + sizeof(HeapSlot<>) <= layoutStart + srcLen) {
                offsets.push_back(static_cast<size_t>(address - layoutStart));
            }
        }, layoutStart, layoutStart + srcLen);
    CopyReferenceSlots( dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Heap);
}

void ZBarrier::CopyStaticStructColouredToHeap(MAddress dst, size_t dstLen, MAddress src,
                                              size_t srcLen, const GCTib gctib)
{
    CHECK(Heap::IsHeapAddress(dst));
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(
        src,
        [&offsets, src](RefField<>& field) {
            offsets.push_back(static_cast<size_t>(reinterpret_cast<MAddress>(&field) - src));
        }, src, src + srcLen);
    CopyReferenceSlots( dst, dstLen, src, srcLen, std::move(offsets),
        CopySlotKind::Native, CopySlotKind::Heap);
}

void ZBarrier::CopyStructArrayColouredToHeap(BaseObject* dstObj, MAddress dst, size_t dstLen,
                                             MAddress src, size_t srcLen)
{
    CHECK(dstObj != nullptr && Heap::IsHeapAddress(dstObj));
    std::vector<size_t> offsets;
    static_cast<MArray*>(dstObj)->ForEachRefFieldInRange(
        [&offsets, dst](RefField<>& field) {
            offsets.push_back(static_cast<size_t>(reinterpret_cast<MAddress>(&field) - dst));
        }, dst, dst + srcLen);
    CopyReferenceSlots( dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Heap);
}

void ZBarrier::CopyRefArrayColouredToHeap(MAddress dst, size_t dstLen, MAddress src, size_t srcLen)
{
    CHECK(Heap::IsHeapAddress(dst));
    CHECK_DETAIL(srcLen <= dstLen && srcLen % sizeof(HeapSlot<>) == 0,
                 "full-colour ref-array copy shape invalid: dstLen=%zu srcLen=%zu", dstLen, srcLen);
    std::vector<size_t> offsets;
    for (size_t offset = 0; offset < srcLen; offset += sizeof(HeapSlot<>)) {
        offsets.push_back(offset);
    }
    CopyReferenceSlots( dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Heap);
}

void ZBarrier::CopyStaticStructPlainToNonHeap(MAddress dst, MAddress src, size_t size, const GCTib gctib)
{
    CHECK(!Heap::IsHeapAddress(dst));
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(src, [&offsets, src](RefField<>& field) {
        offsets.push_back(reinterpret_cast<MAddress>(&field) - src);
    }, src, src + size);
    CopyReferenceSlots( dst, size, src, size, std::move(offsets),
                       CopySlotKind::Native, CopySlotKind::Uncolored);
}

void ZBarrier::CopyStructArrayPlainToNonHeap(MAddress dstField, BaseObject* srcObj, MAddress srcField,
                                           size_t srcSize)
{
    CHECK(!Heap::IsHeapAddress(dstField));
    if (srcSize == 0) {
        return;
    }
    if (!Heap::IsHeapAddress(srcField) && dstField < srcField + srcSize && srcField < dstField + srcSize) {
        CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), srcSize,
                               reinterpret_cast<void*>(srcField), srcSize) == EOK,
                     "copy struct array overlap memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), srcSize);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
        return;
    }
    if (srcObj == nullptr) {
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dstField), srcSize,
                              reinterpret_cast<void*>(srcField), srcSize) == EOK,
                     "copy struct array plain memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), srcSize);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
        return;
    }
    MAddress cursor = srcField;
    const MAddress srcEnd = srcField + srcSize;
    static_cast<MArray*>(srcObj)->ForEachRefFieldInRange(
        [srcObj, dstField, srcField, srcEnd, &cursor](RefField<false>& field) {
            MAddress fieldAddr = reinterpret_cast<MAddress>(&field);
            if (fieldAddr < cursor || fieldAddr >= srcEnd) {
                return;
            }
            if (fieldAddr > cursor) {
                size_t gap = static_cast<size_t>(fieldAddr - cursor);
                CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dstField + (cursor - srcField)), gap,
                                      reinterpret_cast<void*>(cursor), gap) == EOK,
                             "copy struct array gap memcpy_s failed");
            }
            BaseObject* target = Heap::IsHeapAddress(&field) ? ReadReference(srcObj, field)
                : to_object(safe(RootSlotAt(static_cast<void*>(&field)).LoadPlain()));
            StorePlain(RootSlotAt(dstField + (fieldAddr - srcField)), from_object(target));
            cursor = fieldAddr + sizeof(RefField<>);
        },
        srcField, srcEnd);
    if (cursor < srcEnd) {
        size_t tail = static_cast<size_t>(srcEnd - cursor);
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dstField + (cursor - srcField)), tail,
                              reinterpret_cast<void*>(cursor), tail) == EOK,
                     "copy struct array tail memcpy_s failed");
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), srcSize);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
}

void ZBarrier::CopyRefArrayPlainToNonHeap(MAddress dst, BaseObject* srcObj, MAddress src, MIndex dstSize,
                                         MIndex srcSize)
{
    CHECK(!Heap::IsHeapAddress(dst));
    if (dst == src) {
        return;
    }
    const size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    if (copyLen == 0) {
        return;
    }
    if (dst < src) {
        MAddress currentDst = dst;
        MAddress currentSrc = src;
        MAddress fieldBound = dst + copyLen;
        while (currentDst < fieldBound) {
            HeapSlot<false>& currentSrcField = HeapSlotAt<false>(currentSrc);
            BaseObject* newRef = Heap::IsHeapAddress(currentSrc) ? ReadReference(srcObj, currentSrcField)
                : to_object(safe(RootSlotAt(currentSrc).LoadPlain()));
            StorePlain(RootSlotAt(currentDst), from_object(newRef));
            currentDst += sizeof(RefField<false>);
            currentSrc += sizeof(RefField<false>);
        }
    } else {
        MAddress currentDst = dst + copyLen - sizeof(RefField<>);
        MAddress currentSrc = src + copyLen - sizeof(RefField<>);
        MAddress fieldBound = dst;
        while (currentDst >= fieldBound) {
            HeapSlot<false>& currentSrcField = HeapSlotAt<false>(currentSrc);
            BaseObject* newRef = Heap::IsHeapAddress(currentSrc) ? ReadReference(srcObj, currentSrcField)
                : to_object(safe(RootSlotAt(currentSrc).LoadPlain()));
            StorePlain(RootSlotAt(currentDst), from_object(newRef));
            currentDst -= sizeof(RefField<false>);
            currentSrc -= sizeof(RefField<false>);
        }
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), copyLen);
#endif
}

void ZBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size)
{

    if (!Heap::IsHeapAddress(dst)) {
        CopyStructPlainToNonHeap(dst, obj, src, size);
        return;
    }
    CHECK(obj != nullptr);
    CopyObjectStructColouredToHeap(obj, src, dst, size, src, size);
}

void ZBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib)
{

    if (!Heap::IsHeapAddress(dst)) {
        CopyStaticStructPlainToNonHeap(dst, src, size, gctib);
        return;
    }
    CopyStaticStructColouredToHeap(dst, size, src, size, gctib);
}

void ZBarrier::WriteGeneric(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size)
{
    WriteGenericImpl(obj, fieldPtr, src, size);
}

void ZBarrier::WriteGenericImpl(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size)
{
    ObjectPtr dst = obj;
    void* fp = fieldPtr;
    ObjectPtr from = src;

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
    } else if ((Heap::IsHeapAddress(dst) && !Heap::IsHeapAddress(from))||
        (Heap::IsHeapAddress(dst) && Heap::IsHeapAddress(from))) {
        MAddress dstAddr = reinterpret_cast<MAddress>(fp);
        MAddress srcAddr = reinterpret_cast<MAddress>(from) + TYPEINFO_PTR_SIZE;
        WriteStruct(dst, dstAddr, size, srcAddr, size);
    }
}
void ZBarrier::ReadGeneric(const ObjectPtr dstObj, ObjectPtr obj, void* fieldPtr, size_t size)
{
    ReadGenericImpl(dstObj, obj, fieldPtr, size);
}

void ZBarrier::ReadGenericImpl(const ObjectPtr dstObj, ObjectPtr obj, void* fieldPtr, size_t size)
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

void ZBarrier::RecordCrossGenEdge(BaseObject* obj, MAddress fieldAddress, BaseObject* ref, zpointer prev)
{
    (void)obj;
    (void)ref;
    StoreBarrierBuffer* buffer = StoreBarrierBuffer::buffer_for_store(false);
    if (buffer != nullptr) {
        buffer->Add(fieldAddress, prev, Heap::GetHeap().GetRememberedSet());
        return;
    }
    mark_and_remember(reinterpret_cast<volatile zpointer*>(fieldAddress), make_load_good(prev));
}

void ZBarrier::store_barrier_on_heap_oop_field(volatile zpointer* p, bool heal)
{
    auto& field = *reinterpret_cast<RefField<false>*>(const_cast<zpointer*>(p));
    StoreBarrier<false>(nullptr, field, heal);
}

void ZBarrier::store_barrier_on_native_oop_field(volatile zpointer* p, bool heal)
{
    auto& field = *reinterpret_cast<NativeSlot*>(const_cast<zpointer*>(p));
    NativeStoreBarrier<false>(field, heal);
}

} // namespace MapleRuntime
