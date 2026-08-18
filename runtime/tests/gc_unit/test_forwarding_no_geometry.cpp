// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Invariant: region in the forward set, object not copied → LookupTo is null.
// Geometry GetRoute may still invent a to; the table must not.

#include "Heap/Allocator/ForwardingTable.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ForwardingNoGeometry, ArmedMissIsNullNotGeometry)
{
    static_assert(ForwardingTable::kEntriesSoleWhenArmed, "step 3 requires sole-when-armed");

    GcHeapFixture fx;
    ForwardingTable::Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    fx.region0->SetRouteState(RegionInfo::ROUTED);
    fx.region0->SetRouteInfo(0x20000000u, 4096);

    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    size_t regionSize = fx.region0->GetRegionSize();
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);
    size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)bm->MarkBits(offset, 8, regionSize);
    fx.region0->metadata.liveInfo0 = live;
    fx.region0->metadata.regionEnd0 = fx.region0->GetRegionEnd();

    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    GC_EXPECT_TRUE(ForwardingTable::EntriesArmed(from));

    BaseObject* geometric = fx.region0->GetRouteForProbe(fx.obj0);
    GC_EXPECT_TRUE(geometric != nullptr);
    GC_EXPECT_TRUE(reinterpret_cast<MAddress>(geometric) == 0x20000000u ||
                   reinterpret_cast<MAddress>(geometric) != 0);

    ForwardingTable::ToAnswer ans = ForwardingTable::ToAnswer::Unarmed;
    const MAddress looked = ForwardingTable::LookupTo(from, &ans);
    GC_EXPECT_TRUE(ans == ForwardingTable::ToAnswer::ArmedMiss);
    GC_EXPECT_EQ(looked, static_cast<MAddress>(0));
    GC_EXPECT_TRUE(looked != reinterpret_cast<MAddress>(geometric));

    const MAddress stored = fx.heapStart + RegionInfo::UNIT_SIZE + 128;
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(from, stored), stored);
    ans = ForwardingTable::ToAnswer::Unarmed;
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from, &ans), stored);
    GC_EXPECT_TRUE(ans == ForwardingTable::ToAnswer::ArmedHit);

    fx.region0->SetRouteState(RegionInfo::NORMAL);
    fx.region0->metadata.liveInfo0 = nullptr;
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}
