// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Unit probes for Region Epoch (EPOCH_DESIGN_0729 §4):
// 1) route teardown during GetRoute geometry consumption is rejected
// 2) ClearGhostRegionBit bumps and invalidates its route carrier
// 3) retained LiveInfo snapshot becomes invalid after its region epoch changes

#include <atomic>
#include <cstdint>
#include <cstdio>

#define MRT_REGION_EPOCH_TEST 1
#include "Allocator/RegionManager.h"
#include "Allocator/RegionSpace.h"
#include "CangjieRuntime.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
std::atomic<bool> routeTeardownAfterGeometry { false };

void RouteEpochAfterGeometryReadForTest(RegionInfo* region)
{
    if (routeTeardownAfterGeometry.exchange(false, std::memory_order_acq_rel)) {
        region->DispelGhostFromRegion();
    }
}

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
    region->GetOrAllocLiveInfo();
    region->AddLiveByteCount(16);
    region->PrepareForwardableRegion();
    region->SetRouteInfo(region->GetRegionStart(), 16);
    region->SetRouteState(RegionInfo::RouteState::ROUTED);
    const uint64_t expected = region->GetIdentityEpoch();
    const bool matchBefore = region->RouteEpochMatches(expected);

    // Deterministically tear down after GetRoute has consumed geometry but before
    // RouteObject's final epoch/ghost validation publishes the computed address.
    manager.ResetRouteEpochMismatchCount();
    routeTeardownAfterGeometry.store(true, std::memory_order_release);
    BaseObject* geometryRace = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, expected);
    const size_t geometryMismatchCount = manager.GetRouteEpochMismatchCount();
    const uint64_t afterTeardown = region->GetRouteInstallEpoch();
    const uint64_t lateExpected = region->GetIdentityEpoch();
    const bool lateMatch = region->RouteEpochMatches(lateExpected);
    manager.ResetRouteEpochMismatchCount();
    BaseObject* late = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, lateExpected);
    const size_t lateMismatchCount = manager.GetRouteEpochMismatchCount();
    const bool pass = matchBefore && geometryRace == nullptr && geometryMismatchCount > 0 &&
        afterTeardown == RouteInfo::INVALID_EPOCH && !lateMatch && late == nullptr && lateMismatchCount > 0;
    std::printf(
        "EPOCH_PROBE route result=%s expected=%llu after_teardown=%llu match_before=%d "
        "sentinel=%d geometry_race_null=%d geometry_mismatch_count=%zu "
        "late_match=%d late_null=%d late_mismatch_count=%zu\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(expected),
        static_cast<unsigned long long>(afterTeardown), matchBefore ? 1 : 0,
        afterTeardown == RouteInfo::INVALID_EPOCH ? 1 : 0, geometryRace == nullptr ? 1 : 0,
        geometryMismatchCount, lateMatch ? 1 : 0, late == nullptr ? 1 : 0, lateMismatchCount);
    manager.ReclaimRegion(region);
    return pass;
}

bool ProbeClearGhostRouteEpoch(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE clear_ghost result=FAIL reason=take-region\n");
        return false;
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->GetOrAllocLiveInfo();
    region->AddLiveByteCount(16);
    region->PrepareForwardableRegion();
    region->SetRouteInfo(region->GetRegionStart(), 16);
    region->PreserveRetainedLiveInfo();
    const uint64_t snapshotBefore = region->GetSnapshotEpoch();
    const bool snapshotValidBefore = region->IsRetainedSnapshotValid() &&
        region->GetRetainedLiveInfoState() == RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID;
    const uint64_t before = region->GetIdentityEpoch();
    region->ClearGhostRegionBit();
    const uint64_t after = region->GetIdentityEpoch();
    const uint64_t snapshotAfter = region->GetSnapshotEpoch();
    const uint64_t routeEpoch = region->GetRouteInstallEpoch();
    const bool matchAfter = region->RouteEpochMatches(after);
    const bool pass = after != before && routeEpoch == RouteInfo::INVALID_EPOCH && !matchAfter &&
        !region->IsGhostFromRegion() && snapshotAfter != snapshotBefore && snapshotValidBefore &&
        region->IsRetainedSnapshotValid();
    std::printf(
        "EPOCH_PROBE clear_ghost result=%s before=%llu after=%llu bumped=%d sentinel=%d "
        "match_after=%d ghost=%d snapshot_before=%llu snapshot_after=%llu "
        "snapshot_valid_before=%d snapshot_valid_after=%d\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(before),
        static_cast<unsigned long long>(after), after != before ? 1 : 0,
        routeEpoch == RouteInfo::INVALID_EPOCH ? 1 : 0, matchAfter ? 1 : 0,
        region->IsGhostFromRegion() ? 1 : 0, static_cast<unsigned long long>(snapshotBefore),
        static_cast<unsigned long long>(snapshotAfter), snapshotValidBefore ? 1 : 0,
        region->IsRetainedSnapshotValid() ? 1 : 0);
    manager.ReclaimRegion(region);
    return pass;
}

bool ProbeRetainedSnapshotEpoch(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE snapshot result=FAIL reason=take-region\n");
        return false;
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->GetOrAllocLiveInfo();
    region->AddLiveByteCount(16);
    region->PrepareForwardableRegion();
    region->SetRouteInfo(region->GetRegionStart(), 16);
    region->PreserveRetainedLiveInfo();
    const uint64_t identityBefore = region->GetIdentityEpoch();
    const uint64_t snapshotBefore = region->GetSnapshotEpoch();
    const bool beforeClear = region->IsRetainedSnapshotValid() &&
        region->GetRetainedLiveInfoState() == RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID &&
        region->RouteEpochMatches(identityBefore);
    region->ClearLiveInfo();
    BaseObject* routed = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, identityBefore);
    const bool afterClear = !region->IsRetainedSnapshotValid();
    const bool pass = beforeClear && afterClear && region->GetIdentityEpoch() == identityBefore &&
        region->GetSnapshotEpoch() != snapshotBefore && region->RouteEpochMatches(identityBefore) &&
        routed != nullptr;
    std::printf(
        "EPOCH_PROBE snapshot result=%s identity_before=%llu identity_after=%llu "
        "snapshot_before=%llu snapshot_after=%llu route_match_after=%d route_nonnull=%d "
        "snapshot_invalid_after=%d\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(identityBefore),
        static_cast<unsigned long long>(region->GetIdentityEpoch()),
        static_cast<unsigned long long>(snapshotBefore),
        static_cast<unsigned long long>(region->GetSnapshotEpoch()),
        region->RouteEpochMatches(identityBefore) ? 1 : 0, routed != nullptr ? 1 : 0, afterClear ? 1 : 0);
    manager.ReclaimRegion(region);
    return pass;
}

bool ProbeRegionReuseRouteEpoch(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE reuse_route result=FAIL reason=take-region\n");
        return false;
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->GetOrAllocLiveInfo();
    region->AddLiveByteCount(16);
    region->PrepareForwardableRegion();
    region->SetRouteInfo(region->GetRegionStart(), 16);
    const uint64_t expected = region->GetIdentityEpoch();
    manager.ReclaimRegion(region);
    RegionInfo* reused = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    const bool sameRegion = reused == region;
    BaseObject* stale = sameRegion ? manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, expected) : nullptr;
    const bool identityChanged = sameRegion && region->GetIdentityEpoch() != expected;
    const bool pass = sameRegion && identityChanged && stale == nullptr;
    std::printf(
        "EPOCH_PROBE reuse_route result=%s same_region=%d expected=%llu actual=%llu "
        "identity_changed=%d stale_null=%d\n",
        pass ? "PASS" : "FAIL", sameRegion ? 1 : 0, static_cast<unsigned long long>(expected),
        static_cast<unsigned long long>(sameRegion ? region->GetIdentityEpoch() : 0),
        identityChanged ? 1 : 0, stale == nullptr ? 1 : 0);
    if (reused != nullptr) {
        manager.ReclaimRegion(reused);
    }
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
    if (!region->MarkObject(oldObject, AllocatorUtils::ALLOC_ALIGNMENT)) {
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
    region->BumpSnapshotEpoch();
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
    if (!region->MarkObject(object, AllocatorUtils::ALLOC_ALIGNMENT)) {
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
    const bool clearGhost = MapleRuntime::ProbeClearGhostRouteEpoch(manager);
    const bool snapshot = MapleRuntime::ProbeRetainedSnapshotEpoch(manager);
    const bool reuseRoute = MapleRuntime::ProbeRegionReuseRouteEpoch(manager);
    const bool boundary = MapleRuntime::ProbeRetainedCoveredBoundary(manager);
    const bool staleEmpty = MapleRuntime::ProbeStaleEmpty(manager);
    const bool largePromotion = MapleRuntime::ProbeLargePromotion(manager);
    MapleRuntime::CangjieRuntime::FiniAndDelete();
    std::printf(
        "EPOCH_PROBE summary route=%s clear_ghost=%s snapshot=%s reuse_route=%s boundary=%s "
        "stale_empty=%s large_promotion=%s\n",
        route ? "PASS" : "FAIL", clearGhost ? "PASS" : "FAIL", snapshot ? "PASS" : "FAIL",
        reuseRoute ? "PASS" : "FAIL", boundary ? "PASS" : "FAIL", staleEmpty ? "PASS" : "FAIL",
        largePromotion ? "PASS" : "FAIL");
    return route && clearGhost && snapshot && reuseRoute && boundary && staleEmpty && largePromotion ? 0 : 1;
}
