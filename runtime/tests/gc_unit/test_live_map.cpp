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
    MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, off0));
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, off64));

    bool was = bm->MarkBits(off64, 8, regionSize);
    GC_EXPECT_FALSE(was);
    GC_EXPECT_TRUE(live->IsSurvivedObject(view, off64));
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, off0));
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, 128));

    GC_EXPECT_TRUE(bm->MarkBits(off64, 8, regionSize));
    GC_EXPECT_TRUE(live->IsSurvivedObject(view, off64));
    GC_EXPECT_TRUE(bm->IsMarked(off64));

    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// livemap: retained own-copy is the mark-time authority after the borrowed
// LiveInfo has been unbound and forwarding has retired the current mark face.
GC_TEST(LiveMap, RetainedMarkWordsSurviveUnbindAndForwardEpochBump)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bm = fx.PlantMarkBitmap(live, region->GetRegionSize());
    size_t holderOffset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)bm->MarkBits(holderOffset, 8, region->GetRegionSize());

    MarkView<Generation::Old> old = region->GetMarkView<Generation::Old>();
    region->ResetLiveMapAfterForward(old);
    GC_EXPECT_TRUE(region->HasRetainedMarkWords());
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(holderOffset));

    region->CheckAndClearLiveInfo(live);
    GC_EXPECT_FALSE(region->IsMarkedObject(region->GetMarkView<Generation::Old>(), holderOffset));
    GC_EXPECT_TRUE(region->IsRetainedSnapshotValid());
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(holderOffset));

    region->FreeRetainedMarkWords();
    fx.FreePlanted(live);
}

// unmovmark: in-place young promote captures retained words while the region
// is still young (RegionManager.cpp:3212/3252/3477, WCollector.cpp:7391).
// PromoteYoungRegion deliberately does not copy the Young face into Old
// (RegionInfo.h:2540-2544). ZGC keeps the same page livemap across
// flip-promote (zPage.cpp:103-113 reset generation_id; zPage.inline.hpp:254-256
// still reads that livemap). Capture must therefore union the current Young
// face, or UNMOVABLE_FROM holders stay bit=0 after promotion.
GC_TEST(LiveMap, RetainedCaptureUnionsYoungFaceBeforePromotion)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* youngBitmap = fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    (void)fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    size_t holderOffset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)youngBitmap->MarkBits(holderOffset, 8, region->GetRegionSize());

    region->PreserveRetainedLiveInfo();
    GC_EXPECT_TRUE(region->HasRetainedMarkWords());
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(holderOffset));

    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();
    (void)region->PromoteYoungRegion(young);
    GC_EXPECT_TRUE(region->IsRetainedSnapshotValid());
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(holderOffset));

    region->FreeRetainedMarkWords();
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// After a real copy, MarkForwardingDone is published first
// (RegionManager.cpp:3544). Young bits then name FROM copies and must
// not enter the retained snapshot (nw256 GOLD 9→3 when they did).
GC_TEST(LiveMap, RetainedCaptureSkipsYoungFaceAfterForwardingDone)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* youngBitmap = fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    (void)fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    size_t holderOffset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)youngBitmap->MarkBits(holderOffset, 8, region->GetRegionSize());

    region->MarkForwardingDone();
    region->PreserveRetainedLiveInfo();
    GC_EXPECT_FALSE(region->RetainedMarkWordsSay(holderOffset));

    region->FreeRetainedMarkWords();
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(LiveMap, RetainedCaptureUnionsYoungLargeFlagBeforePromotion)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    region->SetRegionType(RegionInfo::RegionType::RECENT_LARGE_REGION);
    region->SetYoungRegionFlag(1);
    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();
    region->SetMarkedRegionFlag(young, 1);
    region->SetRegionAllocPtr(region->GetRegionStart() + 64);
    region->AddLiveByteCount(64);

    region->PreserveRetainedLiveInfo();
    GC_EXPECT_TRUE(region->HasRetainedMarkWords());
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(0));

    region->FreeRetainedMarkWords();
}

// After promotion the region is old. A later Preserve must not resurrect
// objects the Old closure left unmarked by unioning a leftover Young face.
GC_TEST(LiveMap, RetainedCaptureDoesNotUnionYoungFaceAfterPromotion)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* youngBitmap = fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    (void)fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    size_t holderOffset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)youngBitmap->MarkBits(holderOffset, 8, region->GetRegionSize());

    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();
    (void)region->PromoteYoungRegion(young);
    region->PreserveRetainedLiveInfo();
    GC_EXPECT_FALSE(region->RetainedMarkWordsSay(holderOffset));

    region->FreeRetainedMarkWords();
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// ZGC zPage.inline.hpp:53-58: a large page contains one object at page start.
// Its retained livemap is therefore one persistent bit, including resurrection.
GC_TEST(LiveMap, RetainedLargeMarkBitSurvivesCurrentFaceLoss)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    region->SetRegionType(RegionInfo::RegionType::LARGE_REGION);
    BaseObject* holder = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(region->GetRegionStart() + holder->GetSize());
    MarkView<Generation::Old> old = region->GetMarkView<Generation::Old>();
    GC_EXPECT_FALSE(region->MarkObject(old, holder, holder->GetSize()));

    // Product write site: CollectLargeGarbage resets the one-bit face after
    // mark and ResetMarkBit must preserve it first.
    region->ResetMarkBit(old);
    GC_EXPECT_TRUE(region->HasRetainedMarkWords());
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(0));

    GC_EXPECT_FALSE(region->IsMarkedObject(old, holder));
    GC_EXPECT_TRUE(region->IsRetainedSnapshotValid());
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(0));

    region->FreeRetainedMarkWords();
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

    // Product publication shape: pointer-share plus route mark epoch/life stamp.
    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(region->GetLiveInfo0ForProbe()),
                 reinterpret_cast<uintptr_t>(live));
    GC_EXPECT_TRUE(region->IsRouteSurvivedObject(256));

    region->metadata.liveInfo = nullptr;
    GC_EXPECT_TRUE(region->GetLiveInfo() == nullptr);
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() != nullptr);
    GC_EXPECT_TRUE(region->IsRouteSurvivedObject(256));

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
    GC_EXPECT_TRUE(region->IsRouteSurvivedObject(8));

    region->metadata.liveInfo0 = nullptr;
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// U4: null markBitmap ⇒ never survived (domain reject).
GC_TEST(LiveMap, NullBitmapNeverSurvived)
{
    GcHeapFixture fx;
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    MarkView<Generation::Old> view = fx.region0->GetMarkView<Generation::Old>();
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, 0));
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, 100));
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// genface: the same ordinary region carries two independent closure faces.
GC_TEST(LiveMap, YoungAndOldBitmapFacesAreIndependent)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* youngBitmap = fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    RegionBitmap* oldBitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();
    MarkView<Generation::Old> old = region->GetMarkView<Generation::Old>();

    (void)youngBitmap->MarkBits(64, 8, region->GetRegionSize());
    (void)oldBitmap->MarkBits(128, 8, region->GetRegionSize());
    GC_EXPECT_TRUE(region->IsMarkedObject(young, 64));
    GC_EXPECT_FALSE(region->IsMarkedObject(old, 64));
    GC_EXPECT_TRUE(region->IsMarkedObject(old, 128));
    GC_EXPECT_FALSE(region->IsMarkedObject(young, 128));

    region->ClearLiveInfo(young);
    MarkView<Generation::Young> nextYoung = region->GetMarkView<Generation::Young>();
    GC_EXPECT_TRUE(region->GetMarkBitmap(nextYoung) == nullptr);
    GC_EXPECT_TRUE(region->IsMarkedObject(old, 128));

    // Product bitmaps are arena-owned; this fixture planted them with calloc.
    youngBitmap->~RegionBitmap();
    std::free(youngBitmap);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// genface promotion boundary: a Young mark may remain readable through the
// already-captured historical token (PromotedRegionDomain needs that), but it
// cannot leak into the newly authoritative Old face.
GC_TEST(LiveMap, PromotionDoesNotInheritYoungBitmapMark)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* youngBitmap = fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    RegionBitmap* oldBitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();

    (void)youngBitmap->MarkBits(64, 8, region->GetRegionSize());
    MarkView<Generation::Old> old = region->PromoteYoungRegion(young);
    GC_EXPECT_TRUE(region->IsMarkedObject(young, 64));
    GC_EXPECT_FALSE(region->IsMarkedObject(old, 64));

    // The promoted object becomes marked on the Old face only when the Old
    // collector itself writes that independent face.
    (void)oldBitmap->MarkBits(64, 8, region->GetRegionSize());
    GC_EXPECT_TRUE(region->IsMarkedObject(old, 64));

    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(LiveMap, PromotionDoesNotInheritYoungLargeFlag)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    region->SetRegionType(RegionInfo::RegionType::RECENT_LARGE_REGION);
    region->SetYoungRegionFlag(1);
    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();

    region->SetMarkedRegionFlag(young, 1);
    MarkView<Generation::Old> old = region->PromoteYoungRegion(young);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(young), 1u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 0u);

    region->SetMarkedRegionFlag(old, 1);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 1u);
}

// genface face B: large regions use two independent single-bit flags, not bitmaps.
GC_TEST(LiveMap, YoungAndOldLargeFlagsAreIndependent)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    region->SetRegionType(RegionInfo::RegionType::RECENT_LARGE_REGION);
    region->SetYoungRegionFlag(1);
    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();
    MarkView<Generation::Old> old = region->GetMarkView<Generation::Old>();

    region->SetMarkedRegionFlag(young, 1);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(young), 1u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 0u);
    region->SetMarkedRegionFlag(old, 1);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(young), 1u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 1u);

    region->ClearLiveInfo(young);
    MarkView<Generation::Young> nextYoung = region->GetMarkView<Generation::Young>();
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(nextYoung), 0u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 1u);

    region->SetMarkedRegionFlag(nextYoung, 1);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(nextYoung), 1u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(young), 0u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 1u);
}

// markwater: ClearLiveInfo snapshots allocPtr. Objects at offset ≥ water
// are allocate-black (ZGC zPage is_allocating). A stale view must not
// inherit that verdict (oracleblack2 b-face).
GC_TEST(LiveMap, MarkStartAllocWaterIsImplicitLive)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    MAddress start = region->GetRegionStart();
    region->SetRegionAllocPtr(start + 128);
    GC_EXPECT_EQ(region->GetMarkStartAllocPtr(), 0u);

    MarkView<Generation::Old> stale = region->GetMarkView<Generation::Old>();
    region->ClearLiveInfo(stale);
    GC_EXPECT_EQ(region->GetMarkStartAllocPtr(), start + 128);

    region->SetRegionAllocPtr(start + 256);
    MarkView<Generation::Old> current = region->GetMarkView<Generation::Old>();
    GC_EXPECT_TRUE(region->HasMarkStartAllocGap());
    GC_EXPECT_FALSE(region->IsKnownEmpty(current));
    GC_EXPECT_FALSE(region->AllocatedAfterMarkStart(64));
    GC_EXPECT_TRUE(region->AllocatedAfterMarkStart(128));
    GC_EXPECT_TRUE(region->AllocatedAfterMarkStart(192));
    GC_EXPECT_FALSE(region->IsMarkedObject(current, static_cast<size_t>(64)));
    GC_EXPECT_TRUE(region->IsMarkedObject(current, static_cast<size_t>(128)));
    GC_EXPECT_TRUE(region->IsSurvivedObject(current, static_cast<size_t>(192)));
    GC_EXPECT_TRUE(region->IsRouteSurvivedObject(128));
    // Stale view (pre-ClearLiveInfo epoch) must not treat post-water as marked.
    GC_EXPECT_FALSE(region->IsMarkedObject(stale, static_cast<size_t>(128)));
    GC_EXPECT_FALSE(region->IsSurvivedObject(stale, static_cast<size_t>(192)));
}
