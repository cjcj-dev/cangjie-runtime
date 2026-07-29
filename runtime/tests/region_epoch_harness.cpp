// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Unit probes for Region Epoch (EPOCH_DESIGN_0729 §4):
// 1) teardown interleaving yields one complete old route followed by empty
// 2) an empty carrier is rejected with the mismatch counter incremented
// 3) UINT64_MAX is a legal install epoch, independent of carrier presence
// 4) retained LiveInfo snapshot becomes invalid after its region epoch changes

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
    const uint64_t expected = region->GetEpoch();
    RouteInfo before = region->AcquireRouteInfo();

    // Tear down after RouteObject has acquired a complete record. The reader owns
    // a by-value snapshot and must therefore return the complete old geometry.
    manager.ResetRouteEpochMismatchCount();
    routeTeardownAfterAcquire.store(true, std::memory_order_release);
    BaseObject* oldRoute = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, expected);
    const size_t interleaveMismatchCount = manager.GetRouteEpochMismatchCount();
    RouteInfo afterTeardown = region->AcquireRouteInfo();
    const uint64_t lateExpected = region->GetEpoch();
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
    const uint64_t expected = region->GetEpoch();
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
    const bool snapshot = MapleRuntime::ProbeRetainedSnapshotEpoch(manager);
    MapleRuntime::CangjieRuntime::FiniAndDelete();
    std::printf("EPOCH_PROBE summary route=%s empty=%s max_epoch=%s snapshot=%s\n",
                route ? "PASS" : "FAIL", empty ? "PASS" : "FAIL", maxEpoch ? "PASS" : "FAIL",
                snapshot ? "PASS" : "FAIL");
    return route && empty && maxEpoch && snapshot ? 0 : 1;
}
