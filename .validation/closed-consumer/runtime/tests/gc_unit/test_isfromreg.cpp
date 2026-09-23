// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// CI face: reclaim claim is a role CAS (#710).
// ZGC: zGeneration.cpp:211-221 register_empty_page only if is_relocatable.

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zPageAllocator.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct IsFromRegTestAccess {
    static void ParkFrom(RegionManager&, ZPage* region)
    {
        region->SetRegionRole(ZPageRole::From);
    }
    static void ParkGarbage(RegionManager&, ZPage* region)
    {
        region->SetRegionRole(ZPageRole::Garbage);
    }
    static bool OnFrom(RegionManager&, const ZPage* region)
    {
        return region->GetRegionRole() == ZPageRole::From;
    }
    static bool OnGarbage(RegionManager&, const ZPage* region)
    {
        return region->GetRegionRole() == ZPageRole::Garbage;
    }
    static bool TryClaimFrom(RegionManager&, ZPage* region, int)
    {
        ZPageRole expect = ZPageRole::From;
        return region->CASRegionRole(expect, ZPageRole::None);
    }
    static bool TryClaimGarbage(RegionManager&, ZPage* region, int)
    {
        ZPageRole expect = ZPageRole::Garbage;
        return region->CASRegionRole(expect, ZPageRole::None);
    }
};

} // namespace MapleRuntime

GC_TEST(IsFromReg, UnlistedGarbageClaimIsRefusedUntilPrepend)
{
    GcHeapFixture fx;
    RegionManager manager;
    IsFromRegTestAccess::ParkFrom(manager, fx.region0);
    GC_EXPECT_TRUE(IsFromRegTestAccess::TryClaimFrom(manager, fx.region0, 0));
    GC_EXPECT_FALSE(IsFromRegTestAccess::TryClaimGarbage(manager, fx.region0, 0));
    IsFromRegTestAccess::ParkGarbage(manager, fx.region0);
    GC_EXPECT_TRUE(IsFromRegTestAccess::TryClaimGarbage(manager, fx.region0, 0));
    GC_EXPECT_FALSE(IsFromRegTestAccess::OnGarbage(manager, fx.region0));
}
