// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Common/BaseObject.inline.h"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zMark.hpp"
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

template<bool forward>
bool ZBarrier::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                       BaseObject*& toObj)
{
    RefField<> oldRef(field);
    if (ZPointer::is_load_bad(oldRef.GetFieldValue())) {
        fromObj = to_object(oldRef.GetTargetObject());
        if (forward) {
            toObj = ZBarrier::remap_generation(oldRef.GetFieldValue())->relocate_or_remap_object(fromObj);
        } else {
            toObj = ZBarrier::remap_generation(oldRef.GetFieldValue())->remap_object(fromObj);
        }
        if (toObj == nullptr) {
            return false;
        }
        // R7：写回必须经规范色单产地，禁 plain RefField<>(toObj)。
        // expected 仍是 observed-raw（oldRef.GetFieldValue()）；模板 = GetAndTryTagRefField。
        RefField<> tmpField = ZBarrier::GetAndTryTagRefField(toObj);
        if (field.CompareExchange(oldRef.GetFieldValue(), tmpField.GetFieldValue())) {
            if (obj != nullptr) {
                DLOG(TRACE, "update obj %p<%p>(%zu)+%zu ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(),
                     obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field, raw(oldRef.GetFieldValue()),
                     raw(tmpField.GetFieldValue()));
            } else {
                DLOG(TRACE, "update ref@%p: 0x%zx -> %p", &field, raw(oldRef.GetFieldValue()), toObj);
            }
            return true;
        } else {
            if (obj != nullptr) {
                DLOG(TRACE,
                     "update obj %p<%p>(%zu)+%zu but cas failed ref-field@%p: %#zx(%#zx) -> %#zx but cas failed ", obj,
                     obj->GetTypeInfo(), obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field,
                     raw(oldRef.GetFieldValue()), raw(field.GetFieldValue()), raw(tmpField.GetFieldValue()));
            } else {
                DLOG(TRACE, "update but cas failed ref@%p: 0x%zx(%zx) -> %p", &field, raw(oldRef.GetFieldValue()),
                     field.GetFieldValue(), toObj);
            }
            return true;
        }
    }

    return false;
}

bool ZBarrier::TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef)
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<false>(obj, field, oldRef, newRef);
}

bool ZBarrier::CasInstallResolvedTarget(RefField<>& field, MAddress expected, zaddress target,
                                          bool allowNull)
{
    BaseObject* object = to_object(target);
    if (object != nullptr) {
        CHECK_DETAIL(Heap::IsHeapAddress(object),
                     "resolved heal target must be a heap address target=%p", object);
        CHECK_DETAIL(ZBarrier::JudgeHandOutTarget(object) == HandVerdict::Usable,
                     "resolved heal target must be usable target=%p", object);
    }
    zpointer desired = is_null(target) ? zpointer::null : RefField<>(ZAddress::store_good(target)).GetFieldValue();
    if (expected == raw(desired)) {
        return true;
    }
    const zpointer observed = to_zpointer(expected);
    auto loadGood = [](zpointer value) {
        RefField<> probe(value);
        return is_null(probe.GetTargetObject()) || ZPointer::is_load_good(probe.GetFieldValue());
    };
    if (loadGood(observed)) {
        return true;
    }
    ZBarrier::self_heal(ZBarrier::is_load_good_or_null_fast_path,
                        reinterpret_cast<volatile zpointer*>(&field), observed, desired,
                        allowNull);
    const bool healed = true;
    if (healed) {
        return true;
    }
    return true;
}

BaseObject* ZBarrier::GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field)
{
    RefField<> oldField(field);
    const char* sourceKind = kind == RefSlotKind::WEAK_REFERENT ? "weak" : "strong";
    BaseObject* latest = nullptr;
    if (ZPointer::is_mark_good(oldField.GetFieldValue())) {
        BaseObject* targetObj = to_object(oldField.GetTargetObject());
        if (!Heap::IsHeapAddress(targetObj)) {
            return nullptr;
        }
        // Anchor main ced6b14fe41380fd2dfb94c91b7fe6973786a80e
        CHECK_DETAIL(targetObj->IsValidObject(),
                     "Invalid object %p is referenced by %s object %p: %s and offset %zd", targetObj, sourceKind, obj,
                     obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
        return targetObj;
    }
    latest = to_object(ZBarrier::make_load_good(oldField.GetFieldValue()));
    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return nullptr;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by %s object %p: %s and offset %zd",
                 latest, sourceKind, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
    RefField<> newField = ZBarrier::GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, raw(oldField.GetFieldValue()),
            raw(newField.GetFieldValue()), latest, latest->GetTypeInfo(), latest->GetSize());
    }
    return latest;
}

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

// ZGC zBarrier.cpp:253-278: store slow paths are separate from entry routing.
zaddress ZBarrier::heap_store_slow_path(volatile zpointer* p, zaddress addr, zpointer prev, bool heal)
{
    StoreBarrierBuffer* buffer = StoreBarrierBuffer::buffer_for_store(heal);
    if (buffer != nullptr) {
        buffer->add(reinterpret_cast<MAddress>(p), prev);
    } else {
        mark_and_remember(p, addr);
    }
    return addr;
}

zaddress ZBarrier::no_keep_alive_heap_store_slow_path(volatile zpointer* p, zaddress addr)
{
    remember(p);
    return addr;
}

zaddress ZBarrier::native_store_slow_path(zaddress addr)
{
    if (!is_null(addr)) {
        Heap::GetHeap().MarkObjectIfActive(to_object(addr));
    }
    return addr;
}

void ZBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref)
{
    store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field), false);
    WriteReferenceImpl(obj, field, ref);
}

void ZBarrier::WriteWeakReference(BaseObject* obj, RefField<false>& field, BaseObject* ref)
{
    no_keep_alive_store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field));
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

void ZBarrier::WriteStaticRef(NativeSlot& field, BaseObject* ref)
{
    store_barrier_on_native_oop_field(reinterpret_cast<volatile zpointer*>(&field), false);
    WriteReferenceImpl(nullptr, field, ref);
}

BaseObject* ZBarrier::ReadStaticRef(NativeSlot& field)
{
    const zpointer observed = field.GetFieldValue();
    return to_object(load_barrier_on_oop_field_preloaded(reinterpret_cast<volatile zpointer*>(&field), observed));
}

// ZBarrier::mark, zBarrier.inline.hpp:742-751.
template<bool resurrect, bool gcThread, bool follow, bool finalizable>
void ZBarrier::Mark(zaddress addr)
{
    BaseObject* object = to_object(addr);
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    if (!Heap::page(reinterpret_cast<MAddress>(object))->IsYoungRegion()) {
        Heap::GetHeap().old().MarkObjectIfActive<resurrect, gcThread, follow, finalizable>(addr);
    } else {
        Heap::GetHeap().young().MarkObjectIfActive<resurrect, gcThread, follow, false>(addr);
    }
}

template void ZBarrier::Mark<false, false, true, false>(zaddress);
template void ZBarrier::Mark<false, false, false, false>(zaddress);
template void ZBarrier::Mark<true, false, true, false>(zaddress);
template void ZBarrier::Mark<false, true, true, false>(zaddress);
template void ZBarrier::Mark<false, true, false, false>(zaddress);

// ZZBarrier::mark_slow_path, zBarrier.cpp:146-156.
zaddress ZBarrier::MarkSlowPath(zaddress address)
{
    if (is_null(address)) {
        return address;
    }
    Mark<false, false, true, false>(address);
    return address;
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

// ZGC zBarrier.cpp:280-285: keep-alive loads publish resurrecting marks.
zaddress ZBarrier::keep_alive_slow_path(zaddress addr)
{
    if (!is_null(addr)) {
        Mark<true, false, true, false>(addr);
    }
    return addr;
}

// ZGC zBarrier.cpp:61-144: distinct weak/phantom keep-alive and load slow paths.
static void keep_alive_young(zaddress addr)
{
    auto& young = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
    if (young.IsPhaseMark()) {
        young.MarkObject<true, false, true, false>(addr);
    }
}

zaddress ZBarrier::blocking_keep_alive_on_weak_slow_path(volatile zpointer* p, zaddress addr)
{
    if (is_null(addr)) {
        return zaddress::null;
    }
    // Cangjie stack objects and headerless records have no heap generation.
    if (!Heap::IsHeapAddress(to_object(addr))) {
        return addr;
    }
    ZPage* page = Heap::page(raw(addr));
    if (!page->IsYoungRegion()) {
        if (!page->is_object_strongly_live(addr)) {
            return zaddress::null;
        }
    } else {
        keep_alive_young(addr);
    }
    return addr;
}

zaddress ZBarrier::blocking_keep_alive_on_phantom_slow_path(volatile zpointer* p, zaddress addr)
{
    if (is_null(addr)) {
        return zaddress::null;
    }
    // Cangjie stack objects and headerless records have no heap generation.
    if (!Heap::IsHeapAddress(to_object(addr))) {
        return addr;
    }
    ZPage* page = Heap::page(raw(addr));
    if (!page->IsYoungRegion()) {
        if (!page->is_object_live(addr)) {
            return zaddress::null;
        }
    } else {
        keep_alive_young(addr);
    }
    return addr;
}

zaddress ZBarrier::blocking_load_barrier_on_weak_slow_path(volatile zpointer* p, zaddress addr)
{
    if (is_null(addr)) {
        return zaddress::null;
    }
    // Cangjie stack objects and headerless records have no heap generation.
    if (!Heap::IsHeapAddress(to_object(addr))) {
        return addr;
    }
    ZPage* page = Heap::page(raw(addr));
    if (!page->IsYoungRegion()) {
        if (!page->is_object_strongly_live(addr)) {
            return zaddress::null;
        }
    } else {
        keep_alive_young(addr);
    }
    return addr;
}

zaddress ZBarrier::blocking_load_barrier_on_phantom_slow_path(volatile zpointer* p, zaddress addr)
{
    if (is_null(addr)) {
        return zaddress::null;
    }
    // Cangjie stack objects and headerless records have no heap generation.
    if (!Heap::IsHeapAddress(to_object(addr))) {
        return addr;
    }
    ZPage* page = Heap::page(raw(addr));
    if (!page->IsYoungRegion()) {
        if (!page->is_object_live(addr)) {
            return zaddress::null;
        }
    } else {
        keep_alive_young(addr);
    }
    return addr;
}

#if defined(MRT_DEBUG) && MRT_DEBUG == 1
// ZGC zBarrier.cpp:291-299. The Cangjie referent follows the type-info word.
void ZBarrier::verify_on_weak(volatile zpointer* referent_addr)
{
    if (referent_addr != nullptr) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(referent_addr) - TYPEINFO_PTR_SIZE;
        const BaseObject* obj = reinterpret_cast<const BaseObject*>(base);
        ASSERT(obj->IsValidObject());
        ASSERT(obj->IsWeakRef());
    }
}
#endif

zpointer ZBarrier::ColorLoadGood(zaddress address, zpointer previous)
{
    return ZAddress::load_good(address, previous);
}

BaseObject* ZBarrier::ReadReference(BaseObject* obj, RefField<false>& field)
{
    return to_object(load_barrier_on_oop_field_preloaded(
        reinterpret_cast<volatile zpointer*>(&field), field.GetFieldValue()));
}

BaseObject* ZBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field)
{
    return to_object(load_barrier_on_weak_oop_field_preloaded(
        reinterpret_cast<volatile zpointer*>(&field), field.GetFieldValue()));
}

BaseObject* ZBarrier::ReadPhantomRef(BaseObject* obj, RefField<false>& field)
{
    return to_object(load_barrier_on_phantom_oop_field_preloaded(
        reinterpret_cast<volatile zpointer*>(&field), field.GetFieldValue()));
}

// barrier for atomic operation.
void ZBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order)
{
    if (!Heap::IsHeapAddress(&field)) {
        store_barrier_on_native_oop_field(reinterpret_cast<volatile zpointer*>(&field), true);
        AtomicWriteReferenceImpl(obj, field, ref, order);
        return;
    }
    store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field), true);
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
        store_barrier_on_native_oop_field(reinterpret_cast<volatile zpointer*>(&field), true);
        return AtomicSwapReferenceImpl(obj, field, newRef, order);
    }
    store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field), true);
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
    return to_object(load_barrier_on_oop_field_preloaded(
        reinterpret_cast<volatile zpointer*>(&field), field.GetFieldValue(order)));
}

bool ZBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                      MemoryOrder succOrder, MemoryOrder failOrder)
{
    if (!Heap::IsHeapAddress(&field)) {
        store_barrier_on_native_oop_field(reinterpret_cast<volatile zpointer*>(&field), true);
        return CompareAndSwapReferenceImpl(obj, field, oldRef, newRef, succOrder, failOrder);
    }
    store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field), true);
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

bool ZBarrier::clean_barrier_on_phantom_oop_field(volatile zpointer* p)
{
    CHECK_DETAIL(ZResurrection::is_blocked(),
                 "phantom clean is only valid when resurrection is blocked");
    const zpointer o = load_atomic(p);
    auto slow_path = [=](zaddress addr) {
        return blocking_load_barrier_on_phantom_slow_path(p, addr);
    };
    return is_null(barrier(is_mark_good_fast_path, slow_path, ColorMarkGood, p, o, true));
}

void ZBarrier::load_barrier_on_oop_array(volatile zpointer* p, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        (void)load_barrier_on_oop_field(p + i);
    }
}

namespace {
HandVerdict ClassifyRawHeader(uint64_t header)
{
    if (((header >> 48) & 0x3u) == 3u) {
        return HandVerdict::Forwarded;
    }
    if ((header & 0xffffffffffffull) == 0) {
        return HandVerdict::ZeroHeader;
    }
    return HandVerdict::Usable;
}
}

RefField<> ZBarrier::GetAndTryTagRefField(BaseObject* target)
{
    // Null carries no colour (ZGC zAddress: null is never load-bad).
    if (target == nullptr) {
        return RefField<>(zpointer::null);
    }
    // TypeInfo* / binary constants / immortal metadata are not relocated,
    // but a non-null HeapSlot word is still coloured.  The load-good mask
    // fast path peels it without routing through the collector.
    if (!Heap::IsHeapAddress(target)) {
        return RefField<>(ZAddress::store_good(from_object(target)));
    }
    // ZPointer::uncolor is the sole producer accepted by ZAddress::store_good
    // (zAddress.inline.hpp:609-624,806-811). ResolveStoreValue is our
    // make-load-good producer: a relocation-set address is looked up or copied
    // by this thread; an unresolved address never reaches colouring.
    target = ZBarrier::ValidateCurrentValue(target);
    CHECK_DETAIL(target != nullptr && Heap::IsHeapAddress(target),
                 "store-good requires a resolved heap address");
    ZBarrier::CheckStoreGoodTarget("GetAndTryTagRefField", target);
    return RefField<>(ZAddress::store_good(from_object(target)));
}

BaseObject* ZBarrier::ValidateCurrentValue(BaseObject* ref)
{
    if (ref == nullptr || !Heap::IsHeapAddress(ref) || ZBarrier::JudgeHandOutTarget(ref) == HandVerdict::Usable) {
        return ref;
    }
    ZBarrier::FailClosedLoad("current raw value required", ref, 0);
}

HandVerdict ZBarrier::JudgeHandOutTarget(BaseObject* target)
{
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return HandVerdict::Usable;
    }
    const uint64_t hdr = __atomic_load_n(reinterpret_cast<const uint64_t*>(target), __ATOMIC_RELAXED);
    return ClassifyRawHeader(hdr);
}

[[noreturn]] void ZBarrier::FailClosedLoad(const char* site, BaseObject* target, uintptr_t slotBits)
{
    const HandVerdict verdict = ZBarrier::JudgeHandOutTarget(target);
    const MAddress from = target != nullptr ? reinterpret_cast<MAddress>(target) : 0;
    ZPage* region = (from != 0 && Heap::IsHeapAddress(target) && verdict != HandVerdict::ZeroHeader)
        ? Heap::page(from)
        : nullptr;
    const bool canLookup = from != 0 && Heap::IsHeapAddress(target) && verdict != HandVerdict::ZeroHeader;
    const MAddress lookupTo = canLookup
        ? forwarding_find(Heap::GetHeap().ObjectGeneration(target), from)
        : 0;
    // This is the last-chance diagnostic (zBarrier.inline.hpp:327-343). Pre-init callers, including
    // gc_unit other-vm children can enter before the generation cycle is active.
    const unsigned gcPhase = Heap::GetHeap().IsGcStarted() && ZGeneration::old() != nullptr
        ? static_cast<unsigned>(ZGeneration::old()->Snapshot().phase)
        : 0xffu;
    std::fprintf(stderr,
                 "[LOADFC][fail-closed] site=%s target=%p verdict=%u slotBits=%#zx "
                 "from=%p from_region=%p "
                 "region_type=%u generation=%u forwarding_lookup_hit=%u "
                 "table_id=%#zx from_page_epoch=%llu lifeId=%llu "
                 "lookup_state=%s gc_phase=%u "
                 "unresolved non-Usable from-address must not be handed out\n",
                 site != nullptr ? site : "?", static_cast<void*>(target),
                 static_cast<unsigned>(verdict), slotBits,
                 static_cast<void*>(target),
                 static_cast<void*>(region),
                 region != nullptr ? static_cast<unsigned>(0u) : 0xffu,
                 region != nullptr ? static_cast<unsigned>(region->generation_id()) : 0xffu,
                  lookupTo != 0 ? 1u : 0u,
                  static_cast<size_t>(0),
                  0ull,
                  0ull,
                  !canLookup ? "not_attempted" : (lookupTo != 0 ? "hit" : "miss"),
                 gcPhase);
    (void)fflush(stderr);
    (void)fflush(stdout);
    std::abort();
}

void ZBarrier::CheckStoreGoodTarget(const char* consumer, BaseObject* target)
{
    // zAddress.inline.hpp:store_good consumes an already current address.
    // The originating load/root operation performed generation-specific remap.
    (void)consumer;
    (void)ZBarrier::ValidateCurrentValue(target);
}
} // namespace MapleRuntime
