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
                                 NativeSlot& field, zpointer observed) const
{
    // Cangjie NativeSlot tables also contain plain, read-only ELF literals.
    // They have no ZGC heap-root counterpart and must retain their plain word.
    BaseObject* payload = to_object(RefField<>(observed).GetTargetObject());
    if (payload != nullptr && !Heap::IsHeapAddress(payload)) {
        return from_object(payload);
    }
    // Retain the colored native-root admission invariant from ReadStaticRef.
    // ZPointer::assert_is_valid, zAddress.inline.hpp:320-393.
    CHECK_DETAIL(payload == nullptr ||
                     (raw(observed) & (ZPointerRemappedMask | ZPointerMarkedYoungMask | ZPointerMarkedOldMask)) != 0,
                 "NativeSlot requires colored value at MarkYoungGoodBarrier slot=%p word=%#zx", &field,
                 raw(observed));
    if (fast(observed)) {
        return RefField<>(observed).GetTargetObject();
    }
    RefField<> value(observed);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &field };
    const zaddress loadGood = from_object(theCollector.make_load_good(value, provenance));
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
    MarkBarrier(IsMarkYoungGoodFastPath, &Barrier::MarkYoungSlowPath,
                       ColorMarkYoungGood, field, observed);
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
