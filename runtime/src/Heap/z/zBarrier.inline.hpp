// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_BARRIER_INLINE_H
#define MRT_BARRIER_INLINE_H

#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "securec.h"

namespace MapleRuntime {
// ZBarrier::barrier, zBarrier.inline.hpp:319-344. Retain the observed colored
// word until after marking: the color operation needs its old-generation bits.
template<typename SlowPath>
inline zaddress Barrier::MarkBarrier(MarkFastPath fast, SlowPath slow, MarkColor color,
                                 RefField<>& field, zpointer observed, const ForwardingProvenance& provenance) const
{
    if (fast(observed)) {
        return RefField<>(observed).GetTargetObject();
    }
    RefField<> value(observed);
    const zaddress loadGood = from_object(theCollector.ValidateCurrentValue(
        theCollector.make_load_good(value, provenance), provenance));
    const zaddress good = (this->*slow)(loadGood);
    const zpointer colored = color(good, observed);
    ZgcSelfHeal(field, observed, colored, fast, HealSite::BarrierReadReference);
    return good;
}

// ZBarrier::mark_if_young, zBarrier.inline.hpp:763-767. Native literal roots
// are the Cangjie non-heap case and have no generation owner.
inline void Barrier::MarkIfYoung(zaddress address) const
{
    BaseObject* object = to_object(address);
    if (Heap::IsHeapAddress(object) &&
        RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object))->IsYoungRegion()) {
        MarkYoung(address);
    }
}

// ZBarrier::mark_young<DontResurrect, GCThread, Follow>, :754-759.
inline void Barrier::MarkYoung(zaddress address) const
{
    theCollector.MarkYoungRootObject(to_object(address));
}

// ZBarrier::is_mark_young_good_fast_path, zBarrier.inline.hpp:392-394.
inline bool Barrier::IsMarkYoungGoodFastPath(zpointer value)
{
    return ZPointer::is_load_good(to_zpointer(raw(value))) &&
           ZPointer::is_marked_young(to_zpointer(raw(value)));
}

inline zpointer Barrier::ColorMarkYoungGood(zaddress address, zpointer previous)
{
    return ZAddress::mark_young_good(address, previous);
}

inline void Barrier::MarkYoungGoodBarrierOnOopField(NativeSlot& field) const
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &field };
    MarkBarrier(IsMarkYoungGoodFastPath, &Barrier::MarkYoungSlowPath,
                ColorMarkYoungGood, field, observed, provenance);
}

// ZBarrier fast/color functions, zBarrier.inline.hpp:379-448.
inline bool Barrier::IsMarkGoodFastPath(zpointer value)
{
    return ZPointer::is_mark_good(value);
}

inline bool Barrier::IsStoreGoodOrNullAnyFastPath(zpointer value)
{
    return is_null_any(value) || !ZPointer::is_store_bad(value);
}

inline zpointer Barrier::ColorMarkGood(zaddress address, zpointer previous)
{
    return ZAddress::mark_good(address, previous);
}

// ZBarrier::is_finalizable_good_fast_path / color_finalizable_good, :396/:418.
inline bool Barrier::IsFinalizableGoodFastPath(zpointer value)
{
    return ZPointer::is_load_good(value) && ZPointer::is_marked_any_old(value);
}

inline zpointer Barrier::ColorFinalizableGood(zaddress address, zpointer previous)
{
    return ZPointer::is_marked_old(previous) ? ZAddress::mark_old_good(address, previous)
                                           : ZAddress::finalizable_good(address, previous);
}

inline zpointer Barrier::ColorStoreGood(zaddress address, zpointer)
{
    return ZAddress::store_good(address);
}

inline zpointer Barrier::ColorRemsetGood(zaddress address, zpointer previous)
{
    if (is_null(address) || RegionInfo::GetRegionInfoAt(raw(address))->IsYoungRegion()) {
        return ColorMarkGood(address, previous);
    }
    return ColorMarkYoungGood(address, previous);
}

// Native finalizer registrations represent the referent slot of a Java
// FinalReference. Root/seed routing is separate from from-old field routing.
inline void Barrier::MarkFinalizableBarrierOnRoot(NativeSlot& field) const
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &field };
    MarkBarrier(IsFinalizableGoodFastPath, &Barrier::MarkFinalizableSlowPath,
                ColorFinalizableGood, field, observed, provenance);
}

// ZBarrier::mark_barrier_on_old_oop_field, zBarrier.inline.hpp:626-660.
inline void Barrier::MarkBarrierOnOldOopField(BaseObject* holder, RefField<>& field, bool finalizable) const
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, holder, &field };
    zaddress result;
    if (finalizable) {
        result = MarkBarrier(IsFinalizableGoodFastPath, &Barrier::MarkFinalizableFromOldSlowPath,
                             ColorFinalizableGood, field, observed, provenance);
    } else {
        result = MarkBarrier(IsMarkGoodFastPath, &Barrier::MarkFromOldSlowPath,
                             ColorMarkGood, field, observed, provenance);
    }
#if defined(MRT_TESTABLE_INTERNALS)
    if (testFieldMarkResult) testFieldMarkResult(finalizable ? FieldMarkKind::Finalizable : FieldMarkKind::Old,
                                                field, observed, result);
#endif
    (void)result;
}

// ZBarrier::mark_barrier_on_young_oop_field, zBarrier.inline.hpp:662-666.
inline void Barrier::MarkBarrierOnYoungOopField(RefField<>& field) const
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, nullptr, &field };
    const zaddress result = MarkBarrier(IsStoreGoodOrNullAnyFastPath, &Barrier::MarkFromYoungSlowPath,
                                       ColorStoreGood, field, observed, provenance);
#if defined(MRT_TESTABLE_INTERNALS)
    if (testFieldMarkResult) testFieldMarkResult(FieldMarkKind::Young, field, observed, result);
#endif
    (void)result;
}

// ZBarrier::remset_barrier_on_oop_field, zBarrier.inline.hpp:681-684.
inline zaddress Barrier::RemsetBarrierOnOopField(RefField<>& field) const
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Remset, nullptr, &field };
    const zaddress result = MarkBarrier(IsMarkYoungGoodFastPath, &Barrier::MarkYoungSlowPath,
                                       ColorRemsetGood, field, observed, provenance);
#if defined(MRT_TESTABLE_INTERNALS)
    if (testFieldMarkResult) testFieldMarkResult(FieldMarkKind::Remset, field, observed, result);
#endif
    return result;
}

} // namespace MapleRuntime
#endif // ~MRT_BARRIER_INLINE_H
