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

} // namespace MapleRuntime
#endif // ~MRT_BARRIER_INLINE_H
