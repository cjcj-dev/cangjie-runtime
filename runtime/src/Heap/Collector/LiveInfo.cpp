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
    uint64_t toRegion1Used = GetToRegion1UsedBytes();
    if (preLiveBytes < toRegion1Used) {
        return GetToRegion1StartAddress() + preLiveBytes;
    } else { // object is routed to to-region2
        uint32_t toRegion2 = GetToRegion2Idx();
        CHECK(toRegion2 != INVALID_VALUE);
        RegionInfo* toRegion2Info = reinterpret_cast<RegionInfo*>(RegionInfo::GetRegionInfo(toRegion2));
        return toRegion2Info->GetRegionStart() + (preLiveBytes - toRegion1Used);
    }
}
} // namespace MapleRuntime
