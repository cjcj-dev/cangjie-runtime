// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U3 — product RouteInfo::GetRoute + RegionInfo domain gate.
// Product symbols: MapleRuntime::RouteInfo::GetRoute, RegionInfo::GetRoute,
// RegionInfo::SetRouteInfo, RegionInfo::BindLiveInfo0FromLiveIfNull.
// Defect anchor: GetRoute nullptr ⇒ ior; installdomain BindLiveInfo0FromLiveIfNull.

#include <cstdint>
#include <cstring>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// U3: product RouteInfo::GetRoute region1 geometry (LiveInfo.cpp:15-18).
GC_TEST(RouteInfo, InsertLookupIdempotentRegion1)
{
    RouteInfo ri;
    constexpr uintptr_t kToStart = 0x20000000u;
    constexpr uint32_t kUsed = 4096;
    ri.SetRouteInfo(kToStart, kUsed);

    GC_EXPECT_EQ(ri.GetRoute(0), kToStart);
    GC_EXPECT_EQ(ri.GetRoute(64), kToStart + 64);
    GC_EXPECT_EQ(ri.GetRoute(4095), kToStart + 4095);
    GC_EXPECT_EQ(ri.GetRoute(64), kToStart + 64);
}

// U3: product RegionInfo::GetRoute domain gate — null ghost / unmarked ⇒ nullptr.
GC_TEST(RouteInfo, MissingDomainReturnsNullNotGarbage)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    BaseObject* obj = fx.obj0;
    region->SetRouteInfo(0x20000000u, 4096);

    // No ghost liveInfo0 → reject even if route geometry is set.
    GC_EXPECT_TRUE(region->GetRoute(obj) == nullptr);

    LiveInfo* live = fx.PlantLiveInfo(region);
    size_t regionSize = region->GetRegionSize();
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);
    size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(obj));
    // Marked but ghost still null → still reject.
    (void)bm->MarkBits(offset, 8, regionSize);
    GC_EXPECT_TRUE(region->GetRoute(obj) == nullptr);

    // Bind ghost; still unmarked offset stays null — re-use unmarked sibling.
    BaseObject* sibling = fx.PlaceObject(reinterpret_cast<MAddress>(obj) + 128);
    region->metadata.liveInfo0 = live;
    region->metadata.regionEnd0 = region->GetRegionEnd();
    GC_EXPECT_TRUE(region->GetRoute(sibling) == nullptr);

    // Survivor in domain → product route geometry (RouteInfo::GetRoute via RegionInfo::GetRoute).
    BaseObject* to = region->GetRoute(obj);
    GC_EXPECT_TRUE(to != nullptr);
    uintptr_t pre = region->GetPreLiveBytesInGhostRegion(reinterpret_cast<MAddress>(obj));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(to), 0x20000000u + pre);

    region->metadata.liveInfo0 = nullptr;
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// U3: product SetRouteInfo overwrites geometry.
GC_TEST(RouteInfo, SetRouteInfoOverwrites)
{
    RouteInfo ri;
    ri.SetRouteInfo(0x1000u, 100);
    GC_EXPECT_EQ(ri.GetRoute(10), 0x1000u + 10u);
    ri.SetRouteInfo(0x9000u, 100);
    GC_EXPECT_EQ(ri.GetRoute(10), 0x9000u + 10u);
}

// U4 surface: product BindLiveInfo0FromLiveIfNull on RegionInfo.
GC_TEST(RouteInfo, BindLiveInfo0FromLiveIfNull)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    LiveInfo* liveA = fx.PlantLiveInfo(region);
    LiveInfo* liveB = new LiveInfo();
    liveB->bindedRegion = region;

    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == nullptr);
    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(region->GetLiveInfo0ForProbe()),
                 reinterpret_cast<uintptr_t>(liveA));

    region->metadata.liveInfo = liveB;
    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(region->GetLiveInfo0ForProbe()),
                 reinterpret_cast<uintptr_t>(liveA));

    region->metadata.liveInfo0 = nullptr;
    region->metadata.liveInfo = nullptr;
    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == nullptr);

    region->metadata.liveInfo = liveA;
    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(region->GetLiveInfo0ForProbe()),
                 reinterpret_cast<uintptr_t>(liveA));

    region->metadata.liveInfo0 = nullptr;
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(liveA);
    delete liveB;
}
