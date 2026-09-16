// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_BARRIER_INLINE_H
#define MRT_BARRIER_INLINE_H

#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zGenerationId.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "ObjectModel/RefField.inline.h"
#include "securec.h"

namespace MapleRuntime {
// ZZBarrier::barrier, zBarrier.inline.hpp:319-344. Retain the observed colored
// word until after marking: the color operation needs its old-generation bits.
template<typename SlowPath>
inline zaddress ZBarrier::MarkBarrier(MarkFastPath fast, SlowPath slow, MarkColor color,
                                 RefField<>& field, zpointer observed, const ForwardingProvenance&)
{
    return barrier(fast, slow, color, reinterpret_cast<volatile zpointer*>(&field), observed, false);
}

// ZZBarrier::mark_if_young, zBarrier.inline.hpp:763-767. Native literal roots
// are the Cangjie non-heap case and have no generation owner.
inline void ZBarrier::MarkIfYoung(zaddress address)
{
    BaseObject* object = to_object(address);
    if (Heap::IsHeapAddress(object) &&
        Heap::page(reinterpret_cast<MAddress>(object))->IsYoungRegion()) {
        MarkYoung(address);
    }
}

// ZZBarrier::mark_young<DontResurrect, GCThread, Follow>, :754-759.
inline void ZBarrier::MarkYoung(zaddress address)
{
    Heap::GetHeap().GetCollector().MarkYoungRootObject(to_object(address));
}

// ZZBarrier::is_mark_young_good_fast_path, zBarrier.inline.hpp:392-394.
inline bool ZBarrier::IsMarkYoungGoodFastPath(zpointer value)
{
    return ZPointer::is_load_good(to_zpointer(raw(value))) &&
           ZPointer::is_marked_young(to_zpointer(raw(value)));
}

inline zpointer ZBarrier::ColorMarkYoungGood(zaddress address, zpointer previous)
{
    return ZAddress::mark_young_good(address, previous);
}

inline void ZBarrier::MarkYoungGoodBarrierOnOopField(NativeSlot& field)
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &field };
    MarkBarrier(IsMarkYoungGoodFastPath, &ZBarrier::MarkYoungSlowPath,
                ColorMarkYoungGood, field, observed, provenance);
}

// ZBarrier fast/color functions, zBarrier.inline.hpp:379-448.
inline bool ZBarrier::IsMarkGoodFastPath(zpointer value)
{
    return ZPointer::is_mark_good(value);
}

inline bool ZBarrier::IsStoreGoodOrNullAnyFastPath(zpointer value)
{
    return is_null_any(value) || !ZPointer::is_store_bad(value);
}

inline zpointer ZBarrier::ColorMarkGood(zaddress address, zpointer previous)
{
    return ZAddress::mark_good(address, previous);
}

// ZZBarrier::is_finalizable_good_fast_path / color_finalizable_good, :396/:418.
inline bool ZBarrier::IsFinalizableGoodFastPath(zpointer value)
{
    return ZPointer::is_load_good(value) && ZPointer::is_marked_any_old(value);
}

inline zpointer ZBarrier::ColorFinalizableGood(zaddress address, zpointer previous)
{
    return ZPointer::is_marked_old(previous) ? ZAddress::mark_old_good(address, previous)
                                           : ZAddress::finalizable_good(address, previous);
}

inline zpointer ZBarrier::ColorStoreGood(zaddress address, zpointer)
{
    return ZAddress::store_good(address);
}

inline zpointer ZBarrier::ColorRemsetGood(zaddress address, zpointer previous)
{
    if (is_null(address) || Heap::page(raw(address))->IsYoungRegion()) {
        return ColorMarkGood(address, previous);
    }
    return ColorMarkYoungGood(address, previous);
}

// Native finalizer registrations represent the referent slot of a Java
// FinalReference. Root/seed routing is separate from from-old field routing.
inline void ZBarrier::MarkFinalizableBarrierOnRoot(NativeSlot& field)
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &field };
    MarkBarrier(IsFinalizableGoodFastPath, &ZBarrier::MarkFinalizableSlowPath,
                ColorFinalizableGood, field, observed, provenance);
}

// ZZBarrier::mark_barrier_on_old_oop_field, zBarrier.inline.hpp:626-660.
inline void ZBarrier::MarkBarrierOnOldOopField(BaseObject* holder, RefField<>& field, bool finalizable)
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, holder, &field };
    zaddress result;
    if (finalizable) {
        result = MarkBarrier(IsFinalizableGoodFastPath, &ZBarrier::MarkFinalizableFromOldSlowPath,
                             ColorFinalizableGood, field, observed, provenance);
    } else {
        result = MarkBarrier(IsMarkGoodFastPath, &ZBarrier::MarkFromOldSlowPath,
                             ColorMarkGood, field, observed, provenance);
    }
#if defined(MRT_TESTABLE_INTERNALS)
    if (testFieldMarkResult) testFieldMarkResult(finalizable ? FieldMarkKind::Finalizable : FieldMarkKind::Old,
                                                field, observed, result);
#endif
    (void)result;
}

// ZZBarrier::mark_barrier_on_young_oop_field, zBarrier.inline.hpp:662-666.
inline void ZBarrier::MarkBarrierOnYoungOopField(RefField<>& field)
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, nullptr, &field };
    const zaddress result = MarkBarrier(IsStoreGoodOrNullAnyFastPath, &ZBarrier::MarkFromYoungSlowPath,
                                       ColorStoreGood, field, observed, provenance);
#if defined(MRT_TESTABLE_INTERNALS)
    if (testFieldMarkResult) testFieldMarkResult(FieldMarkKind::Young, field, observed, result);
#endif
    (void)result;
}

// ZZBarrier::remset_barrier_on_oop_field, zBarrier.inline.hpp:681-684.
inline zaddress ZBarrier::RemsetBarrierOnOopField(RefField<>& field)
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Remset, nullptr, &field };
    const zaddress result = MarkBarrier(IsMarkYoungGoodFastPath, &ZBarrier::MarkYoungSlowPath,
                                       ColorRemsetGood, field, observed, provenance);
#if defined(MRT_TESTABLE_INTERNALS)
    if (testFieldMarkResult) testFieldMarkResult(FieldMarkKind::Remset, field, observed, result);
#endif
    return result;
}

inline bool ZBarrier::is_load_good_or_null_fast_path(zpointer ptr)
{
    return ZPointer::is_load_good_or_null(ptr);
}

inline bool ZBarrier::is_mark_good_fast_path(zpointer ptr)
{
    return ZPointer::is_mark_good(ptr);
}

inline bool ZBarrier::is_store_good_fast_path(zpointer ptr)
{
    return ZPointer::is_store_good(ptr);
}

inline bool ZBarrier::is_store_good_or_null_fast_path(zpointer ptr)
{
    return ZPointer::is_store_good_or_null(ptr);
}

inline bool ZBarrier::is_store_good_or_null_any_fast_path(zpointer ptr)
{
    return IsStoreGoodOrNullAnyFastPath(ptr);
}

inline bool ZBarrier::is_mark_young_good_fast_path(zpointer ptr)
{
    return IsMarkYoungGoodFastPath(ptr);
}

inline bool ZBarrier::is_finalizable_good_fast_path(zpointer ptr)
{
    return IsFinalizableGoodFastPath(ptr);
}

inline zpointer ZBarrier::load_atomic(volatile zpointer* p)
{
    return reinterpret_cast<RefField<>*>(const_cast<zpointer*>(p))->GetFieldValue(std::memory_order_relaxed);
}

inline ZGeneration* ZBarrier::remap_generation(zpointer ptr)
{
    CHECK_DETAIL(!ZPointer::is_load_good(ptr), "load-good reference does not need remap");
    auto& collector = Heap::GetHeap().GetCollector();
    if (ZPointer::is_old_load_good(ptr)) {
        return &collector.GetGenerationCycle(GCCycleGeneration::YOUNG);
    }
    if (ZPointer::is_young_load_good(ptr)) {
        return &collector.GetGenerationCycle(GCCycleGeneration::OLD);
    }
    if ((raw(ptr) & ZPointerRememberedMask) == ZPointerRememberedMask) {
        return &collector.GetGenerationCycle(GCCycleGeneration::OLD);
    }
    const MAddress address = untype(RefField<>(ptr).GetTargetObject());
    if (ForwardingTable::get(address, Generation::Young) != nullptr) {
        CHECK(ForwardingTable::get(address, Generation::Old) == nullptr);
        return &collector.GetGenerationCycle(GCCycleGeneration::YOUNG);
    }
    return &collector.GetGenerationCycle(GCCycleGeneration::OLD);
}

inline zaddress ZBarrier::relocate_or_remap(zaddress_unsafe addr, ZGeneration* generation)
{
    const ZGenerationId id = (generation == &Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG))
        ? ZGenerationId::young
        : ZGenerationId::old;
    return from_object(Heap::GetHeap().GetCollector().relocate_or_remap_object(to_object(safe(addr)), id));
}

inline zaddress ZBarrier::remap(zaddress_unsafe addr, ZGeneration* generation)
{
    return relocate_or_remap(addr, generation);
}

inline zaddress ZBarrier::make_load_good(zpointer ptr)
{
    if (is_null_any(ptr)) {
        return zaddress::null;
    }
    if (ZPointer::is_load_good_or_null(ptr)) {
        return RefField<>(ptr).GetTargetObject();
    }
    return relocate_or_remap(to_zaddress_unsafe(untype(RefField<>(ptr).GetTargetObject())),
                            remap_generation(ptr));
}

inline zaddress ZBarrier::make_load_good_no_relocate(zpointer ptr)
{
    if (is_null_any(ptr)) {
        return zaddress::null;
    }
    if (ZPointer::is_load_good_or_null(ptr)) {
        return RefField<>(ptr).GetTargetObject();
    }
    return remap(to_zaddress_unsafe(untype(RefField<>(ptr).GetTargetObject())), remap_generation(ptr));
}

inline void ZBarrier::assert_transition_monotonicity(zpointer oldPtr, zpointer newPtr)
{
    AssertBarrierTransitionMonotonicity(oldPtr, newPtr);
}

inline void ZBarrier::self_heal(ZBarrierFastPath fast_path, volatile zpointer* p, zpointer ptr, zpointer heal_ptr,
                               bool allow_null)
{
    if (!allow_null && is_null_assert_load_good(heal_ptr) && !is_null_any(ptr)) {
        return;
    }
    if (fast_path(ptr) || !fast_path(heal_ptr)) {
        return;
    }
    auto& field = *reinterpret_cast<RefField<>*>(const_cast<zpointer*>(p));
    for (;;) {
        assert_transition_monotonicity(ptr, heal_ptr);
        zpointer prev = zpointer::null;
        if (field.CompareExchange(ptr, heal_ptr, std::memory_order_relaxed, std::memory_order_relaxed, &prev)) {
            return;
        }
        if (fast_path(prev)) {
            return;
        }
        ptr = prev;
    }
}

template<typename SlowPath>
inline zaddress ZBarrier::barrier(ZBarrierFastPath fast_path, SlowPath slow_path, ZBarrierColor color,
                                  volatile zpointer* p, zpointer o, bool allow_null)
{
    if (fast_path(o)) {
        return RefField<>(o).GetTargetObject();
    }
    const zaddress load_good_addr = make_load_good(o);
    const zaddress good_addr = slow_path(load_good_addr);
    if (p != nullptr) {
        const zpointer good_ptr = color(good_addr, o);
        self_heal(fast_path, p, o, good_ptr, allow_null);
    }
    return good_addr;
}

inline void ZBarrier::remap_young_relocated(volatile zpointer* p, zpointer o)
{
    const zaddress load_good_addr = make_load_good_no_relocate(o);
    const zpointer good_ptr = ZAddress::load_good(load_good_addr, o);
    self_heal(is_load_good_or_null_fast_path, p, o, good_ptr, false);
}

inline void ZBarrier::remember(volatile zpointer* p)
{
    const MAddress address = reinterpret_cast<MAddress>(p);
    ZPage* page = Heap::page(address);
    if (page != nullptr && !page->IsYoungRegion()) {
        Heap::GetHeap().GetRememberedSet().Record(address, true);
    }
}

inline void ZBarrier::mark_and_remember(volatile zpointer* p, zaddress addr)
{
    if (!is_null(addr)) {
        Heap::GetHeap().GetCollector().MarkObjectIfActive(to_object(addr));
    }
    remember(p);
}

} // namespace MapleRuntime
#endif // ~MRT_BARRIER_INLINE_H

