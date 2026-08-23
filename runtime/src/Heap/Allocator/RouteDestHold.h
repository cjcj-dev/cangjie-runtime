// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_ROUTE_DEST_HOLD_H
#define MRT_ROUTE_DEST_HOLD_H

#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// Product guard for route destinations. A region named by a published route must
// not be reclaimed until the route generation drops its destination holds.
namespace RouteDestHold {

enum class Site : uint32_t {
    ASSEMBLE_RECENT_FULL = 0,
    ASSEMBLE_UNMOVABLE = 1,
    YOUNG_UNMOVABLE = 2,
    YOUNG_RECENT_FULL = 3,
    TAKE_GARBAGE = 4,
    TAKE_AFTER_DISPEL = 5,
    SITE_COUNT = 6
};

bool HoldsBack(const RegionInfo* region, Site site);

} // namespace RouteDestHold
} // namespace MapleRuntime

#endif // MRT_ROUTE_DEST_HOLD_H
