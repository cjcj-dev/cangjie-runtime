// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/ImmortalWrapper.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "ForwardDataManager.h"
#include "LiveInfo.h"

namespace MapleRuntime {
uintptr_t RouteInfo::GetRoute(uint64_t preLiveBytes)
{
    if (preLiveBytes < toRegion1UsedBytes) {
        return toRegion1StartAddress + preLiveBytes;
    }
    // object is routed to to-region2.
    CHECK(toRegion2Idx != INVALID_VALUE);
    // noindirection: resolve the recorded unit index arithmetically instead of looking up
    // whatever RegionInfo owns that unit now. The index stored at RegionManager.cpp:1998 is
    // always a region head (toRegion2->GetUnitIdx()), and for a head unit the two are the
    // same expression: GetUnitAddress is heapStartAddress + idx * UNIT_SIZE, GetRegionStart
    // is idx * UNIT_SIZE + heapStartAddress. They diverge only once the recorded unit has
    // been absorbed as a SUBORDINATE_UNIT of a larger region, in which case GetRegionInfo
    // returns unit->GetMetadata().ownerRegion and the route relocates to a different, lower
    // base address than the plan was made against — a silent wrong answer rather than a
    // stale one. Arithmetic is the recorded plan's intended meaning; the lookup only ever
    // added a way for it to change underneath the reader.
    MAddress toRegion2Start = RegionInfo::GetUnitAddress(toRegion2Idx);
    RouteDestHold::NoteTo2Resolve(static_cast<uintptr_t>(toRegion2Start), toRegion2Idx);
    return toRegion2Start + (preLiveBytes - toRegion1UsedBytes);
}
} // namespace MapleRuntime
