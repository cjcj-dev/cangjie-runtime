// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Unit probes for Region Epoch (EPOCH_DESIGN_0729 §4):
// 1) route teardown: GetRoute after BumpEpoch/dispel-style install mismatch fails
// 2) retained LiveInfo snapshot becomes invalid after its region epoch changes

#include <cstdint>
#include <cstdio>

#include "Allocator/RegionManager.h"
#include "Allocator/RegionSpace.h"
#include "CangjieRuntime.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace {

BaseObject* AllocateTestObject(RegionInfo* region)
{
    MAddress address = region->Alloc(AllocatorUtils::ALLOC_ALIGNMENT);
    if (address == 0) {
        return nullptr;
    }
    return reinterpret_cast<BaseObject*>(address);
}

bool ProbeRouteEpochMismatch(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE route result=FAIL reason=take-region\n");
        return false;
    }
    // Establish a real from/ghost route lifetime, then retain the caller's view.
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->AddLiveByteCount(TYPEINFO_PTR_SIZE);
    region->PrepareForwardableRegion();
    region->SetRouteInfo(region->GetRegionStart(), 16);
    const uint64_t expected = region->GetEpoch();
    const bool matchBefore = region->RouteEpochMatches(expected);

    // Real route teardown: bump region epoch, restamp RouteInfo, and clear ghost state.
    region->DispelGhostFromRegion();
    const uint64_t afterTeardown = region->GetRouteInstallEpoch();
    manager.ResetRouteEpochMismatchCount();
    BaseObject* stale = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, expected);
    const size_t mismatchCount = manager.GetRouteEpochMismatchCount();
    // Close the lookup->epoch-capture interleaving too: after teardown all three
    // epochs equal again, but the actual ghost/route state makes consumption invalid.
    const uint64_t lateExpected = region->GetEpoch();
    manager.ResetRouteEpochMismatchCount();
    BaseObject* late = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, lateExpected);
    const size_t lateMismatchCount = manager.GetRouteEpochMismatchCount();
    const bool pass = matchBefore && stale == nullptr && mismatchCount > 0 && afterTeardown != expected &&
        late == nullptr && lateMismatchCount > 0;
    std::printf(
        "EPOCH_PROBE route result=%s expected=%llu after_teardown=%llu match_before=%d "
        "stale_null=%d mismatch_count=%zu late_null=%d late_mismatch_count=%zu\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(expected),
        static_cast<unsigned long long>(afterTeardown), matchBefore ? 1 : 0,
        stale == nullptr ? 1 : 0, mismatchCount, late == nullptr ? 1 : 0, lateMismatchCount);
    manager.ReclaimRegion(region);
    return pass;
}

bool ProbeRetainedCoveredBoundary(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE boundary result=FAIL reason=take-region\n");
        return false;
    }
    BaseObject* oldObject = AllocateTestObject(region);
    if (oldObject == nullptr) {
        manager.ReclaimRegion(region);
        std::printf("EPOCH_PROBE boundary result=FAIL reason=alloc-old\n");
        return false;
    }
    if (!region->MarkObject(oldObject)) {
        region->AddLiveByteCount(AllocatorUtils::ALLOC_ALIGNMENT);
    }
    region->PreserveRetainedLiveInfo();
    MAddress coveredUpTo = region->GetRetainedLiveInfoCoveredUpTo();
    BaseObject* newObject = AllocateTestObject(region);
    LiveInfo* retained = region->GetRetainedLiveInfo();
    bool oldBitmapLive = retained != nullptr && retained->IsSurvivedObject(0);
    bool newBitmapDead = newObject != nullptr && retained != nullptr &&
        !retained->IsSurvivedObject(reinterpret_cast<MAddress>(newObject) - region->GetRegionStart());
    bool newImplicitLive = newObject != nullptr && reinterpret_cast<MAddress>(newObject) >= coveredUpTo;
    bool pass = region->IsRetainedSnapshotValid() && coveredUpTo == reinterpret_cast<MAddress>(newObject) &&
        oldBitmapLive && newBitmapDead && newImplicitLive;
    std::printf(
        "EPOCH_PROBE boundary result=%s covered_up_to=%#zx new_object=%#zx old_bitmap_live=%d "
        "new_bitmap_dead=%d new_implicit_live=%d\n",
        pass ? "PASS" : "FAIL", coveredUpTo, reinterpret_cast<MAddress>(newObject),
        oldBitmapLive ? 1 : 0, newBitmapDead ? 1 : 0, newImplicitLive ? 1 : 0);
    manager.ReclaimRegion(region);
    return pass;
}

bool ProbeStaleEmpty(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE stale_empty result=FAIL reason=take-region\n");
        return false;
    }
    region->PreserveRetainedLiveInfo();
    bool wasEmpty = region->GetRetainedLiveInfoState() == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY;
    bool validBefore = region->IsRetainedSnapshotValid();
    region->BumpEpoch();
    bool staleDetected = !region->IsRetainedSnapshotValid();
    bool pass = wasEmpty && validBefore && staleDetected;
    std::printf(
        "EPOCH_PROBE stale_empty result=%s was_empty=%d valid_before=%d stale_detected=%d\n",
        pass ? "PASS" : "FAIL", wasEmpty ? 1 : 0, validBefore ? 1 : 0, staleDetected ? 1 : 0);
    manager.ReclaimRegion(region);
    return pass;
}

bool ProbeLargePromotion(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE large_promotion result=FAIL reason=take-region\n");
        return false;
    }
    BaseObject* object = AllocateTestObject(region);
    if (object == nullptr) {
        manager.ReclaimRegion(region);
        std::printf("EPOCH_PROBE large_promotion result=FAIL reason=alloc\n");
        return false;
    }
    if (!region->MarkObject(object)) {
        region->AddLiveByteCount(AllocatorUtils::ALLOC_ALIGNMENT);
    }
    manager.PromoteAllRegions();
    bool snapshotValid = region->GetRetainedLiveInfoState() ==
            RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID &&
        region->IsRetainedSnapshotValid();
    bool largeFlagLive = region->IsSurvivedObject(0);
    bool covered = region->GetRetainedLiveInfoCoveredUpTo() == region->GetRegionAllocPtr();
    bool bitmapShape = region->GetRetainedLiveInfo() == nullptr;
    bool pass = snapshotValid && largeFlagLive && covered && bitmapShape;
    std::printf(
        "EPOCH_PROBE large_promotion result=%s snapshot_valid=%d large_flag_live=%d "
        "covered=%d bitmap_shape=%d\n",
        pass ? "PASS" : "FAIL", snapshotValid ? 1 : 0, largeFlagLive ? 1 : 0,
        covered ? 1 : 0, bitmapShape ? 1 : 0);
    manager.ReclaimRegion(region);
    return pass;
}

} // namespace
} // namespace MapleRuntime

int main()
{
    MapleRuntime::MRT_CjRuntimeInit();
    auto& allocator =
        reinterpret_cast<MapleRuntime::RegionSpace&>(MapleRuntime::Heap::GetHeap().GetAllocator());
    MapleRuntime::RegionManager& manager = allocator.GetRegionManager();
    const bool route = MapleRuntime::ProbeRouteEpochMismatch(manager);
    const bool boundary = MapleRuntime::ProbeRetainedCoveredBoundary(manager);
    const bool staleEmpty = MapleRuntime::ProbeStaleEmpty(manager);
    const bool largePromotion = MapleRuntime::ProbeLargePromotion(manager);
    MapleRuntime::CangjieRuntime::FiniAndDelete();
    std::printf("EPOCH_PROBE summary route=%s boundary=%s stale_empty=%s large_promotion=%s\n",
                route ? "PASS" : "FAIL", boundary ? "PASS" : "FAIL",
                staleEmpty ? "PASS" : "FAIL", largePromotion ? "PASS" : "FAIL");
    return route && boundary && staleEmpty && largePromotion ? 0 : 1;
}
