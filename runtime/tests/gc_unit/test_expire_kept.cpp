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
#include "Heap/Collector/RelocationSetTxn.h"

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

GC_TEST(ExemptLife, DropRetiredCoveringRemovesFindTo)
{
    // After-copy Exempt ClearEntries unlinks into the retired generation.
    // FindTo still scans it; the next install must drop covering tables
    // (zRelocationSet.cpp:91-96). Addresses below any live heap base so
    // GetEntries cannot hit a previous fixture's map.
    constexpr MAddress kStart = 0x10000;
    constexpr size_t kSize = 0x1000;
    ForwardingEntries* tab = ForwardingEntries::Create(4, kStart, 0, kSize);
    GC_EXPECT_TRUE(tab != nullptr);
    const MAddress from = kStart + 16;
    const MAddress stale = 0x2000;
    GC_EXPECT_EQ(tab->insert(from, stale), stale);
    ForwardingTable::Retire(tab);
    if (RelocationSetTxn::Enabled()) {
        // Transaction authority freezes new retired-table discoverers.
        GC_EXPECT_EQ(ForwardingTable::FindTo(from), static_cast<MAddress>(0));
    } else {
        GC_EXPECT_EQ(ForwardingTable::FindTo(from), stale);
    }
    ForwardingTable::DropRetiredCovering(kStart, kSize);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), static_cast<MAddress>(0));
}
