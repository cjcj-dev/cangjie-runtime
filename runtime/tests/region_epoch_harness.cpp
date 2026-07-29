// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Unit probes for Region Epoch (EPOCH_DESIGN_0729 §4), merged k6 (domain split)
// × k7 (seqlock route carrier) × k8 (reclaim teardown) union:
// 1) teardown interleaving yields one complete old route followed by empty
// 2) an empty carrier is rejected with the mismatch counter incremented
// 3) UINT64_MAX is a legal install epoch, independent of carrier presence
// 4) ClearGhostRegionBit bumps identity (and snapshot), publishes carrier absence
// 5) ClearLiveInfo ends only the snapshot lifetime; identity and route survive
// 6) region reuse turns identity over; a stale caller view is rejected
// 7) retained snapshot binds its coverage boundary (allocation frontier)
// 8) a stale EMPTY snapshot is detected via snapshot epoch
// 9) large-region promotion preserves a valid flag-shaped retained snapshot
// 10) garbage consumers retain a ghost carrier until dispel; the original reclaim
//     path then invalidates head/subordinate lookup and route metadata
// Note: k6's post-geometry-read probe was retired with its product window —
// an acquired by-value snapshot cannot change under the reader (k7 protocol).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>

#define MRT_REGION_EPOCH_TEST 1
#include "Allocator/RegionManager.h"
#include "Allocator/RegionSpace.h"
#include "CangjieRuntime.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
std::atomic<bool> routeTeardownAfterAcquire { false };

void RouteRecordAfterAcquireForTest(RegionInfo* region)
{
    if (routeTeardownAfterAcquire.exchange(false, std::memory_order_acq_rel)) {
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

bool ProbeRouteRecordInterleave(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE route_interleave result=FAIL reason=take-region\n");
        return false;
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->GetOrAllocLiveInfo();
    region->AddLiveByteCount(16);
    region->PrepareForwardableRegion();
    region->SetRouteInfo(region->GetRegionStart(), 16);
    region->SetRouteState(RegionInfo::RouteState::ROUTED);
    const uint64_t expected = region->GetIdentityEpoch();
    RouteInfo before = region->AcquireRouteInfo();

    // Tear down after RouteObject has acquired a complete record. The reader owns
    // a by-value snapshot and must therefore return the complete old geometry.
    manager.ResetRouteEpochMismatchCount();
    routeTeardownAfterAcquire.store(true, std::memory_order_release);
    BaseObject* oldRoute = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, expected);
    const size_t interleaveMismatchCount = manager.GetRouteEpochMismatchCount();
    RouteInfo afterTeardown = region->AcquireRouteInfo();
    const uint64_t lateExpected = region->GetIdentityEpoch();
    manager.ResetRouteEpochMismatchCount();
    BaseObject* late = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, lateExpected);
    const size_t lateMismatchCount = manager.GetRouteEpochMismatchCount();
    const bool pass = before.IsInstalled() && oldRoute == reinterpret_cast<BaseObject*>(region->GetRegionStart()) &&
        interleaveMismatchCount == 0 && !afterTeardown.IsInstalled() && late == nullptr && lateMismatchCount > 0;
    std::printf(
        "EPOCH_PROBE route_interleave result=%s expected=%llu before_present=%d full_old=%d "
        "after_empty=%d interleave_mismatch_count=%zu late_null=%d late_mismatch_count=%zu\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(expected),
        before.IsInstalled() ? 1 : 0,
        oldRoute == reinterpret_cast<BaseObject*>(region->GetRegionStart()) ? 1 : 0,
        afterTeardown.IsInstalled() ? 0 : 1, interleaveMismatchCount,
        late == nullptr ? 1 : 0, lateMismatchCount);
    manager.ReclaimRegion(region);
    return pass;
}

bool ProbeEmptyRouteRecord(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE route_empty result=FAIL reason=take-region\n");
        return false;
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->GetOrAllocLiveInfo();
    region->AddLiveByteCount(16);
    region->PrepareForwardableRegion();
    region->SetRouteState(RegionInfo::RouteState::ROUTED);
    const uint64_t expected = region->GetIdentityEpoch();
    RouteInfo empty = region->AcquireRouteInfo();
    manager.ResetRouteEpochMismatchCount();
    BaseObject* route = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, expected);
    const size_t mismatchCount = manager.GetRouteEpochMismatchCount();
    const bool pass = !empty.IsInstalled() && route == nullptr && mismatchCount > 0;
    std::printf(
        "EPOCH_PROBE route_empty result=%s present=%d route_null=%d mismatch_count=%zu\n",
        pass ? "PASS" : "FAIL", empty.IsInstalled() ? 1 : 0, route == nullptr ? 1 : 0, mismatchCount);
    region->DispelGhostFromRegion();
    manager.ReclaimRegion(region);
    return pass;
}

bool ProbeMaxEpochRouteRecord(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE route_max_epoch result=FAIL reason=take-region\n");
        return false;
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->GetOrAllocLiveInfo();
    region->AddLiveByteCount(16);
    region->PrepareForwardableRegion();
    const uint64_t maxEpoch = std::numeric_limits<uint64_t>::max();
    region->SetEpochForTest(maxEpoch);
    region->SetRouteInfo(region->GetRegionStart(), 16);
    region->SetRouteState(RegionInfo::RouteState::ROUTED);
    RouteInfo routeInfo = region->AcquireRouteInfo();
    manager.ResetRouteEpochMismatchCount();
    BaseObject* route = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, maxEpoch);
    const size_t mismatchCount = manager.GetRouteEpochMismatchCount();
    const bool pass = routeInfo.IsInstalled() && routeInfo.GetInstallEpoch() == maxEpoch &&
        route == reinterpret_cast<BaseObject*>(region->GetRegionStart()) && mismatchCount == 0;
    std::printf(
        "EPOCH_PROBE route_max_epoch result=%s epoch=%llu present=%d epoch_match=%d "
        "route_full=%d mismatch_count=%zu\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(maxEpoch),
        routeInfo.IsInstalled() ? 1 : 0, routeInfo.GetInstallEpoch() == maxEpoch ? 1 : 0,
        route == reinterpret_cast<BaseObject*>(region->GetRegionStart()) ? 1 : 0, mismatchCount);
    region->DispelGhostFromRegion();
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
    RouteInfo cleared = region->AcquireRouteInfo();
    const bool matchAfter = region->RouteEpochMatches(after);
    const bool pass = after != before && !cleared.IsInstalled() && !matchAfter &&
        !region->IsGhostFromRegion() && snapshotAfter != snapshotBefore && snapshotValidBefore &&
        region->IsRetainedSnapshotValid();
    std::printf(
        "EPOCH_PROBE clear_ghost result=%s before=%llu after=%llu bumped=%d absent=%d "
        "match_after=%d ghost=%d snapshot_before=%llu snapshot_after=%llu "
        "snapshot_valid_before=%d snapshot_valid_after=%d\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(before),
        static_cast<unsigned long long>(after), after != before ? 1 : 0,
        cleared.IsInstalled() ? 0 : 1, matchAfter ? 1 : 0,
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
    const bool afterClear = !region->IsRetainedSnapshotValid();
    const bool pass = beforeClear && afterClear && region->GetIdentityEpoch() == identityBefore &&
        region->GetSnapshotEpoch() != snapshotBefore && region->RouteEpochMatches(identityBefore) &&
        region->IsGhostFromRegion();
    std::printf(
        "EPOCH_PROBE snapshot result=%s identity_before=%llu identity_after=%llu "
        "snapshot_before=%llu snapshot_after=%llu route_match_after=%d ghost_after=%d "
        "snapshot_invalid_after=%d\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(identityBefore),
        static_cast<unsigned long long>(region->GetIdentityEpoch()),
        static_cast<unsigned long long>(snapshotBefore),
        static_cast<unsigned long long>(region->GetSnapshotEpoch()),
        region->RouteEpochMatches(identityBefore) ? 1 : 0, region->IsGhostFromRegion() ? 1 : 0,
        afterClear ? 1 : 0);
    // Reclaim preserves the historical ghost overlay by design. End the route lifetime
    // through its production teardown path before returning this region to the free tree.
    region->ClearGhostRegionBit();
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

bool ProbeReclaimGhostTeardown(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(2, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE reclaim_ghost result=FAIL reason=take-region\n");
        return false;
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->GetOrAllocLiveInfo();
    region->AddLiveByteCount(16);
    region->PrepareForwardableRegion();
    region->SetRouteInfo(region->GetRegionStart(), 16);
    region->SetRouteState(RegionInfo::RouteState::ROUTED);

    const MAddress staleHead = region->GetRegionStart();
    const MAddress staleSubordinate = staleHead + RegionInfo::UNIT_SIZE;
    const bool ghostBefore = RegionInfo::GetGhostFromRegionAt(staleHead) == region &&
        RegionInfo::GetGhostFromRegionAt(staleSubordinate) == region;
    (void)manager.CollectRegion(region);
    manager.ReclaimGarbageRegions();
    const bool retainedInGarbage = region->IsGarbageRegion() &&
        RegionInfo::GetGhostFromRegionAt(staleHead) == region &&
        RegionInfo::GetGhostFromRegionAt(staleSubordinate) == region &&
        region->AcquireRouteInfo().IsInstalled();
    RegionInfo* other = manager.TakeRegion(2, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    const bool takeSkippedGhost = other != region;
    if (other != nullptr && other != region) {
        manager.ReclaimRegion(other);
    }

    region->DispelGhostFromRegion();
    manager.ReclaimGarbageRegions();
    const bool headCleared = RegionInfo::GetGhostFromRegionAt(staleHead) == nullptr;
    const bool subordinateCleared = RegionInfo::GetGhostFromRegionAt(staleSubordinate) == nullptr;
    const bool routeCleared = !region->AcquireRouteInfo().IsInstalled() &&
        region->GetRouteState() == RegionInfo::RouteState::NORMAL;
    const bool reclaimedAfterDispel = region->IsFreeRegion();
    const bool pass = ghostBefore && retainedInGarbage && takeSkippedGhost && headCleared &&
        subordinateCleared && routeCleared && reclaimedAfterDispel;
    std::printf(
        "EPOCH_PROBE reclaim_ghost result=%s ghost_before=%d retained_in_garbage=%d "
        "take_skipped_ghost=%d head_null=%d subordinate_null=%d route_absent=%d "
        "route_normal=%d reclaimed_after_dispel=%d\n",
        pass ? "PASS" : "FAIL", ghostBefore ? 1 : 0, retainedInGarbage ? 1 : 0,
        takeSkippedGhost ? 1 : 0, headCleared ? 1 : 0, subordinateCleared ? 1 : 0,
        region->AcquireRouteInfo().IsInstalled() ? 0 : 1,
        region->GetRouteState() == RegionInfo::RouteState::NORMAL ? 1 : 0, reclaimedAfterDispel ? 1 : 0);
    if (other == region) {
        manager.ReclaimRegion(region);
    }
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
    const bool route = MapleRuntime::ProbeRouteRecordInterleave(manager);
    const bool empty = MapleRuntime::ProbeEmptyRouteRecord(manager);
    const bool maxEpoch = MapleRuntime::ProbeMaxEpochRouteRecord(manager);
    const bool clearGhost = MapleRuntime::ProbeClearGhostRouteEpoch(manager);
    const bool snapshot = MapleRuntime::ProbeRetainedSnapshotEpoch(manager);
    const bool reuseRoute = MapleRuntime::ProbeRegionReuseRouteEpoch(manager);
    const bool boundary = MapleRuntime::ProbeRetainedCoveredBoundary(manager);
    const bool staleEmpty = MapleRuntime::ProbeStaleEmpty(manager);
    const bool largePromotion = MapleRuntime::ProbeLargePromotion(manager);
    const bool reclaimGhost = MapleRuntime::ProbeReclaimGhostTeardown(manager);
    MapleRuntime::CangjieRuntime::FiniAndDelete();
    std::printf(
        "EPOCH_PROBE summary route=%s empty=%s max_epoch=%s clear_ghost=%s snapshot=%s "
        "reuse_route=%s boundary=%s stale_empty=%s large_promotion=%s reclaim_ghost=%s\n",
        route ? "PASS" : "FAIL", empty ? "PASS" : "FAIL", maxEpoch ? "PASS" : "FAIL",
        clearGhost ? "PASS" : "FAIL", snapshot ? "PASS" : "FAIL", reuseRoute ? "PASS" : "FAIL",
        boundary ? "PASS" : "FAIL", staleEmpty ? "PASS" : "FAIL", largePromotion ? "PASS" : "FAIL",
        reclaimGhost ? "PASS" : "FAIL");
    return route && empty && maxEpoch && clearGhost && snapshot && reuseRoute && boundary &&
        staleEmpty && largePromotion && reclaimGhost ? 0 : 1;
}
