// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// CI face: CHECK(del->IsFromRegion()) at CSet empty-free (RegionManager.cpp
// ExemptFromRegions). Mutator AddRawPointerObject may retype FROM→PINNED after
// the snapshot (RegionManager.h:AddRawPointerObject). Claim is TryDelete under
// the from-list lock; a lost claim parks PINNED, never frees a native-held page.
// ZGC: zGeneration.cpp:211-221 register_empty_page only if is_relocatable.

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/Allocator/RegionManager.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct IsFromRegTestAccess {
    static void ParkFrom(RegionManager& manager, RegionInfo* region)
    {
        manager.fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
    }
    static void ParkGarbage(RegionManager& manager, RegionInfo* region)
    {
        manager.garbageRegionList.PrependRegion(region, RegionInfo::RegionType::GARBAGE_REGION);
    }
    static bool OnFrom(RegionManager& manager, const RegionInfo* region)
    {
        bool found = false;
        manager.fromRegionList.VisitAllRegions([&found, region](RegionInfo* r) {
            if (r == region) {
                found = true;
            }
        });
        return found;
    }
    static bool OnPinned(RegionManager& manager, const RegionInfo* region)
    {
        bool found = false;
        manager.rawPointerPinnedRegionList.VisitAllRegions([&found, region](RegionInfo* r) {
            if (r == region) {
                found = true;
            }
        });
        return found;
    }
    static bool OnGarbage(RegionManager& manager, const RegionInfo* region)
    {
        bool found = false;
        manager.garbageRegionList.VisitAllRegions([&found, region](RegionInfo* r) {
            if (r == region) {
                found = true;
            }
        });
        return found;
    }
    static bool TryClaimFrom(RegionManager& manager, RegionInfo* region, RegionInfo::RegionType newType)
    {
        return manager.fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION, newType);
    }
    static bool TryClaimGarbage(RegionManager& manager, RegionInfo* region, RegionInfo::RegionType newType)
    {
        return manager.garbageRegionList.TryDeleteRegion(region, RegionInfo::RegionType::GARBAGE_REGION, newType);
    }
};

} // namespace MapleRuntime

GC_TEST(IsFromReg, TryDeleteFromFailsAfterPinRetype)
{
    GcHeapFixture fx;
    RegionManager manager;
    IsFromRegTestAccess::ParkFrom(manager, fx.region0);
    GC_EXPECT_TRUE(fx.region0->IsFromRegion());
    GC_EXPECT_TRUE(IsFromRegTestAccess::TryClaimFrom(manager, fx.region0,
                                                     RegionInfo::RegionType::RAW_POINTER_PINNED_REGION));
    manager.rawPointerPinnedRegionList.PrependRegion(fx.region0,
                                                     RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
    GC_EXPECT_FALSE(fx.region0->IsFromRegion());
    GC_EXPECT_FALSE(IsFromRegTestAccess::TryClaimFrom(manager, fx.region0,
                                                      RegionInfo::RegionType::GARBAGE_REGION));
    GC_EXPECT_EQ(static_cast<unsigned>(fx.region0->GetRegionType()),
                 static_cast<unsigned>(RegionInfo::RegionType::RAW_POINTER_PINNED_REGION));
    GC_EXPECT_TRUE(IsFromRegTestAccess::OnPinned(manager, fx.region0));
    GC_EXPECT_FALSE(IsFromRegTestAccess::OnFrom(manager, fx.region0));
}

GC_TEST(IsFromReg, UnlistedGarbageClaimIsRefusedUntilPrepend)
{
    GcHeapFixture fx;
    RegionManager manager;
    IsFromRegTestAccess::ParkFrom(manager, fx.region0);
    GC_EXPECT_TRUE(IsFromRegTestAccess::TryClaimFrom(manager, fx.region0,
                                                     RegionInfo::RegionType::GARBAGE_REGION));
    GC_EXPECT_EQ(static_cast<unsigned>(fx.region0->GetRegionType()),
                 static_cast<unsigned>(RegionInfo::RegionType::GARBAGE_REGION));
    GC_EXPECT_FALSE(IsFromRegTestAccess::TryClaimGarbage(manager, fx.region0,
                                                         RegionInfo::RegionType::RAW_POINTER_PINNED_REGION));
    IsFromRegTestAccess::ParkGarbage(manager, fx.region0);
    GC_EXPECT_TRUE(IsFromRegTestAccess::TryClaimGarbage(manager, fx.region0,
                                                        RegionInfo::RegionType::RAW_POINTER_PINNED_REGION));
    manager.rawPointerPinnedRegionList.PrependRegion(fx.region0,
                                                     RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
    GC_EXPECT_TRUE(IsFromRegTestAccess::OnPinned(manager, fx.region0));
    GC_EXPECT_FALSE(IsFromRegTestAccess::OnGarbage(manager, fx.region0));
}

GC_TEST(IsFromReg, TakeGarbageSkipsRawPointerHold)
{
    GcHeapFixture fx;
    RegionManager manager;
    fx.region0->IncRawPointerObjectCount();
    IsFromRegTestAccess::ParkGarbage(manager, fx.region0);
    RegionInfo* taken = manager.TakeReclaimableGarbageRegion();
    GC_EXPECT_TRUE(taken == nullptr);
    GC_EXPECT_TRUE(IsFromRegTestAccess::OnGarbage(manager, fx.region0));
    fx.region0->DecRawPointerObjectCount();
    taken = manager.TakeReclaimableGarbageRegion();
    GC_EXPECT_TRUE(taken == fx.region0);
}
