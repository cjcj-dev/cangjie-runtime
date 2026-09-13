// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Barrier.inline.h"
#include "Base/Macros.h"
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Heap.h"
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
    const uintptr_t loadBad = ::g_cjLoadBadMask;
    const uintptr_t markBad = ::g_cjMarkBadMask;
    const uintptr_t storeBad = ::g_cjStoreBadMask;
    CHECK(!ColourPredicates::is_load_good(oldRaw, loadBad) ||
          ColourPredicates::is_load_good(newRaw, loadBad));
    CHECK(!ColourPredicates::is_mark_good(oldRaw, loadBad, markBad) ||
          ColourPredicates::is_mark_good(newRaw, loadBad, markBad));
    CHECK(!ColourPredicates::is_store_good(oldRaw, loadBad, storeBad) ||
          ColourPredicates::is_store_good(newRaw, loadBad, storeBad));
    if (!ColourPredicates::has_address(newRaw)) {
        return;
    }
    CHECK(!ColourPredicates::is_marked_young(oldRaw, markBad) ||
          ColourPredicates::is_marked_young(newRaw, markBad));
    CHECK(!ColourPredicates::is_marked_old(oldRaw, markBad) ||
          ColourPredicates::is_marked_old(newRaw, markBad));
    CHECK(!ColourPredicates::is_marked_finalizable(oldRaw, markBad) ||
          ColourPredicates::is_marked_finalizable(newRaw, markBad) ||
          ColourPredicates::is_marked_old(newRaw, markBad));
}

// Preserve mark/remember metadata when upgrading a load (ZAddress::load_good).
static zpointer ColourLoadGood(BaseObject* target, zpointer observed)
{
    if (!ColourPredicates::has_address(raw(observed))) {
        return to_zpointer(::g_cjStoreGoodMask | REMEMBERED_MASK);
    }
    return to_zpointer(reinterpret_cast<uintptr_t>(target) | REMEMBERED_MASK |
        (raw(observed) & ~kPointerAddressMask & ~REMAP_COLOUR_MASK) |
        (::g_cjLoadBadMask ^ REMAP_COLOUR_MASK));
}

void Barrier::WriteI8(BaseObject* obj, Field<int8_t>& field, int8_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteI16(BaseObject* obj, Field<int16_t>& field, int16_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteI32(BaseObject* obj, Field<int32_t>& field, int32_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteI64(BaseObject* obj, Field<int64_t>& field, int64_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteF32(BaseObject* obj, Field<float>& field, float val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteF64(BaseObject* obj, Field<double>& field, double val) const { field.SetFieldValue(obj, val); }

// ZBarrier::store_barrier_on_heap_oop_field, zBarrier.inline.hpp:695-706.
// Atomic operations heal before attempting the exchange, ordinary stores buffer prev.
template<bool atomic>
void Barrier::StoreBarrier(BaseObject* obj, RefField<atomic>& field, bool heal,
                            ReferenceStrength strength) const
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    RefField<> previous(observed);
    auto fastPath = [this, heal](zpointer word) {
        RefField<> value(word);
        return theCollector.is_store_good(value) || (!heal && is_null(word));
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
        const zpointer good = to_zpointer(MakeStoreGoodSlotWord(
            reinterpret_cast<uintptr_t>(target), ::g_cjStoreGoodMask));
        ZgcSelfHeal(field, observed, good, fastPath, HealSite::BarrierReadReference);
    } else {
        RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), target, observed);
    }
}

void Barrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    const bool weakReferent = obj != nullptr && obj->IsWeakRef() &&
        reinterpret_cast<MAddress>(&field) == reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    StoreBarrier(obj, field, false, weakReferent ? ReferenceStrength::Weak : ReferenceStrength::Strong);
    WriteReferenceImpl(obj, field, ref);
}

void Barrier::PostWriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref, zpointer prev) const
{
    RefField<> previous(prev);
    if (!theCollector.is_store_good(previous) && !is_null(prev)) {
        const MAddress address = reinterpret_cast<MAddress>(&field);
        const bool weakReferent = obj != nullptr && obj->IsWeakRef() &&
            address == reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
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
    field.StoreColoured(to_zpointer(MakeStoreGoodSlotWord(
        reinterpret_cast<uintptr_t>(ref), ::g_cjStoreGoodMask)));
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

void Barrier::WriteStaticRef(RootSlot& field, BaseObject* ref) const
{
    // Native root stores have a direct previous-value mark barrier.
    // ZBarrier::native_store_slow_path (zBarrier.cpp:272-278).
    theCollector.MarkObjectIfActive(ReadStaticRef(field));

    WriteStaticRefPlain(field, ref);

}

void Barrier::WriteStaticRefPlain(RootSlot& field, BaseObject* ref) const
{
    DLOG(BARRIER, "write (barrier) static ref@%p: %p", &field, ref);
    StorePlain(field, from_object(ref));
}

void Barrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const
{
    gctib.ForEachBitmapWord(dst, [this](RefField<>& field) {
        theCollector.MarkObjectIfActive(ReadStaticRef(RootSlotAt(static_cast<void*>(&field))));
    });

    // R9 bulk：静态槽 barrier 可见；post-copy 解析转发（STACK_ROOTS_STAY_PLAIN：写回 plain）。
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK,
                 "memcpy_s failed");
    ResolveStaticStructRoots(dst, gctib);
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstLen < srcLen ? dstLen : srcLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), copyLen);
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
            ? ColourPredicates::is_load_good_or_null(raw(word), ::g_cjLoadBadMask)
            : theCollector.is_mark_good(value);
    };
    RefField<> value(observed);
    if (fastPath(observed)) {
        return to_object(value.GetTargetObject());
    }
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, obj, &field };
    BaseObject* target = theCollector.make_load_good(value, provenance);
    CHECK_DETAIL(target != nullptr || !ColourPredicates::has_address(raw(observed)),
                 "load barrier relocation must preserve a non-null reference");
    if (strength != ReferenceStrength::Strong && target != nullptr && Heap::IsHeapAddress(target)) {
        // Only the shared resurrection rendezvous publishes the blocked window.
        if (Heap::GetHeap().GetCollectorResources().IsResurrectionBlocked()) {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (region->IsYoungRegion()) {
                theCollector.MarkYoungObjectIfActive(target);
            } else {
                const bool stronglyLive = region->IsMarkedObject(region->GetMarkView<Generation::Old>(), target);
                const bool live = stronglyLive || (strength == ReferenceStrength::Phantom &&
                                                   region->IsResurrectedObject(target));
                if (!live) {
                    return nullptr; // A load never clears a referent (ZBarrier::self_heal).
                }
            }
        } else {
            theCollector.MarkObjectIfActive(target);
        }
    }
    const zpointer healed = strength == ReferenceStrength::Strong
        ? ColourLoadGood(target, observed)
        : to_zpointer(reinterpret_cast<uintptr_t>(target) | ::g_cjStoreGoodMask | REMEMBERED_MASK);
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

BaseObject* Barrier::ReadNativeValue(zaddress_unsafe observed) const
{
    BaseObject* target = reinterpret_cast<BaseObject*>(raw(observed));
    if (target != nullptr && Heap::IsHeapAddress(target)) {
        const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, nullptr };
        target = theCollector.FindLatestVersion(target, provenance, theCollector.ActiveForwardingGeneration());
    }
    return target;
}

BaseObject* Barrier::ReadStaticRef(RootSlot& field) const
{
    const zaddress_unsafe observed = field.LoadPlain();
    BaseObject* target = ReadNativeValue(observed);
    if (target != nullptr && raw(observed) != reinterpret_cast<uintptr_t>(target)) {
        HealRootIfObserved(field, observed, from_object(target), HealSite::BarrierReadStaticReference);
    }
    return target;
}

// barrier for atomic operation.
void Barrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const
{
    if (!Heap::IsHeapAddress(&field)) {
        RootSlot& root = RootSlotAt(static_cast<void*>(&field));
        theCollector.MarkObjectIfActive(ReadStaticRef(root));
        StorePlain(root, from_object(ref), order);
        return;
    }
    StoreBarrier(obj, field, true);
    AtomicWriteReferenceImpl(obj, field, ref, order);
}

void Barrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const
{
    field.StoreColoured(to_zpointer(MakeStoreGoodSlotWord(
        reinterpret_cast<uintptr_t>(ref), ::g_cjStoreGoodMask)), order);
}

BaseObject* Barrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                         MemoryOrder order) const
{
    if (!Heap::IsHeapAddress(&field)) {
        RootSlot& root = RootSlotAt(static_cast<void*>(&field));
        theCollector.MarkObjectIfActive(ReadStaticRef(root));
        return ReadNativeValue(root.ExchangePlain(from_object(newRef), order));
    }
    StoreBarrier(obj, field, true);
    return AtomicSwapReferenceImpl(obj, field, newRef, order);
}

BaseObject* Barrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                             MemoryOrder order) const
{
    const zpointer desired = to_zpointer(MakeStoreGoodSlotWord(
        reinterpret_cast<uintptr_t>(newRef), ::g_cjStoreGoodMask));
    RefField<> previous(field.Exchange(desired, order));
    return to_object(previous.GetTargetObject());
}

BaseObject* Barrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    if (!Heap::IsHeapAddress(&field)) {
        RootSlot& root = RootSlotAt(static_cast<void*>(&field));
        return ReadNativeValue(root.LoadPlain(order));
    }
    return LoadBarrier(obj, field, field.GetFieldValue(order), ReferenceStrength::Strong);
}

bool Barrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                      MemoryOrder succOrder, MemoryOrder failOrder) const
{
    if (!Heap::IsHeapAddress(&field)) {
        RootSlot& root = RootSlotAt(static_cast<void*>(&field));
        const zaddress_unsafe observed = root.LoadPlain(std::memory_order_relaxed);
        BaseObject* previous = ReadNativeValue(observed);
        theCollector.MarkObjectIfActive(previous);
        return previous == oldRef && root.CompareExchangePlain(observed, from_object(newRef),
                                                                succOrder, failOrder);
    }
    StoreBarrier(obj, field, true);
    return CompareAndSwapReferenceImpl(obj, field, oldRef, newRef, succOrder, failOrder);
}

bool Barrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                          BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    const zpointer expected = to_zpointer(MakeStoreGoodSlotWord(
        reinterpret_cast<uintptr_t>(oldRef), ::g_cjStoreGoodMask));
    const zpointer desired = to_zpointer(MakeStoreGoodSlotWord(
        reinterpret_cast<uintptr_t>(newRef), ::g_cjStoreGoodMask));
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
                BaseObject* target = ReadReference(srcObj, field);
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

namespace {
void CopyColouredSlotsToHeap(const Barrier& barrier, Collector& collector,
                             MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                             std::vector<size_t> offsets)
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
        HeapSlot<> source(to_zpointer(sourceWord));
        BaseObject* target = barrier.ReadReference(nullptr, source);
        barrier.WriteReference(nullptr, HeapSlotAt<>(dst + offset), target);
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
    CopyColouredSlotsToHeap(*this, theCollector, dst, dstLen, src, srcLen, std::move(offsets));
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
    CopyColouredSlotsToHeap(*this, theCollector, dst, dstLen, src, srcLen, std::move(offsets));
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
    CopyColouredSlotsToHeap(*this, theCollector, dst, dstLen, src, srcLen, std::move(offsets));
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
    CopyColouredSlotsToHeap(*this, theCollector, dst, dstLen, src, srcLen, std::move(offsets));
}

void Barrier::CopyStaticStructPlainToNonHeap(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    CHECK(!Heap::IsHeapAddress(dst));
    if (size == 0) {
        return;
    }
    if (!Heap::IsHeapAddress(src) && dst < src + size && src < dst + size) {
        CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK,
                     "read static struct overlap memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), size);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), size);
#endif
        return;
    }
    MAddress cursor = src;
    const MAddress srcEnd = src + size;
    gctib.ForEachBitmapWordInRange(
        src,
        [this, dst, src, srcEnd, &cursor](RefField<>& srcField) {
            MAddress fieldAddr = reinterpret_cast<MAddress>(&srcField);
            if (fieldAddr < cursor || fieldAddr >= srcEnd) {
                return;
            }
            if (fieldAddr > cursor) {
                size_t gap = static_cast<size_t>(fieldAddr - cursor);
                CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst + (cursor - src)), gap,
                                      reinterpret_cast<void*>(cursor), gap) == EOK,
                             "read static struct gap memcpy_s failed");
            }
            BaseObject* target = ReadReference(nullptr, srcField);
            StorePlain(RootSlotAt(dst + (fieldAddr - src)), from_object(target));
            cursor = fieldAddr + sizeof(RefField<>);
        },
        src, srcEnd);
    if (cursor < srcEnd) {
        size_t tail = static_cast<size_t>(srcEnd - cursor);
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst + (cursor - src)), tail,
                              reinterpret_cast<void*>(cursor), tail) == EOK,
                     "read static struct tail memcpy_s failed");
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), size);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), size);
#endif
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
            BaseObject* target = ReadReference(srcObj, field);
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
            BaseObject* newRef = ReadReference(srcObj, currentSrcField);
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
            BaseObject* newRef = ReadReference(srcObj, currentSrcField);
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

// Post-copy fixup for a bulk write into static/global storage.
//
// What it must do: the bytes just memcpy'd may name stale (pre-forwarding) objects, so each ref
// word is resolved through the phase read barrier and the current version is stored back.
//
// What it must NOT do: store a *coloured* value. Static words are roots -- StaticRootTable
// registers them as RootSlot (TracingCollector.cpp:225-243) and WCollector::EnumAndTagRawRoot
// heals them with StorePlain (WCollector.cpp:962-1001, "the root storage itself is never exposed
// as a HeapSlot"). Colouring here is overwritten plain by the next root enumeration, and CAS on a
// static slot sits on the relroroot hazard (B-4 ⑤: those pages can be RELRO r--p).
//
// The read barrier may self-heal the slot it is handed; it is handed a *local copy* so the heal
// cannot leak colour back into the static word.
void Barrier::ResolveStaticStructRoots(MAddress dst, const GCTib gctib) const
{
    gctib.ForEachRootSlot(dst, [this](RootSlot& slot) {
        zaddress_unsafe observed = slot.LoadPlain();
        if (is_null(observed)) {
            return;
        }
        // Legacy coloured roots still exist at external ABI edges; decode, never store back.
        HeapSlot<> observedBits(to_zpointer(raw(observed)));
        BaseObject* resolved = ReadReference(nullptr, observedBits);
        if (raw(observed) != reinterpret_cast<MAddress>(resolved)) {
            StorePlain(slot, from_object(resolved));
        }
    });
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
    if (kBufferStoreBarriers && !IsGcThread() && Mutator::GetMutator() != nullptr && heapSlot && obj != nullptr) {
        ThreadLocal::GetGCData().storeBarrierBuffer.Add(fieldAddress, obj, prev, theRememberedSet);
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
