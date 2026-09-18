// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Base/Macros.h"
#include "Base/Panic.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zResurrection.hpp"
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
#if defined(MRT_TESTABLE_INTERNALS)
std::function<void(ZBarrier::FieldMarkKind, RefField<>&, zpointer, zaddress)> ZBarrier::testFieldMarkResult;
#endif
static_assert(!std::is_polymorphic<ZBarrier>::value, "ZBarrier must not regain virtual dispatch");

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

        void CopyOopOne(
                             MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                             std::vector<size_t> offsets, CopySlotKind sourceKind, CopySlotKind destinationKind)
{
    CHECK_DETAIL(srcLen <= dstLen, "full-colour copy source does not fit: dstLen=%zu srcLen=%zu", dstLen, srcLen);
    if (srcLen == 0) {
        return;
    }
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    size_t cursor = 0;
    for (size_t offset : offsets) {
        CHECK_DETAIL(offset + sizeof(HeapSlot<>) <= srcLen,
                     "full-colour ref offset outside copy: offset=%zu srcLen=%zu", offset, srcLen);
        if (offset > cursor) {
            const size_t gap = offset - cursor;
            CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dst + cursor), gap,
                                   reinterpret_cast<void*>(src + cursor), gap) == EOK,
                         "full-colour primitive-gap copy failed");
        }
        BaseObject* target = nullptr;
        if (sourceKind == CopySlotKind::Uncolored) {
            target = to_object(safe(RootSlotAt(src + offset).LoadPlain()));
        } else if (sourceKind == CopySlotKind::Native) {
            target = ZBarrier::ReadStaticRef(NativeSlotAt(src + offset));
        } else {
            target = ZBarrier::ReadReference(nullptr, HeapSlotAt<>(src + offset));
        }
        if (destinationKind == CopySlotKind::Heap) {
            ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(dst + offset), false);
            HeapSlotAt<>(dst + offset).StoreColoured(ZAddress::store_good(from_object(target)));
        } else if (destinationKind == CopySlotKind::Native) {
            ZBarrier::WriteStaticRef(NativeSlotAt(dst + offset), target);
        } else {
            StorePlain(RootSlotAt(dst + offset), from_object(target));
        }
        cursor = offset + sizeof(HeapSlot<>);
    }
    if (cursor < srcLen) {
        const size_t tail = srcLen - cursor;
        CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dst + cursor), tail,
                               reinterpret_cast<void*>(src + cursor), tail) == EOK,
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
    CopyOopOne( dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Heap);
}

void ZBarrier::ReadStruct(MAddress dst, MAddress src, size_t size, GCTib gctib)
{
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(src, [&offsets, src](RefField<>& field) {
        offsets.push_back(reinterpret_cast<MAddress>(&field) - src);
    }, src, src + size);
    CopyOopOne( dst, size, src, size, std::move(offsets),
        CopySlotKind::Heap, CopySlotKind::Uncolored);
}

// ZZBarrier::store_barrier_on_heap_oop_field, zBarrier.inline.hpp:695-706.
// Atomic operations heal before attempting the exchange, ordinary stores buffer prev.
template<bool atomic>
void ZBarrier::StoreBarrier(BaseObject* obj, RefField<atomic>& field, bool heal,
                            ReferenceStrength strength)
{
    (void)obj;
    volatile zpointer* p = reinterpret_cast<volatile zpointer*>(&field);
    const zpointer prev = load_atomic(p);
    if (strength != ReferenceStrength::Strong) {
        auto slow = [p](zaddress addr) {
            remember(p);
            return addr;
        };
        barrier(is_store_good_fast_path, slow, ColorStoreGood, nullptr, prev, false);
        return;
    }
    auto slow = [p, prev, heal](zaddress addr) {
        StoreBarrierBuffer* buffer = StoreBarrierBuffer::buffer_for_store(heal);
        if (buffer != nullptr) {
            buffer->add(reinterpret_cast<MAddress>(p), prev);
        } else {
            mark_and_remember(p, addr);
        }
        return addr;
    };
    if (heal) {
        barrier(is_store_good_fast_path, slow, ColorStoreGood, p, prev, false);
    } else {
        barrier(is_store_good_or_null_fast_path, slow, ColorStoreGood, nullptr, prev, false);
    }
}

void ZBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref)
{
    const bool weakReferent = obj != nullptr && Heap::IsHeapAddress(obj) && obj->IsWeakRef() &&
        reinterpret_cast<MAddress>(&field) == reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    StoreBarrier(obj, field, false, weakReferent ? ReferenceStrength::Weak : ReferenceStrength::Strong);
    WriteReferenceImpl(obj, field, ref);
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
    volatile zpointer* p = reinterpret_cast<volatile zpointer*>(&field);
    const zpointer prev = load_atomic(p);
    auto slow = [](zaddress addr) {
        if (!is_null(addr)) {
            Heap::GetHeap().MarkObjectIfActive(to_object(addr));
        }
        return addr;
    };
    if (heal) {
        barrier(is_store_good_fast_path, slow, ColorStoreGood, p, prev, false);
    } else {
        barrier(is_store_good_or_null_fast_path, slow, ColorStoreGood, nullptr, prev, false);
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
    auto& young = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
    ASSERT(young.IsPhaseMark());
    if (is_null(address)) return address;
    if (Heap::page(raw(address))->IsYoungRegion()) {
        young.MarkObject<false, true, true, false>(address);
        return address;
    }
    if (young.IsMajorRoots()) {
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).MarkObject<false, true, true, false>(address);
        return address;
    }
    return address;
}

// ZZBarrier::mark_from_old_slow_path, zBarrier.cpp:185-203.
zaddress ZBarrier::MarkFromOldSlowPath(zaddress address)
{
    auto& old = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
    if (is_null(address)) return address;
    if (!Heap::page(raw(address))->IsYoungRegion()) {
        old.MarkObject<false, true, true, false>(address);
        return address;
    }
    return zaddress::null;
}

// ZZBarrier::mark_finalizable_slow_path, zBarrier.cpp:218-232.
zaddress ZBarrier::MarkFinalizableSlowPath(zaddress address)
{
    auto& old = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
    auto& young = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
    ASSERT(old.IsPhaseMark() || young.IsPhaseMark());
    if (is_null(address)) return address;
    if (!Heap::page(raw(address))->IsYoungRegion()) {
        old.MarkObject<false, true, true, true>(address);
        return address;
    }
    young.MarkObjectIfActive<false, true, true, false>(address);
    return address;
}

// ZZBarrier::mark_finalizable_from_old_slow_path, zBarrier.cpp:234-250.
zaddress ZBarrier::MarkFinalizableFromOldSlowPath(zaddress address)
{
    auto& old = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
    CHECK(old.IsPhaseMark() || Heap::GetHeap().GetZGeneration(ZGenerationId::young).IsPhaseMark());
    if (is_null(address)) return address;
    if (!Heap::page(raw(address))->IsYoungRegion()) {
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
    CopyOopOne( dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Native);
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), srcLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

// ZZBarrier::barrier and weak/phantom slow paths, zBarrier.inline.hpp:319-343,484-565.
zaddress ZBarrier::load_good_slow_path(zaddress addr)
{
    return addr;
}

zaddress ZBarrier::keep_alive_slow_path(zaddress addr)
{
    if (!is_null(addr)) {
        Heap::GetHeap().MarkObjectIfActive(to_object(addr));
    }
    return addr;
}

zaddress ZBarrier::blocking_keep_alive_on_weak_slow_path(zaddress addr)
{
    if (is_null(addr)) {
        return zaddress::null;
    }
    BaseObject* target = to_object(addr);
    if (!Heap::IsHeapAddress(target)) {
        return addr;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(target));
    if (region->IsYoungRegion()) {
        Heap::GetHeap().MarkYoungObjectIfActive(target);
        return addr;
    }
    if (!region->is_object_strongly_live(addr)) {
        return zaddress::null;
    }
    return addr;
}

zaddress ZBarrier::blocking_keep_alive_on_phantom_slow_path(zaddress addr)
{
    if (is_null(addr)) {
        return zaddress::null;
    }
    BaseObject* target = to_object(addr);
    if (!Heap::IsHeapAddress(target)) {
        return addr;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(target));
    if (region->IsYoungRegion()) {
        Heap::GetHeap().MarkYoungObjectIfActive(target);
        return addr;
    }
    if (!region->is_object_live(addr)) {
        return zaddress::null;
    }
    return addr;
}

zaddress ZBarrier::blocking_load_barrier_on_phantom_slow_path(zaddress addr)
{
    return blocking_keep_alive_on_phantom_slow_path(addr);
}

zpointer ZBarrier::ColorLoadGood(zaddress address, zpointer previous)
{
    return ZAddress::load_good(address, previous);
}

template<bool atomic>
BaseObject* ZBarrier::LoadBarrier(BaseObject* obj, RefField<atomic>& field, zpointer observed,
                                 ReferenceStrength strength)
{
    (void)obj;
    volatile zpointer* p = reinterpret_cast<volatile zpointer*>(&field);
    if (strength == ReferenceStrength::Strong) {
        return to_object(barrier(is_load_good_or_null_fast_path, &ZBarrier::load_good_slow_path,
                                 ColorLoadGood, p, observed, false));
    }
    const bool blocked = ZResurrection::is_blocked();
    if (!blocked) {
        return to_object(barrier(is_mark_good_fast_path, &ZBarrier::keep_alive_slow_path,
                                 ColorMarkGood, p, observed, false));
    }
    if (strength == ReferenceStrength::Weak) {
        return to_object(barrier(is_mark_good_fast_path, &ZBarrier::blocking_keep_alive_on_weak_slow_path,
                                 ColorMarkGood, p, observed, false));
    }
    return to_object(barrier(is_mark_good_fast_path, &ZBarrier::blocking_keep_alive_on_phantom_slow_path,
                             ColorMarkGood, p, observed, false));
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
    return field.CompareExchange(expected, desired, succOrder, failOrder);
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
    CopyOopOne( dst, dstLen, src, srcLen, std::move(offsets),
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
    CopyOopOne( dst, dstLen, src, srcLen, std::move(offsets),
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
    CopyOopOne( dst, dstLen, src, srcLen, std::move(offsets),
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
    CopyOopOne( dst, dstLen, src, srcLen, std::move(offsets),
        Heap::IsHeapAddress(src) ? CopySlotKind::Heap : CopySlotKind::Uncolored, CopySlotKind::Heap);
}

void ZBarrier::CopyStaticStructPlainToNonHeap(MAddress dst, MAddress src, size_t size, const GCTib gctib)
{
    CHECK(!Heap::IsHeapAddress(dst));
    std::vector<size_t> offsets;
    gctib.ForEachBitmapWordInRange(src, [&offsets, src](RefField<>& field) {
        offsets.push_back(reinterpret_cast<MAddress>(&field) - src);
    }, src, src + size);
    CopyOopOne( dst, size, src, size, std::move(offsets),
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

void ZBarrier::RecordCrossGenEdge(BaseObject* obj, MAddress fieldAddress, BaseObject* ref, zpointer prev)
{
    (void)obj;
    (void)ref;
    StoreBarrierBuffer* buffer = StoreBarrierBuffer::buffer_for_store(false);
    if (buffer != nullptr) {
        buffer->add(fieldAddress, prev);
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

zaddress ZBarrier::load_barrier_on_oop_field_preloaded(volatile zpointer* p, zpointer o)
{
    auto& field = *reinterpret_cast<RefField<false>*>(const_cast<zpointer*>(p));
    return from_object(LoadBarrier(nullptr, field, o, ReferenceStrength::Strong));
}

zaddress ZBarrier::load_barrier_on_oop_field(volatile zpointer* p)
{
    return load_barrier_on_oop_field_preloaded(p, load_atomic(p));
}

zaddress ZBarrier::load_barrier_on_weak_oop_field_preloaded(volatile zpointer* p, zpointer o)
{
    auto& field = *reinterpret_cast<RefField<false>*>(const_cast<zpointer*>(p));
    return from_object(LoadBarrier(nullptr, field, o, ReferenceStrength::Weak));
}

zaddress ZBarrier::load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o)
{
    auto& field = *reinterpret_cast<RefField<false>*>(const_cast<zpointer*>(p));
    return from_object(LoadBarrier(nullptr, field, o, ReferenceStrength::Phantom));
}

zaddress ZBarrier::no_keep_alive_load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o)
{
    if (ZResurrection::is_blocked()) {
        return barrier(is_mark_good_fast_path, &ZBarrier::blocking_load_barrier_on_phantom_slow_path,
                       ColorMarkGood, p, o, false);
    }
    return load_barrier_on_oop_field_preloaded(p, o);
}

bool ZBarrier::clean_barrier_on_phantom_oop_field(volatile zpointer* p)
{
    CHECK_DETAIL(ZResurrection::is_blocked(),
                 "phantom clean is only valid when resurrection is blocked");
    const zpointer o = load_atomic(p);
    return is_null(barrier(is_mark_good_fast_path, &ZBarrier::blocking_load_barrier_on_phantom_slow_path,
                           ColorMarkGood, p, o, true));
}

void ZBarrier::load_barrier_on_oop_array(volatile zpointer* p, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        (void)load_barrier_on_oop_field(p + i);
    }
}

} // namespace MapleRuntime
