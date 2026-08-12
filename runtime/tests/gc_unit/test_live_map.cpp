// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U4 — product LiveInfo / RegionBitmap + liveInfo0 snapshot + BindLiveInfo0FromLiveIfNull.
// Product symbols: RegionBitmap::MarkBits / IsMarked, LiveInfo::IsSurvivedObject,
// RegionInfo::BindLiveInfo0FromLiveIfNull, PrepareForwardable-style ghost pointer share.

#include <cstdint>
#include <cstring>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// U4: product mark then IsSurvivedObject.
GC_TEST(LiveMap, MarkAndSurvive)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    size_t regionSize = region->GetRegionSize();
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);

    size_t off0 = 0;
    size_t off64 = 64;
    GC_EXPECT_FALSE(live->IsSurvivedObject(off0));
    GC_EXPECT_FALSE(live->IsSurvivedObject(off64));

    bool was = bm->MarkBits(off64, 8, regionSize);
    GC_EXPECT_FALSE(was);
    GC_EXPECT_TRUE(live->IsSurvivedObject(off64));
    GC_EXPECT_FALSE(live->IsSurvivedObject(off0));
    GC_EXPECT_FALSE(live->IsSurvivedObject(128));

    GC_EXPECT_TRUE(bm->MarkBits(off64, 8, regionSize));
    GC_EXPECT_TRUE(live->IsSurvivedObject(off64));
    GC_EXPECT_TRUE(bm->IsMarked(off64));

    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// U4: liveInfo0 snapshot survives clearing current liveInfo.
GC_TEST(LiveMap, LiveInfo0SnapshotSurvivesClear)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    size_t regionSize = region->GetRegionSize();
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);
    (void)bm->MarkBits(256, 8, regionSize);

    // PrepareForwardableRegion shape: liveInfo0 = liveInfo (pointer share).
    region->metadata.liveInfo0 = region->metadata.liveInfo;
    region->metadata.regionEnd0 = region->GetRegionEnd();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(region->GetLiveInfo0ForProbe()),
                 reinterpret_cast<uintptr_t>(live));
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe()->IsSurvivedObject(256));

    region->metadata.liveInfo = nullptr;
    GC_EXPECT_TRUE(region->GetLiveInfo() == nullptr);
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() != nullptr);
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe()->IsSurvivedObject(256));

    region->metadata.liveInfo0 = nullptr;
    fx.FreePlanted(live);
}

// U4: installdomain — late bind null ghost from current live.
GC_TEST(LiveMap, BindLiveInfo0AfterLateMark)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    size_t regionSize = region->GetRegionSize();

    // PrepareForwardable saw null liveInfo → ghost stays null.
    region->metadata.liveInfo = nullptr;
    region->metadata.liveInfo0 = nullptr;
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == nullptr);

    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);
    (void)bm->MarkBits(8, 8, regionSize);

    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(region->GetLiveInfo0ForProbe()),
                 reinterpret_cast<uintptr_t>(live));
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe()->IsSurvivedObject(8));

    region->metadata.liveInfo0 = nullptr;
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// U4: null markBitmap ⇒ never survived (domain reject).
GC_TEST(LiveMap, NullBitmapNeverSurvived)
{
    LiveInfo live;
    live.markBitmap = nullptr;
    live.resurrectBitmap = nullptr;
    GC_EXPECT_FALSE(live.IsSurvivedObject(0));
    GC_EXPECT_FALSE(live.IsSurvivedObject(100));
}
