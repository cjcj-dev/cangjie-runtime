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
#include <atomic>
#include <csignal>
#include <dlfcn.h>
#include <limits>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// Do not materialize product inline/template bodies in this test executable.
// Product-path calls must resolve from libcangjie-runtime.so, so the test ELF
// cannot self-satisfy the product symbols with weak definitions.
namespace MapleRuntime {
extern template bool RegionInfo::MarkObject<Generation::Young>(
    MarkView<Generation::Young>, const BaseObject*, size_t, bool);
extern template bool RegionInfo::MarkObject<Generation::Old>(
    MarkView<Generation::Old>, const BaseObject*, size_t, bool);
extern template bool RegionInfo::MarkObject<Generation::Young>(
    MarkView<Generation::Young>, const BaseObject*);
extern template bool RegionInfo::MarkObject<Generation::Old>(
    MarkView<Generation::Old>, const BaseObject*);
extern template void RegionInfo::ClearLiveInfo<Generation::Young>(MarkView<Generation::Young>);
extern template void RegionInfo::ClearLiveInfo<Generation::Old>(MarkView<Generation::Old>);
} // namespace MapleRuntime

namespace {

void* ProductRuntimeHandle()
{
    static void* handle = []() {
        void* h = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
        if (h == nullptr) {
            h = dlopen("libcangjie-runtime.so", RTLD_NOW);
        }
        return h;
    }();
    return handle;
}

template<Generation G>
using ProductMarkObject = bool (*)(RegionInfo*, MarkView<G>, const BaseObject*, size_t, bool);

template<Generation G>
ProductMarkObject<G> ProductMarkObjectFn();

template<>
ProductMarkObject<Generation::Young> ProductMarkObjectFn<Generation::Young>()
{
    static auto fn = reinterpret_cast<ProductMarkObject<Generation::Young>>(dlsym(
        ProductRuntimeHandle(),
        "_ZN12MapleRuntime10RegionInfo10MarkObjectILNS_10GenerationE0EEEbNS_8MarkViewIXT_EEEPKNS_10BaseObjectEmb"));
    return fn;
}

template<>
ProductMarkObject<Generation::Old> ProductMarkObjectFn<Generation::Old>()
{
    static auto fn = reinterpret_cast<ProductMarkObject<Generation::Old>>(dlsym(
        ProductRuntimeHandle(),
        "_ZN12MapleRuntime10RegionInfo10MarkObjectILNS_10GenerationE1EEEbNS_8MarkViewIXT_EEEPKNS_10BaseObjectEmb"));
    return fn;
}

using ProductClearLiveInfo = void (*)(RegionInfo*, MarkView<Generation::Young>);
using ProductPreserveRetained = void (*)(RegionInfo*);
using ProductPreserveRetainedUpTo = void (*)(RegionInfo*, MAddress);
using ProductBumpEpoch = void (*)(RegionInfo*);

ProductClearLiveInfo ProductClearLiveInfoFn()
{
    static auto fn = reinterpret_cast<ProductClearLiveInfo>(dlsym(
        ProductRuntimeHandle(),
        "_ZN12MapleRuntime10RegionInfo13ClearLiveInfoILNS_10GenerationE0EEEvNS_8MarkViewIXT_EEE"));
    return fn;
}

ProductPreserveRetained ProductPreserveRetainedFn()
{
    static auto fn = reinterpret_cast<ProductPreserveRetained>(dlsym(
        ProductRuntimeHandle(), "_ZN12MapleRuntime10RegionInfo24PreserveRetainedLiveInfoEv"));
    return fn;
}

ProductPreserveRetainedUpTo ProductPreserveRetainedUpToFn()
{
    static auto fn = reinterpret_cast<ProductPreserveRetainedUpTo>(dlsym(
        ProductRuntimeHandle(), "_ZN12MapleRuntime10RegionInfo28PreserveRetainedLiveInfoUpToEm"));
    return fn;
}

ProductBumpEpoch ProductBumpEpochFn()
{
    static auto fn = reinterpret_cast<ProductBumpEpoch>(dlsym(
        ProductRuntimeHandle(), "_ZN12MapleRuntime10RegionInfo31BumpSnapshotEpochFromInitRegionEv"));
    return fn;
}

} // namespace

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

// ZLiveMap::set uses a bit pair: finalizable paints only live, while a
// subsequent strong mark upgrades the same pair without charging bytes twice.
GC_TEST(ZLiveMapPort, FinalizableAndStrongShareOnePair)
{
    constexpr size_t kPageSize = 4096;
    RegionBitmap* bitmap = GcHeapFixture::AllocPlantedBitmap(kPageSize);
    bool incLive = false;

    GC_EXPECT_FALSE(bitmap->MarkFinalizableBits(64, 16, kPageSize, incLive));
    GC_EXPECT_TRUE(incLive);
    GC_EXPECT_TRUE(bitmap->IsLive(64));
    GC_EXPECT_TRUE(bitmap->IsFinalizable(64));
    GC_EXPECT_FALSE(bitmap->IsMarked(64));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), static_cast<size_t>(16));

    GC_EXPECT_FALSE(bitmap->MarkBits(64, 16, kPageSize, incLive));
    GC_EXPECT_FALSE(incLive);
    GC_EXPECT_TRUE(bitmap->IsLive(64));
    GC_EXPECT_FALSE(bitmap->IsFinalizable(64));
    GC_EXPECT_TRUE(bitmap->IsMarked(64));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), static_cast<size_t>(16));

    GcHeapFixture::FreePlantedBitmap(bitmap);
}

// SATB producers may publish the same object more than once. The strong half
// of the pair is the consumer-side receipt: exactly one 0->1 owns live-byte
// accounting, matching ZLiveMap::set/par_set_bit_pair.
GC_TEST(ZLiveMapPort, DuplicateSatbPublicationConvergesAtStrongMark)
{
    GcHeapFixture fx;
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    RegionBitmap* bitmap = fx.PlantMarkBitmap(live, fx.region0->GetRegionSize());
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));

    GC_EXPECT_TRUE(RegionSpace::ShouldEnqueue<Generation::Old>(fx.obj0));
    GC_EXPECT_TRUE(RegionSpace::ShouldEnqueue<Generation::Old>(fx.obj0));

    bool firstIncLive = false;
    bool secondIncLive = false;
    const bool firstAlready = bitmap->MarkBits(offset, 8, fx.region0->GetRegionSize(), firstIncLive);
    const bool secondAlready = bitmap->MarkBits(offset, 8, fx.region0->GetRegionSize(), secondIncLive);
    const size_t liveBytes = bitmap->GetLiveBytes();
    const bool receiptOnce = !firstAlready && secondAlready;
    const bool incLiveOnce = firstIncLive && !secondIncLive;
    const bool bytesOnce = liveBytes == static_cast<size_t>(8);
    std::fprintf(stderr,
                 "DETAIL duplicate_consumer receipt_once=%d inc_live_once=%d bytes_once=%d live_bytes=%zu\n",
                 receiptOnce, incLiveOnce, bytesOnce, liveBytes);
    GC_EXPECT_TRUE(receiptOnce && incLiveOnce && bytesOnce);
    GC_EXPECT_TRUE(bitmap->IsMarked(offset));
    GC_EXPECT_EQ(liveBytes, static_cast<size_t>(8));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
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

    ProductPreserveRetainedFn()(region);
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
    ProductPreserveRetainedFn()(region);
    GC_EXPECT_FALSE(region->RetainedMarkWordsSay(holderOffset));

    region->FreeRetainedMarkWords();
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// A forwarding completion from the previous face may still be visible while
// the next mark cycle publishes a new current face.  The product sequence
// below (publish carrier → complete/reset forwarding → clear/mark next face)
// must retain that new face rather than treating the stale done bit as a FROM
// copy indicator.
GC_TEST(LiveMap, RetainedCaptureKeepsCurrentYoungFaceAfterStaleForwardingDone)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    LiveInfo* previous = fx.PlantLiveInfo(region);
    RegionBitmap* previousBitmap = fx.PlantMarkBitmap<Generation::Young>(previous, region->GetRegionSize());
    size_t holderOffset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)previousBitmap->MarkBits(holderOffset, 8, region->GetRegionSize());

    MarkView<Generation::Young> previousView = region->GetMarkView<Generation::Young>();
    region->PublishFromPageMetadata(previousView);
    region->MarkForwardingDone();
    region->ResetLiveMapAfterForward(previousView);

    // ClearLiveInfo + MarkObject are the product mark-start/current-face path;
    // no test-only epoch or LiveInfo fields are written here.
    MarkView<Generation::Young> clearView = region->GetMarkView<Generation::Young>();
    ProductClearLiveInfoFn()(region, clearView);
    MarkView<Generation::Young> currentView = region->GetMarkView<Generation::Young>();
    ProductMarkObject<Generation::Young> markObject = ProductMarkObjectFn<Generation::Young>();
    GC_EXPECT_TRUE(markObject != nullptr);
    if (markObject == nullptr) {
        return;
    }
    (void)markObject(region, currentView, fx.obj0, 8, true);
    ProductPreserveRetainedFn()(region);
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(holderOffset));

    region->metadata.liveInfo = nullptr;
    region->RetireFromPageMetadata();
    region->FreeRetainedMarkWords();
    fx.FreePlanted(previous);
}

// Clearing starts a new mark cycle but does not itself publish a current
// liveness face.  With no MarkObject in that cycle, a stale forwarding-done
// carrier must not make the previous from-page bits current again.
GC_TEST(LiveMap, RetainedCaptureRejectsOldFromFaceWhenNewCycleMarksNothing)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    LiveInfo* previous = fx.PlantLiveInfo(region);
    RegionBitmap* previousBitmap = fx.PlantMarkBitmap<Generation::Young>(previous, region->GetRegionSize());
    size_t holderOffset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));

    // Build the old face through the product marker, then carry it through the
    // real forwarding publication/reset path.  No epoch, flag, from-page or
    // retained field is written by the test.
    MarkView<Generation::Young> previousView = region->GetMarkView<Generation::Young>();
    ProductMarkObject<Generation::Young> markObject = ProductMarkObjectFn<Generation::Young>();
    GC_EXPECT_TRUE(markObject != nullptr);
    if (markObject == nullptr) {
        return;
    }
    GC_EXPECT_FALSE(markObject(region, previousView, fx.obj0, 8, true));
    GC_EXPECT_TRUE(previousBitmap->IsMarked(holderOffset));
    region->PublishFromPageMetadata(previousView);
    region->MarkForwardingDone();
    region->ResetLiveMapAfterForward(previousView);

    MarkView<Generation::Young> clearView = region->GetMarkView<Generation::Young>();
    ProductClearLiveInfoFn()(region, clearView);
    // Deliberately no MarkObject: the new cycle has zero first paint.
    ProductPreserveRetainedFn()(region);
    GC_EXPECT_FALSE(region->RetainedMarkWordsSay(holderOffset));

    region->metadata.liveInfo = nullptr;
    region->RetireFromPageMetadata();
    region->FreeRetainedMarkWords();
    fx.FreePlanted(previous);
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
    ProductPreserveRetainedUpToFn()(
        region, std::min(region->GetCensusBoundary(), region->GetRegionAllocPtr()));
    GC_EXPECT_TRUE(region->GetRetainedLiveInfo() == nullptr);
    GC_EXPECT_FALSE(region->HasEverPreservedRetainedLiveInfo());
    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRetainedLiveInfoState()),
                 static_cast<unsigned>(RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED));
}

// The bounded product entry first repairs from the current mark face before it
// publishes the retained carrier.  Calling it through the loaded runtime keeps
// this test from materializing a second inline copy in the test ELF.
GC_TEST(LiveMap, BoundedPreserveProductRepairsCurrentFace)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)bitmap->MarkBits(offset, fx.obj0->GetSize(), region->GetRegionSize());

    ProductPreserveRetainedUpToFn()(region, region->GetRegionAllocPtr());

    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRetainedLiveInfoState()),
                 static_cast<unsigned>(RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID));
    GC_EXPECT_TRUE(region->HasRetainedMarkWords());
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(offset));
    GC_EXPECT_EQ(region->GetRetainedLiveInfoCoveredUpTo(), region->GetRegionAllocPtr());
    region->FreeRetainedMarkWords();
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// If the current LiveInfo has already been unbound, forwarding's from-page
// carrier is the second repair source.  The retained owned copy must contain
// the same marked holder before the hard postcondition is checked.
GC_TEST(LiveMap, BoundedPreserveProductRepairsFromPageFace)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)bitmap->MarkBits(offset, fx.obj0->GetSize(), region->GetRegionSize());
    region->PublishFromPageMetadata(region->GetMarkView<Generation::Old>());
    region->CheckAndClearLiveInfo(live);
    GC_EXPECT_TRUE(region->GetLiveInfo() == nullptr);
    GC_EXPECT_TRUE(region->HasFromPageMetadata());

    ProductPreserveRetainedUpToFn()(region, region->GetRegionAllocPtr());

    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRetainedLiveInfoState()),
                 static_cast<unsigned>(RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID));
    GC_EXPECT_TRUE(region->HasRetainedMarkWords());
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(offset));
    region->RetireFromPageMetadata();
    region->FreeRetainedMarkWords();
    fx.FreePlanted(live);
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
        ProductPreserveRetainedUpToFn()(
            region, std::min(region->GetCensusBoundary(), region->GetRegionAllocPtr()));
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_TRUE(waitpid(pid, &status, 0) == pid);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    fx.FreePlanted(live);
}

// If neither borrowed face remains, a private copy that already covers the
// requested boundary is the third repair source. The product entry must retain
// that carrier instead of destroying it and converting a repairable page to LOST.
GC_TEST(LiveMap, BoundedPreserveProductRepairsOwnedCopy)
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
    const MAddress covered = region->GetRetainedLiveInfoCoveredUpTo();
    ProductPreserveRetainedUpToFn()(region, covered);
    GC_EXPECT_TRUE(region->HasRetainedMarkWords());
    GC_EXPECT_TRUE(region->RetainedMarkWordsSay(0));
    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRetainedLiveInfoState()),
                 static_cast<unsigned>(RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID));
    GC_EXPECT_EQ(region->GetRetainedLiveInfoCoveredUpTo(), covered);
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

    ProductPreserveRetainedFn()(region);
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

// The large-page first paint is a single publication/accounting RMW.  Two
// concurrent callers therefore have exactly one false (new-mark) result and
// the live byte book contains the object size once.
GC_TEST(LiveMap, LargeFirstPaintHasSingleWinner)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    region->SetRegionType(RegionInfo::RegionType::LARGE_REGION);
    BaseObject* holder = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(region->GetRegionStart() + holder->GetSize());
    MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();

    std::atomic<bool> go { false };
    std::atomic<int> first { 0 };
    std::thread t0([&]() {
        while (!go.load(std::memory_order_acquire)) {
        }
        if (!ProductMarkObjectFn<Generation::Old>()(region, view, holder, holder->GetSize(), true)) {
            first.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread t1([&]() {
        while (!go.load(std::memory_order_acquire)) {
        }
        if (!ProductMarkObjectFn<Generation::Old>()(region, view, holder, holder->GetSize(), true)) {
            first.fetch_add(1, std::memory_order_relaxed);
        }
    });
    go.store(true, std::memory_order_release);
    t0.join();
    t1.join();

    GC_EXPECT_EQ(first.load(std::memory_order_relaxed), 1);
    GC_EXPECT_EQ(region->GetLiveByteCount(), static_cast<uint64_t>(holder->GetSize()));
    GC_EXPECT_TRUE(region->IsCurrentFacePublished());
}

// The tagged generation skips raw zero at its only wrap point, keeping the
// non-zero publication invariant intact for the following first paint.
GC_TEST(LiveMap, SnapshotEpochWrapSkipsZero)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->metadata.snapshotEpoch = std::numeric_limits<uint64_t>::max() - 1;
    ProductBumpEpochFn()(region);
    GC_EXPECT_EQ(region->metadata.snapshotEpoch, 2ULL);
    GC_EXPECT_EQ(region->GetSnapshotEpoch(), 1ULL);
    region->PublishCurrentMarkFace();
    GC_EXPECT_TRUE((region->metadata.snapshotEpoch & 1ULL) != 0);
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
