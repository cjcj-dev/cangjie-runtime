// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zBarrier.inline.hpp"
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
static_assert(!std::is_polymorphic<Barrier>::value, "Barrier must not regain virtual dispatch");

// ZBarrier::assert_transition_monotonicity, zBarrier.inline.hpp:40-70.
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

void CopyReferenceSlots(const Barrier& barrier,
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
                ? barrier.ReadStaticRef(source) : barrier.ReadReference(nullptr, source);
        }
        if (destinationKind == CopySlotKind::Heap) {
            barrier.WriteReference(nullptr, HeapSlotAt<>(dst + offset), target);
        } else if (destinationKind == CopySlotKind::Native) {
            barrier.WriteStaticRef(NativeSlotAt(dst + offset), target);
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
void Barrier::WriteStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, GCTib gctib) const
{
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(src, [&offsets, src](RefField<>& field) {
        offsets.push_back(reinterpret_cast<MAddress>(&field) - src);
    }, src, src + srcLen);
    CopyReferenceSlots(*this, dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Heap);
}

void Barrier::ReadStruct(MAddress dst, MAddress src, size_t size, GCTib gctib) const
{
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(src, [&offsets, src](RefField<>& field) {
        offsets.push_back(reinterpret_cast<MAddress>(&field) - src);
    }, src, src + size);
    CopyReferenceSlots(*this, dst, size, src, size, std::move(offsets),
        CopySlotKind::Heap, CopySlotKind::Uncolored);
}

// ZBarrier::store_barrier_on_heap_oop_field, zBarrier.inline.hpp:695-706.
// Atomic operations heal before attempting the exchange, ordinary stores buffer prev.
template<bool atomic>
void Barrier::StoreBarrier(BaseObject* obj, RefField<atomic>& field, bool heal,
                            ReferenceStrength strength) const
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    RefField<> previous(observed);
    auto fastPath = [this, heal, strength](zpointer word) {
        RefField<> value(word);
        return ZPointer::is_store_good(value.GetFieldValue()) ||
            (strength == ReferenceStrength::Strong && !heal && is_null(word));
    };
    if (fastPath(observed)) {
        return;
    }
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, obj, &field };
    BaseObject* target = theCollector.make_load_good(previous, provenance);
    if (strength != ReferenceStrength::Strong) {
        // ZBarrier::no_keep_alive_heap_store_slow_path (zBarrier.cpp:266-270).
        const MAddress address = reinterpret_cast<MAddress>(&field);
        if (Heap::IsHeapAddress(address) && !RegionInfo::GetRegionInfoAt(address)->IsYoungRegion()) {
            theRememberedSet.Record(address, true);
        }
        return;
    }
    if (heal) {
        // ZBarrier::heap_store_slow_path(..., heal=true) does not buffer.
        theCollector.MarkObjectIfActive(target);
        const MAddress address = reinterpret_cast<MAddress>(&field);
        if (Heap::IsHeapAddress(address) && !RegionInfo::GetRegionInfoAt(address)->IsYoungRegion()) {
            theRememberedSet.Record(address, true);
        }
        const zpointer good = to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(target)))));
        ZgcSelfHeal(field, observed, good, fastPath, HealSite::BarrierReadReference);
    } else {
        RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), target, observed);
    }
}

void Barrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    const bool weakReferent = obj != nullptr && Heap::IsHeapAddress(obj) && obj->IsWeakRef() &&
        reinterpret_cast<MAddress>(&field) == reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    StoreBarrier(obj, field, false, weakReferent ? ReferenceStrength::Weak : ReferenceStrength::Strong);
    WriteReferenceImpl(obj, field, ref);
}

void Barrier::PostWriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref, zpointer prev) const
{
    RefField<> previous(prev);
    const MAddress address = reinterpret_cast<MAddress>(&field);
    const bool weakReferent = obj != nullptr && Heap::IsHeapAddress(obj) && obj->IsWeakRef() &&
        address == reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    // ZBarrier::no_keep_alive_store_barrier_on_heap_oop_field uses store-good,
    // including raw null in the slow path so that remember(p) is not skipped.
    if (!ZPointer::is_store_good(previous.GetFieldValue()) && (weakReferent || !is_null(prev))) {
        if (weakReferent) {
            if (!RegionInfo::GetRegionInfoAt(address)->IsYoungRegion()) {
                theRememberedSet.Record(address, true);
            }
        } else {
            RecordCrossGenEdge(obj, address, ref, prev);
        }
    }
}

void Barrier::WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    field.StoreColoured(to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(ref))))));
}

void Barrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    WriteStructImpl(obj, dst, dstLen, src, srcLen);
}

void Barrier::WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
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

// ZBarrier::store_barrier_on_native_oop_field, zBarrier.inline.hpp:709.
// Native slots carry color but have no heap remembered-set obligation.
template<bool atomic>
void Barrier::NativeStoreBarrier(RefField<atomic>& field, bool heal) const
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    auto fastPath = [this, heal](zpointer word) {
        RefField<> value(word);
        return ZPointer::is_store_good(value.GetFieldValue()) || (!heal && is_null(word));
    };
    if (fastPath(observed)) {
        return;
    }
    RefField<> previous(observed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &field };
    BaseObject* target = theCollector.make_load_good(previous, provenance);
    theCollector.MarkObjectIfActive(target);
    if (heal) {
        const zpointer good = to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(target)))));
        ZgcSelfHeal(field, observed, good, fastPath, HealSite::BarrierReadReference);
    }
}

void Barrier::WriteStaticRef(NativeSlot& field, BaseObject* ref) const
{
    NativeStoreBarrier(field, false);
    WriteReferenceImpl(nullptr, field, ref);
}

BaseObject* Barrier::ReadStaticRef(NativeSlot& field) const
{
    const zpointer observed = field.GetFieldValue();
    return LoadBarrier(nullptr, field, observed, ReferenceStrength::Strong);
}

// ZBarrier::mark_young_slow_path, zBarrier.cpp:206-215.
zaddress Barrier::MarkYoungSlowPath(zaddress address) const
{
    if (is_null(address)) {
        return address;
    }
    MarkIfYoung(address);
    return address;
}

void Barrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const
{
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(src, [&offsets, src](RefField<>& field) {
        offsets.push_back(reinterpret_cast<MAddress>(&field) - src);
    }, src, src + srcLen);
    CopyReferenceSlots(*this, dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Native);
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), srcLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

// ZBarrier::barrier and weak/phantom slow paths, zBarrier.inline.hpp:319-343,484-565.
template<bool atomic>
BaseObject* Barrier::LoadBarrier(BaseObject* obj, RefField<atomic>& field, zpointer observed,
                                 ReferenceStrength strength) const
{
    auto fastPath = [this, strength](zpointer word) {
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
    BaseObject* target = theCollector.make_load_good(value, provenance);
    CHECK_DETAIL(target != nullptr || !(!is_null_any(to_zpointer(raw(observed)))),
                 "load barrier relocation must preserve a non-null reference");
    if (strength != ReferenceStrength::Strong && target != nullptr && Heap::IsHeapAddress(target)) {
        // Only the shared resurrection rendezvous publishes the blocked window.
        if (Heap::GetHeap().GetCollectorResources().IsResurrectionBlocked()) {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (region->IsYoungRegion()) {
                theCollector.MarkYoungObjectIfActive(target);
            } else {
                const zaddress targetAddr = from_object(target);
                const bool stronglyLive = region->is_object_strongly_live(targetAddr);
                const bool live = stronglyLive || (strength == ReferenceStrength::Phantom &&
                                                   region->is_object_live(targetAddr));
                if (!live) {
                    return nullptr; // A load never clears a referent (ZBarrier::self_heal).
                }
            }
        } else {
            theCollector.MarkObjectIfActive(target);
        }
    }
    const zpointer healed = strength == ReferenceStrength::Strong
        ? ZAddress::load_good(from_object(target), observed)
        : ZAddress::mark_good(from_object(target), observed);
    ZgcSelfHeal(field, observed, healed, fastPath, HealSite::BarrierReadReference);
    return target;
}

BaseObject* Barrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    return LoadBarrier(obj, field, field.GetFieldValue(), ReferenceStrength::Strong);
}

BaseObject* Barrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    return LoadBarrier(obj, field, field.GetFieldValue(), ReferenceStrength::Weak);
}

BaseObject* Barrier::ReadPhantomRef(BaseObject* obj, RefField<false>& field) const
{
    return LoadBarrier(obj, field, field.GetFieldValue(), ReferenceStrength::Phantom);
}

// barrier for atomic operation.
void Barrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const
{
    if (!Heap::IsHeapAddress(&field)) {
        NativeStoreBarrier(field, true);
        AtomicWriteReferenceImpl(obj, field, ref, order);
        return;
    }
    StoreBarrier(obj, field, true);
    AtomicWriteReferenceImpl(obj, field, ref, order);
}

void Barrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const
{
    field.StoreColoured(to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(ref))))), order);
}

BaseObject* Barrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                         MemoryOrder order) const
{
    if (!Heap::IsHeapAddress(&field)) {
        NativeStoreBarrier(field, true);
        return AtomicSwapReferenceImpl(obj, field, newRef, order);
    }
    StoreBarrier(obj, field, true);
    return AtomicSwapReferenceImpl(obj, field, newRef, order);
}

BaseObject* Barrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                             MemoryOrder order) const
{
    const zpointer desired = to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(newRef)))));
    RefField<> previous(field.Exchange(desired, order));
    return to_object(previous.GetTargetObject());
}

BaseObject* Barrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    return LoadBarrier(obj, field, field.GetFieldValue(order), ReferenceStrength::Strong);
}

bool Barrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                      MemoryOrder succOrder, MemoryOrder failOrder) const
{
    if (!Heap::IsHeapAddress(&field)) {
        NativeStoreBarrier(field, true);
        return CompareAndSwapReferenceImpl(obj, field, oldRef, newRef, succOrder, failOrder);
    }
    StoreBarrier(obj, field, true);
    return CompareAndSwapReferenceImpl(obj, field, oldRef, newRef, succOrder, failOrder);
}

bool Barrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                          BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    const zpointer expected = to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(oldRef)))));
    const zpointer desired = to_zpointer(raw(ZAddress::store_good(to_zaddress(reinterpret_cast<uintptr_t>(newRef)))));
    return HealSlot(field, expected, desired, HealSite::BarrierCompareAndSwapReference,
                    HealNull::Allow, succOrder, failOrder);
}

void Barrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj, MAddress srcField,
                           MIndex srcSize) const
{
    CopyRefArrayImpl(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
}

void Barrier::CopyRefArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                               MAddress srcField, MIndex srcSize) const
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

void Barrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                              MAddress srcField, MIndex srcSize) const
{
    CopyStructArrayImpl(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
}

void Barrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                  MAddress srcField, MIndex srcSize) const
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

void Barrier::CopyStructPlainToNonHeap(MAddress dst, BaseObject* srcObj, MAddress src, size_t size) const
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
            [this, srcObj, dst, src, srcEnd, &cursor](RefField<false>& field) {
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



void Barrier::CopyObjectStructColouredToHeap(BaseObject* layoutObj, MAddress layoutStart,
                                              MAddress dst, size_t dstLen,
                                              MAddress src, size_t srcLen) const
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
    CopyReferenceSlots(*this, dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Heap);
}

void Barrier::CopyStaticStructColouredToHeap(MAddress dst, size_t dstLen, MAddress src,
                                              size_t srcLen, const GCTib gctib) const
{
    CHECK(Heap::IsHeapAddress(dst));
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(
        src,
        [&offsets, src](RefField<>& field) {
            offsets.push_back(static_cast<size_t>(reinterpret_cast<MAddress>(&field) - src));
        }, src, src + srcLen);
    CopyReferenceSlots(*this, dst, dstLen, src, srcLen, std::move(offsets),
        CopySlotKind::Native, CopySlotKind::Heap);
}

void Barrier::CopyStructArrayColouredToHeap(BaseObject* dstObj, MAddress dst, size_t dstLen,
                                             MAddress src, size_t srcLen) const
{
    CHECK(dstObj != nullptr && Heap::IsHeapAddress(dstObj));
    std::vector<size_t> offsets;
    static_cast<MArray*>(dstObj)->ForEachRefFieldInRange(
        [&offsets, dst](RefField<>& field) {
            offsets.push_back(static_cast<size_t>(reinterpret_cast<MAddress>(&field) - dst));
        }, dst, dst + srcLen);
    CopyReferenceSlots(*this, dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Heap);
}

void Barrier::CopyRefArrayColouredToHeap(MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    CHECK(Heap::IsHeapAddress(dst));
    CHECK_DETAIL(srcLen <= dstLen && srcLen % sizeof(HeapSlot<>) == 0,
                 "full-colour ref-array copy shape invalid: dstLen=%zu srcLen=%zu", dstLen, srcLen);
    std::vector<size_t> offsets;
    for (size_t offset = 0; offset < srcLen; offset += sizeof(HeapSlot<>)) {
        offsets.push_back(offset);
    }
    CopyReferenceSlots(*this, dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Heap);
}

void Barrier::CopyStaticStructPlainToNonHeap(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    CHECK(!Heap::IsHeapAddress(dst));
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(src, [&offsets, src](RefField<>& field) {
        offsets.push_back(reinterpret_cast<MAddress>(&field) - src);
    }, src, src + size);
    CopyReferenceSlots(*this, dst, size, src, size, std::move(offsets),
                       CopySlotKind::Native, CopySlotKind::Uncolored);
}

void Barrier::CopyStructArrayPlainToNonHeap(MAddress dstField, BaseObject* srcObj, MAddress srcField,
                                           size_t srcSize) const
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
        [this, srcObj, dstField, srcField, srcEnd, &cursor](RefField<false>& field) {
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

void Barrier::CopyRefArrayPlainToNonHeap(MAddress dst, BaseObject* srcObj, MAddress src, MIndex dstSize,
                                         MIndex srcSize) const
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

void Barrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{

    if (!Heap::IsHeapAddress(dst)) {
        CopyStructPlainToNonHeap(dst, obj, src, size);
        return;
    }
    CHECK(obj != nullptr);
    CopyObjectStructColouredToHeap(obj, src, dst, size, src, size);
}

void Barrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{

    if (!Heap::IsHeapAddress(dst)) {
        CopyStaticStructPlainToNonHeap(dst, src, size, gctib);
        return;
    }
    CopyStaticStructColouredToHeap(dst, size, src, size, gctib);
}

void Barrier::WriteGeneric(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
{
    WriteGenericImpl(obj, fieldPtr, src, size);
}

void Barrier::WriteGenericImpl(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
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
void Barrier::ReadGeneric(const ObjectPtr dstObj, ObjectPtr obj, void* fieldPtr, size_t size) const
{
    ReadGenericImpl(dstObj, obj, fieldPtr, size);
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

void Barrier::RecordCrossGenEdge(BaseObject* obj, MAddress fieldAddress, BaseObject* ref, zpointer prev) const
{
    // ZBarrier::heap_store_slow_path (zBarrier.cpp:253-261): buffer (p, prev)
    // when possible; otherwise mark(addr) and remember(p) directly.
    const bool heapSlot = Heap::IsHeapAddress(fieldAddress);
    // ZStoreBarrierBuffer::make_load_good (zStoreBarrierBuffer.cpp:121-140)
    // requires a heap base. Otherwise use the existing mark-and-remember path.
    if (kBufferStoreBarriers && heapSlot && Heap::IsHeapAddress(obj) &&
        !IsGcThread() && Mutator::GetMutator() != nullptr) {
        ThreadLocal::GetGCData().storeBarrierBuffer->Add(fieldAddress, obj, prev, theRememberedSet);
        return;
    }
    // addr in ZGC's heap_store_slow_path is make_load_good(prev), not the
    // incoming value (zBarrier.inline.hpp:324-334,695-705).
    if (!is_null(prev)) {
        RefField<> previous(prev);
        const ForwardingProvenance provenance{
            ForwardingHolderKind::HeapRef, obj, reinterpret_cast<const void*>(fieldAddress)
        };
        theCollector.MarkObjectIfActive(theCollector.make_load_good(previous, provenance));
    }
    (void)ref;
    if (heapSlot && !RegionInfo::GetRegionInfoAt(fieldAddress)->IsYoungRegion()) {
        theRememberedSet.Record(fieldAddress, true);
    }
}

} // namespace MapleRuntime
