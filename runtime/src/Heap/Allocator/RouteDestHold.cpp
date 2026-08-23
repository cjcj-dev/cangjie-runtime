// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Allocator/RouteDestHold.h"

#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
namespace RouteDestHold {

bool HoldsBack(const RegionInfo* region, Site)
{
    return region != nullptr && region->IsRouteDestHeld();
}

} // namespace RouteDestHold
} // namespace MapleRuntime
