// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <cstddef>
#include <cstdio>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zPageAllocator.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct PinRootTestAccess {
    static bool OnRecentFull(RegionManager&, const ZPage* region)
    {
        return region->GetRegionRole() == ZPageRole::RecentFull;
    }
    static size_t RecentFullCount(const RegionManager&, const ZPage* region)
    {
        return region->GetRegionRole() == ZPageRole::RecentFull ? 1u : 0u;
    }

};

} // namespace MapleRuntime

// ZGC zPageAllocator.cpp:2065: completed in-place pages remain page-table visible.
GC_TEST(RegionRetirement, StayYoungAfterCompactInPlaceDoesNotRelinkRecentFull)
{
    GcHeapFixture fx;
    RegionManager manager;
    ZPage* region = fx.region0;
    region->reset(PageAge::eden);
    region->SetInGhostRegion(1);
    region->MarkForwardingDone();
    region->SetRegionRole(ZPageRole::From);
    manager.RehomeCompactedInPlaceRegion(region);
    manager.EnlistStayYoungSurvivor(region);
    GC_EXPECT_EQ(PinRootTestAccess::RecentFullCount(manager, region), 1u);
    GC_EXPECT_TRUE(PinRootTestAccess::OnRecentFull(manager, region));
}
GC_TEST(RegionRetirement, CompactTailDoesNotStealConcurrentRecentFullNode)
{
    GcHeapFixture fx;
    RegionManager manager;
    ZPage* region = fx.region0;
    region->SetRegionRole(ZPageRole::From);
    manager.RehomeCompactedInPlaceRegion(region);
    manager.EnlistCompactedRegionForAllocator(region);
    GC_EXPECT_EQ(PinRootTestAccess::RecentFullCount(manager, region), 1u);
    GC_EXPECT_TRUE(PinRootTestAccess::OnRecentFull(manager, region));
}
