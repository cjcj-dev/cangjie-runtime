// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Common/BaseObject.inline.h"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zBarrierSet.hpp"
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

// Value-type ABI entries carry their GC layout even when the optional holder
// is null. Process the same slots as object-layout copies without guessing a base.
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
        ZBarrier::MarkYoung<true, false, true>(addr);
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

// barrier for atomic operation.
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
    // (zAddress.inline.hpp:609-624,806-811). ValidateCurrentValue is our
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
    const unsigned gcPhase = ZGeneration::old() != nullptr
        ? static_cast<unsigned>(ZGeneration::old()->phase())
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
