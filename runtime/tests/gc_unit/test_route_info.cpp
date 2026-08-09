// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U3 — forward / route table invariants (HotSpot test_zForwarding.cpp shape).
// Defect anchor: GetRoute returning nullptr ⇒ ior (floor primary signature);
// RegionInfo::GetRoute must return nullptr for out-of-domain, never garbage.

#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include "gc_unittest.hpp"

using namespace MapleRuntime::GcUnit;

namespace {

// Stand-alone RouteInfo matching LiveInfo.h:230-245 / LiveInfo.cpp:15-24 region1 path.
// Region2 path needs live RegionInfo table — out of scope for header-isolated unit tests;
// domain reject (nullptr) is the floor-critical half and is modelled below.
struct RouteInfo {
    static constexpr uint32_t INVALID_VALUE = std::numeric_limits<uint32_t>::max();
    uintptr_t toRegion1StartAddress = 0;
    uint32_t toRegion1UsedBytes = 0;
    uint32_t toRegion2Idx = INVALID_VALUE;

    void SetRouteInfo(uintptr_t to1, uint32_t to1used = 0, uint32_t to2 = INVALID_VALUE)
    {
        toRegion1StartAddress = to1;
        toRegion1UsedBytes = to1used;
        toRegion2Idx = to2;
    }

    // LiveInfo.cpp:15-18 — region1 branch only (preLiveBytes < toRegion1UsedBytes).
    uintptr_t GetRouteRegion1(uint64_t preLiveBytes) const
    {
        if (preLiveBytes < toRegion1UsedBytes) {
            return toRegion1StartAddress + static_cast<uintptr_t>(preLiveBytes);
        }
        return 0; // signal "not region1" without pulling RegionInfo::GetRegionInfo
    }
};

// Model of RegionInfo::GetRoute domain gate (RegionInfo.h:812-914):
// ghost liveInfo0 null OR !IsSurvivedObject(offset) ⇒ nullptr, never a forged to-addr.
struct DomainRoute {
    bool survived = false;
    bool hasGhostLiveInfo0 = false;
    RouteInfo route;

    uintptr_t Lookup(uint64_t preLiveBytes) const
    {
        if (!hasGhostLiveInfo0 || !survived) {
            return 0; // nullptr
        }
        return route.GetRouteRegion1(preLiveBytes);
    }
};

// installdomain: BindLiveInfo0FromLiveIfNull — only binds when liveInfo0 is null and live exists.
struct LiveInfoBindModel {
    void* liveInfo0 = nullptr;
    void* liveInfo = nullptr;

    void BindLiveInfo0FromLiveIfNull()
    {
        if (liveInfo0 != nullptr) {
            return;
        }
        if (liveInfo == nullptr) {
            return;
        }
        liveInfo0 = liveInfo;
    }
};

} // namespace

// U3: insert → lookup → idempotent address for survivors in region1.
GC_TEST(RouteInfo, InsertLookupIdempotentRegion1)
{
    RouteInfo ri;
    constexpr uintptr_t kToStart = 0x20000000u;
    constexpr uint32_t kUsed = 4096;
    ri.SetRouteInfo(kToStart, kUsed);

    GC_EXPECT_EQ(ri.GetRouteRegion1(0), kToStart);
    GC_EXPECT_EQ(ri.GetRouteRegion1(64), kToStart + 64);
    GC_EXPECT_EQ(ri.GetRouteRegion1(4095), kToStart + 4095);
    // Same inputs again — pure function, idempotent.
    GC_EXPECT_EQ(ri.GetRouteRegion1(64), kToStart + 64);
}

// U3: not inserted / out of domain must not invent a to-address (nullptr, not garbage).
GC_TEST(RouteInfo, MissingDomainReturnsNullNotGarbage)
{
    DomainRoute d;
    d.route.SetRouteInfo(0x20000000u, 4096);
    // No ghost liveInfo0 → reject even if route geometry is set.
    GC_EXPECT_EQ(d.Lookup(0), 0u);

    d.hasGhostLiveInfo0 = true;
    d.survived = false; // marked miss
    GC_EXPECT_EQ(d.Lookup(0), 0u);

    d.survived = true;
    GC_EXPECT_EQ(d.Lookup(128), 0x20000000u + 128u);
}

// U3: re-SetRouteInfo overwrites geometry (second prepare must not keep stale to-start).
GC_TEST(RouteInfo, SetRouteInfoOverwrites)
{
    RouteInfo ri;
    ri.SetRouteInfo(0x1000u, 100);
    GC_EXPECT_EQ(ri.GetRouteRegion1(10), 0x1000u + 10u);
    ri.SetRouteInfo(0x9000u, 100);
    GC_EXPECT_EQ(ri.GetRouteRegion1(10), 0x9000u + 10u);
}

// U4 helper surface used by installdomain: bind only when ghost is null.
GC_TEST(RouteInfo, BindLiveInfo0FromLiveIfNull)
{
    LiveInfoBindModel m;
    int liveA = 1;
    int liveB = 2;
    m.liveInfo = &liveA;
    m.BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(m.liveInfo0), reinterpret_cast<uintptr_t>(&liveA));

    // Already bound — do not rebind to a different live.
    m.liveInfo = &liveB;
    m.BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(m.liveInfo0), reinterpret_cast<uintptr_t>(&liveA));

    // Null live leaves ghost null.
    LiveInfoBindModel m2;
    m2.BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(m2.liveInfo0), 0u);
}
