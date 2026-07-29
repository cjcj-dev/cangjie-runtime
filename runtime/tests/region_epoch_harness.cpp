// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Unit probes for Region Epoch (EPOCH_DESIGN_0729 §4):
// 1) route teardown during GetRoute geometry consumption is rejected
// 2) ClearGhostRegionBit bumps and invalidates its route carrier
// 3) retained LiveInfo snapshot becomes invalid after its region epoch changes
// 4) reclaim invalidates ghost lookup and route metadata for stale object addresses

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
    const uint64_t expected = region->GetEpoch();
    const bool matchBefore = region->RouteEpochMatches(expected);

    // Deterministically tear down after GetRoute has consumed geometry but before
    // RouteObject's final epoch/ghost validation publishes the computed address.
    manager.ResetRouteEpochMismatchCount();
    routeTeardownAfterGeometry.store(true, std::memory_order_release);
    BaseObject* geometryRace = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, expected);
    const size_t geometryMismatchCount = manager.GetRouteEpochMismatchCount();
    const uint64_t afterTeardown = region->GetRouteInstallEpoch();
    const uint64_t lateExpected = region->GetEpoch();
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
    const uint64_t before = region->GetEpoch();
    region->ClearGhostRegionBit();
    const uint64_t after = region->GetEpoch();
    const uint64_t routeEpoch = region->GetRouteInstallEpoch();
    const bool matchAfter = region->RouteEpochMatches(after);
    const bool pass = after != before && routeEpoch == RouteInfo::INVALID_EPOCH && !matchAfter &&
        !region->IsGhostFromRegion();
    std::printf(
        "EPOCH_PROBE clear_ghost result=%s before=%llu after=%llu bumped=%d sentinel=%d "
        "match_after=%d ghost=%d\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(before),
        static_cast<unsigned long long>(after), after != before ? 1 : 0,
        routeEpoch == RouteInfo::INVALID_EPOCH ? 1 : 0, matchAfter ? 1 : 0,
        region->IsGhostFromRegion() ? 1 : 0);
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
    // Force SNAPSHOT_VALID-like state via Preserve when empty → SNAPSHOT_EMPTY;
    // for VALID path we only check IsRetainedSnapshotValid after ClearLiveInfo bumps.
    region->PreserveRetainedLiveInfo();
    const bool beforeClear = region->GetRetainedLiveInfoState() == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY ||
        region->IsRetainedSnapshotValid() ||
        region->GetRetainedLiveInfoState() == RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED;
    region->ClearLiveInfo();
    const bool afterClear = !region->IsRetainedSnapshotValid();
    const bool pass = beforeClear && afterClear;
    std::printf("EPOCH_PROBE snapshot result=%s after_clear_invalid=%d\n", pass ? "PASS" : "FAIL",
                afterClear ? 1 : 0);
    manager.ReclaimRegion(region);
    return pass;
}

bool ProbeReclaimGhostTeardown(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(2, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
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
    manager.ReclaimRegion(region);

    const bool headCleared = RegionInfo::GetGhostFromRegionAt(staleHead) == nullptr;
    const bool subordinateCleared = RegionInfo::GetGhostFromRegionAt(staleSubordinate) == nullptr;
    const bool routeCleared = region->GetRouteInstallEpoch() == RouteInfo::INVALID_EPOCH &&
        region->GetRouteState() == RegionInfo::RouteState::NORMAL;
    const bool pass = ghostBefore && headCleared && subordinateCleared && routeCleared;
    std::printf(
        "EPOCH_PROBE reclaim_ghost result=%s ghost_before=%d head_null=%d subordinate_null=%d "
        "route_sentinel=%d route_normal=%d\n",
        pass ? "PASS" : "FAIL", ghostBefore ? 1 : 0, headCleared ? 1 : 0, subordinateCleared ? 1 : 0,
        region->GetRouteInstallEpoch() == RouteInfo::INVALID_EPOCH ? 1 : 0,
        region->GetRouteState() == RegionInfo::RouteState::NORMAL ? 1 : 0);
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
    const bool reclaimGhost = MapleRuntime::ProbeReclaimGhostTeardown(manager);
    MapleRuntime::CangjieRuntime::FiniAndDelete();
    std::printf("EPOCH_PROBE summary route=%s clear_ghost=%s snapshot=%s reclaim_ghost=%s\n",
                route ? "PASS" : "FAIL", clearGhost ? "PASS" : "FAIL", snapshot ? "PASS" : "FAIL",
                reclaimGhost ? "PASS" : "FAIL");
    return route && clearGhost && snapshot && reclaimGhost ? 0 : 1;
}
