// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Kept (Exempt MarkForwardingDone) is in-cycle only. Next cycle start expires it
// so the page re-enters the selector. ZGC: zRelocationSetSelector.cpp:114-196,
// zGeneration.cpp:205-213. FORWARDED receipts must not be expired.

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionManager.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct IkeKeepTestAccess {
    static void ParkUnmovable(RegionManager& manager, RegionInfo* region)
    {
        manager.unmovableFromRegionList.PrependRegion(region, RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    }
    static void ParkFrom(RegionManager& manager, RegionInfo* region)
    {
        manager.fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
    }
    static bool OnUnmovable(RegionManager& manager, const RegionInfo* region)
    {
        bool found = false;
        manager.unmovableFromRegionList.VisitAllRegions([&found, region](RegionInfo* r) {
            if (r == region) {
                found = true;
            }
        });
        return found;
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
};

} // namespace MapleRuntime

GC_TEST(IkeKeep, ExpireClearsKeptButLeavesForwarded)
{
    GcHeapFixture fx;
    RegionManager manager;

    RegionInfo* kept = fx.region0;
    kept->SetRouteState(RegionInfo::RouteState::ROUTED);
    kept->MarkForwardingDone();
    IkeKeepTestAccess::ParkUnmovable(manager, kept);

    RegionInfo* forwarded = fx.region1;
    forwarded->SetRouteState(RegionInfo::RouteState::FORWARDED);
    forwarded->MarkForwardingDone();
    IkeKeepTestAccess::ParkFrom(manager, forwarded);

    GC_EXPECT_TRUE(kept->IsForwardingDone());
    GC_EXPECT_TRUE(forwarded->IsForwardingDone());
    GC_EXPECT_TRUE(IkeKeepTestAccess::OnUnmovable(manager, kept));

    manager.ExpireKeptFromPreviousCycle();

    GC_EXPECT_FALSE(kept->IsForwardingDone());
    GC_EXPECT_EQ(static_cast<unsigned>(kept->GetRouteState()),
                 static_cast<unsigned>(RegionInfo::RouteState::NORMAL));
    GC_EXPECT_TRUE(IkeKeepTestAccess::OnUnmovable(manager, kept));

    GC_EXPECT_TRUE(forwarded->IsForwardingDone());
    GC_EXPECT_EQ(static_cast<unsigned>(forwarded->GetRouteState()),
                  static_cast<unsigned>(RegionInfo::RouteState::FORWARDED));
    GC_EXPECT_TRUE(IkeKeepTestAccess::OnFrom(manager, forwarded));
}

GC_TEST(ExemptLife, ExpireRetiresForwardedKeptTable)
{
    // After-copy Exempt parks FORWARDED+done with a live table. Next cycle
    // must not find() last cycle's dest (zRelocationSet.cpp:91-96).
    GcHeapFixture fx;
    RegionManager manager;
    ForwardingTable::Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);

    RegionInfo* forwarded = fx.region0;
    forwarded->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    forwarded->SetRouteState(RegionInfo::RouteState::FORWARDED);
    forwarded->MarkForwardingDone();
    IkeKeepTestAccess::ParkUnmovable(manager, forwarded);

    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    const MAddress stale = fx.heapStart + RegionInfo::UNIT_SIZE + 128;
    GC_EXPECT_TRUE(ForwardingTable::EntriesArmed(from));
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(from, stale), stale);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), stale);

    manager.ExpireKeptFromPreviousCycle();

    GC_EXPECT_TRUE(forwarded->IsForwardingDone());
    GC_EXPECT_EQ(static_cast<unsigned>(forwarded->GetRouteState()),
                  static_cast<unsigned>(RegionInfo::RouteState::FORWARDED));
    GC_EXPECT_FALSE(ForwardingTable::GetEntries(from) != nullptr);
    // FindTo still answers from the retired generation until the next install.
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), stale);
    ForwardingTable::DropRetiredCovering(forwarded->GetRegionStart(), forwarded->GetRegionSize());
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), static_cast<MAddress>(0));
}
