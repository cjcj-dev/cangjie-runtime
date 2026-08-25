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
#include <algorithm>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// Port of test_zLiveMap.cpp's one-object large-page invariant onto the
// Cangjie RegionBitmap representation.  The first mark makes the only object
// live and accounts its bytes exactly once; repeating it is idempotent.
GC_TEST(ZLiveMapPort, OneObjectPageMarkAccountsLiveOnce)
{
    constexpr size_t kPageSize = 4096;
    RegionBitmap* bitmap = GcHeapFixture::AllocPlantedBitmap(kPageSize);

    GC_EXPECT_FALSE(bitmap->IsMarked(0));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), static_cast<size_t>(0));
    GC_EXPECT_FALSE(bitmap->MarkBits(0, kPageSize, kPageSize));
    GC_EXPECT_TRUE(bitmap->IsMarked(0));
    GC_EXPECT_TRUE(bitmap->IsMarked(kPageSize - kMarkedBytesPerBit));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), kPageSize);
    GC_EXPECT_EQ(bitmap->RecomputeLiveBytes(), kPageSize);

    GC_EXPECT_TRUE(bitmap->MarkBits(0, kPageSize, kPageSize));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), kPageSize);
    GcHeapFixture::FreePlantedBitmap(bitmap);
}

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

// An unmovable Young page may preserve its current owner livemap before the
// promotion replacement; the retained copy is independent of both page lives.
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

// Relocsel's unselected-page arm may carry a live-byte census without a
// current mark face.  The bounded preserve records that this page was not
// examined instead of applying PreserveRetainedLiveInfo's examined-page CHECK.
GC_TEST(LiveMap, UnexaminedRelocselPageKeepsWithoutSnapshot)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    region->AddLiveByteCount(64);
    GC_EXPECT_FALSE(region->HasEverPreservedRetainedLiveInfo());
    region->PreserveRetainedLiveInfoUpTo(
        std::min(region->GetCensusBoundary(), region->GetRegionAllocPtr()));
    GC_EXPECT_TRUE(region->GetRetainedLiveInfo() == nullptr);
    GC_EXPECT_FALSE(region->HasEverPreservedRetainedLiveInfo());
    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRetainedLiveInfoState()),
                 static_cast<unsigned>(RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED));
}

// Positive control: first publish a valid snapshot, then clear/unbind its
// borrowed LiveInfo.  The bounded API must retain the examined-then-lost
// distinction and abort; this is not the initial NEVER_EXAMINED fixture.
GC_TEST(LiveMap, ExaminedPageWithoutSnapshotStillAborts)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bm = fx.PlantMarkBitmap(live, region->GetRegionSize());
    (void)bm->MarkBits(0, 8, region->GetRegionSize());
    GC_EXPECT_FALSE(region->HasEverPreservedRetainedLiveInfo());
    region->PreserveRetainedLiveInfo();
    GC_EXPECT_TRUE(region->HasEverPreservedRetainedLiveInfo());
    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRetainedLiveInfoState()),
                 static_cast<unsigned>(RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID));
    // The production unbind path may have no owned carrier (for example
    // after its arena is retired); exercise that borrowed-pointer loss path
    // explicitly before CheckAndClearLiveInfo stamps SNAPSHOT_LOST.
    region->FreeRetainedMarkWords();
    region->CheckAndClearLiveInfo(live);
    region->AddLiveByteCount(64);
    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRetainedLiveInfoState()),
                 static_cast<unsigned>(RegionInfo::RetainedLiveInfoState::SNAPSHOT_LOST));
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        region->PreserveRetainedLiveInfoUpTo(
            std::min(region->GetCensusBoundary(), region->GetRegionAllocPtr()));
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_TRUE(waitpid(pid, &status, 0) == pid);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    fx.FreePlanted(live);
}

// Owned-copy positive arm: CheckAndClearLiveInfo deliberately returns early
// while the private bitmap still carries the valid snapshot.  The following
// bounded Preserve replaces that owned carrier, finds no current LiveInfo,
// and must derive LOST from the monotonic ever-preserved bit.
GC_TEST(LiveMap, OwnedCopyExaminedPageWithoutSnapshotStillAborts)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bm = fx.PlantMarkBitmap(live, region->GetRegionSize());
    (void)bm->MarkBits(0, 8, region->GetRegionSize());
    GC_EXPECT_FALSE(region->HasEverPreservedRetainedLiveInfo());
    region->PreserveRetainedLiveInfo();
    GC_EXPECT_TRUE(region->HasEverPreservedRetainedLiveInfo());
    GC_EXPECT_TRUE(region->HasRetainedMarkWords());
    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRetainedLiveInfoState()),
                 static_cast<unsigned>(RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID));

    region->CheckAndClearLiveInfo(live);
    GC_EXPECT_TRUE(region->HasRetainedMarkWords());
    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRetainedLiveInfoState()),
                 static_cast<unsigned>(RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID));
    region->AddLiveByteCount(64);
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        region->PreserveRetainedLiveInfoUpTo(
            std::min(region->GetCensusBoundary(), region->GetRegionAllocPtr()));
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_TRUE(waitpid(pid, &status, 0) == pid);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    region->FreeRetainedMarkWords();
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

    region->RetireFromPageMetadata();
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
    region->RetireFromPageMetadata();
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == nullptr);

    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);
    (void)bm->MarkBits(8, 8, regionSize);

    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(region->GetLiveInfo0ForProbe()),
                 reinterpret_cast<uintptr_t>(live));
    GC_EXPECT_TRUE(region->IsRouteSurvivedObject(8));

    region->RetireFromPageMetadata();
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

// Typed views do not select independent storage. A current page has one
// livemap and its owner metadata decides which closure may paint it.
GC_TEST(LiveMap, CurrentPageHasSingleBitmap)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();
    MarkView<Generation::Old> old = region->GetMarkView<Generation::Old>();

    (void)bitmap->MarkBits(64, 8, region->GetRegionSize());
    GC_EXPECT_TRUE(region->IsMarkedObject(young, 64));
    GC_EXPECT_TRUE(region->IsMarkedObject(old, 64));

    region->ClearLiveInfo(young);
    MarkView<Generation::Young> nextYoung = region->GetMarkView<Generation::Young>();
    GC_EXPECT_TRUE(region->GetMarkBitmap(nextYoung) == nullptr);
    GC_EXPECT_FALSE(region->IsMarkedObject(old, 64));
    fx.FreePlanted(live);
}

// pageown / ZGC zPage.inline.hpp:284-294: the target page, not the
// collector closure, chooses the physical mark authority.
GC_TEST(LiveMap, OwnerDispatchMarksYoungFace)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    LiveInfo* live = fx.PlantLiveInfo(region);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    (void)fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));

    GC_EXPECT_FALSE(region->MarkObjectByOwner(fx.obj0, fx.obj0->GetSize()));
    GC_EXPECT_TRUE(region->IsMarkedObject(region->GetMarkView<Generation::Young>(), offset));
    GC_EXPECT_TRUE(region->IsMarkedObject(region->GetMarkView<Generation::Old>(), offset));
    GC_EXPECT_TRUE(region->MarkFaceMatchesOwner<Generation::Young>());
    GC_EXPECT_FALSE(region->MarkFaceMatchesOwner<Generation::Old>());

    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// The from-page carrier owns both generation and livemap for either owner.
GC_TEST(LiveMap, OldForwardingCarrierPublishesOwnerAndRetires)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)bitmap->MarkBits(offset, fx.obj0->GetSize(), region->GetRegionSize());

    MarkView<Generation::Old> old = region->GetMarkView<Generation::Old>();
    region->PublishForwardingCarrier(old);
    GC_EXPECT_TRUE(region->HasFromPageMetadata());
    GC_EXPECT_EQ(region->GetRouteMarkGeneration(), Generation::Old);
    GC_EXPECT_TRUE(region->IsRouteSurvivedObject(offset));
    {
        RegionInfo::RetainScope retained(region);
        GC_EXPECT_TRUE(retained.ok());
        GC_EXPECT_TRUE(region->IsRouteSurvivedObject(offset));
    }

    region->DispelGhostFromRegion();
    GC_EXPECT_FALSE(region->HasFromPageMetadata());
    RegionInfo::RetainScope released(region);
    GC_EXPECT_FALSE(released.ok());
    // With no owner replacement, the current Old page still owns this same
    // livemap; this read is current-page fallback, not carrier access.
    GC_EXPECT_TRUE(region->IsRouteSurvivedObject(offset));
    fx.FreePlanted(live);
}

// Young forwarding keeps its original owner in the carrier across promotion.
GC_TEST(LiveMap, FromPageOwnerAndLivemapStayIdenticalAcrossPromotion)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* young = fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)young->MarkBits(offset, fx.obj0->GetSize(), region->GetRegionSize());
    MarkView<Generation::Young> ownerView = region->GetMarkView<Generation::Young>();
    region->PublishForwardingCarrier(ownerView);

    GC_EXPECT_EQ(region->GetRouteMarkGeneration(), Generation::Young);
    GC_EXPECT_TRUE(region->IsOwnerSurvivedObject(offset));
    GC_EXPECT_FALSE(region->GetOwnerMarkBitmap(live) == nullptr);

    (void)region->PromoteYoungRegion(ownerView);
    GC_EXPECT_TRUE(region->IsOwnerSurvivedObject(offset));

    region->RetireFromPageMetadata();
    fx.FreePlanted(live);
}

// Full product lifecycle: mark the current Young page, publish forwarding
// metadata, replace current metadata on promotion, clear the new Old current
// metadata, then retire forwarding. The Young bits belong only to the carrier.
GC_TEST(LiveMap, PromotionCarrierLivesUntilForwardingRelease)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* youngLive = fx.PlantLiveInfo(region);
    (void)fx.PlantMarkBitmap<Generation::Young>(youngLive, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));

    GC_EXPECT_FALSE(region->MarkObjectByOwner(fx.obj0, fx.obj0->GetSize()));
    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();
    region->PublishForwardingCarrier(young);
    GC_EXPECT_TRUE(region->IsRouteSurvivedObject(offset));

    MarkView<Generation::Old> old = region->PromoteYoungRegion(young);
    GC_EXPECT_TRUE(region->GetLiveInfo() == nullptr);
    GC_EXPECT_TRUE(region->GetMarkBitmap(old) == nullptr);
    GC_EXPECT_TRUE(region->IsRouteSurvivedObject(offset));

    region->ClearLiveInfo(old);
    GC_EXPECT_TRUE(region->GetMarkBitmap(region->GetMarkView<Generation::Old>()) == nullptr);
    {
        RegionInfo::RetainScope retained(region);
        GC_EXPECT_TRUE(retained.ok());
        GC_EXPECT_TRUE(region->IsRouteSurvivedObject(offset));
    }

    region->DispelGhostFromRegion();
    GC_EXPECT_FALSE(region->HasFromPageMetadata());
    GC_EXPECT_FALSE(region->IsRouteSurvivedObject(offset));
    RegionInfo::RetainScope released(region);
    GC_EXPECT_FALSE(released.ok());

    fx.FreePlanted(youngLive);

    // The same replacement rule applies to the one-bit large-page livemap.
    GcHeapFixture largeFx;
    RegionInfo* large = largeFx.region0;
    large->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    large->SetRegionType(RegionInfo::RegionType::RECENT_LARGE_REGION);
    large->SetYoungRegionFlag(1);
    MarkView<Generation::Young> largeYoung = large->GetMarkView<Generation::Young>();
    large->SetMarkedRegionFlag(largeYoung, 1);
    large->PublishForwardingCarrier(largeYoung);
    MarkView<Generation::Old> largeOld = large->PromoteYoungRegion(largeYoung);
    GC_EXPECT_EQ(large->GetMarkedRegionFlag(largeYoung), 1u);
    GC_EXPECT_EQ(large->GetMarkedRegionFlag(largeOld), 0u);
    large->RetireFromPageMetadata();
}

// Large pages follow the same single-livemap rule, represented by one bit.
GC_TEST(LiveMap, CurrentLargePageHasSingleMarkBit)
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
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 1u);

    region->ClearLiveInfo(young);
    MarkView<Generation::Young> nextYoung = region->GetMarkView<Generation::Young>();
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(nextYoung), 0u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 0u);

    region->SetMarkedRegionFlag(nextYoung, 1);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(nextYoung), 1u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(young), 0u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 0u);
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
