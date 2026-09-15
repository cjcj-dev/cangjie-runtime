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
    // Cangjie value records may contain references to non-heap literals.
    // This carrier adaptation preserves those words; they have no ZGC page.
    const zaddress payload = RefField<>(observed).GetTargetObject();
    if (!is_null(payload) && !Heap::IsHeapAddress(raw(payload))) {
        return payload;
    }
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
    return ColourPredicates::is_load_good(raw(value), ::g_cjLoadBadMask) &&
           ColourPredicates::is_marked_young(raw(value), ::g_cjMarkBadMask);
}

inline zpointer Barrier::ColorMarkYoungGood(zaddress address, zpointer previous)
{
    return ColorAddressMarkYoungGood(address, previous);
}

inline void Barrier::MarkYoungGoodBarrierOnOopField(NativeSlot& field) const
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    // Cangjie NativeSlot tables also contain plain, read-only ELF literals.
    // They have no ZGC heap-root counterpart and must retain their plain word.
    BaseObject* payload = to_object(RefField<>(observed).GetTargetObject());
    if (payload != nullptr && !Heap::IsHeapAddress(payload)) {
        return;
    }
    // Retain the colored native-root admission invariant from ReadStaticRef.
    // ZPointer::assert_is_valid, zAddress.inline.hpp:320-393.
    CHECK_DETAIL(payload == nullptr ||
                     (raw(observed) & (REMAP_COLOUR_MASK | MARKED_YOUNG_MASK | MARKED_OLD_MASK)) != 0,
                 "NativeSlot requires colored value at MarkYoungGoodBarrier slot=%p word=%#zx", &field,
                 raw(observed));
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &field };
    MarkBarrier(IsMarkYoungGoodFastPath, &Barrier::MarkYoungSlowPath,
                ColorMarkYoungGood, field, observed, provenance);
}

// ZBarrier fast/color functions, zBarrier.inline.hpp:379-448.
inline bool Barrier::IsMarkGoodFastPath(zpointer value)
{
    return ColourPredicates::is_mark_good(raw(value), ::g_cjLoadBadMask, ::g_cjMarkBadMask);
}

inline bool Barrier::IsStoreGoodOrNullAnyFastPath(zpointer value)
{
    return !ColourPredicates::has_address(raw(value)) ||
           !ColourPredicates::is_store_bad(raw(value), ::g_cjStoreBadMask);
}

inline zpointer Barrier::ColorMarkGood(zaddress address, zpointer previous)
{
    if (!ColourPredicates::has_address(raw(previous))) {
        return to_zpointer(::g_cjStoreGoodMask | REMEMBERED_MASK);
    }
    return to_zpointer(raw(address) | (REMAP_COLOUR_MASK ^ ::g_cjLoadBadMask) |
                      ((MARKED_YOUNG_MASK | MARKED_OLD_MASK) & ~::g_cjMarkBadMask) | REMEMBERED_MASK);
}

inline zpointer Barrier::ColorStoreGood(zaddress address, zpointer)
{
    return to_zpointer(MakeStoreGoodSlotWord(raw(address), ::g_cjStoreGoodMask));
}

inline zpointer Barrier::ColorRemsetGood(zaddress address, zpointer previous)
{
    if (is_null(address) || RegionInfo::GetRegionInfoAt(raw(address))->IsYoungRegion()) {
        return ColorMarkGood(address, previous);
    }
    return ColorMarkYoungGood(address, previous);
}

// ZBarrier::mark_barrier_on_old_oop_field, zBarrier.inline.hpp:626-660.
inline void Barrier::MarkBarrierOnOldOopField(BaseObject* holder, RefField<>& field) const
{
    const zpointer observed = field.GetFieldValue(std::memory_order_relaxed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, holder, &field };
    const zaddress result = MarkBarrier(IsMarkGoodFastPath, &Barrier::MarkFromOldSlowPath,
                                       ColorMarkGood, field, observed, provenance);
#if defined(MRT_TESTABLE_INTERNALS)
    if (testFieldMarkResult) testFieldMarkResult(FieldMarkKind::Old, field, observed, result);
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

template<>
inline void Barrier::WriteField<int8_t>(BaseObject* obj, Field<int8_t>& field, int8_t val) const
{
    WriteI8(obj, field, val);
}

template<>
inline void Barrier::WriteField<int16_t>(BaseObject* obj, Field<int16_t>& field, int16_t val) const
{
    WriteI16(obj, field, val);
}

template<>
inline void Barrier::WriteField<int32_t>(BaseObject* obj, Field<int32_t>& field, int32_t val) const
{
    WriteI32(obj, field, val);
}

template<>
inline void Barrier::WriteField<int64_t>(BaseObject* obj, Field<int64_t>& field, int64_t val) const
{
    WriteI64(obj, field, val);
}

template<>
inline void Barrier::WriteField<float>(BaseObject* obj, Field<float>& field, float val) const
{
    WriteF32(obj, field, val);
}

template<>
inline void Barrier::WriteField<double>(BaseObject* obj, Field<double>& field, double val) const
{
    WriteF64(obj, field, val);
}
} // namespace MapleRuntime
#endif // ~MRT_BARRIER_INLINE_H
