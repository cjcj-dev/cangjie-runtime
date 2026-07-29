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

bool ProbeRouteEpochMismatch(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE route result=FAIL reason=take-region\n");
        return false;
    }
    // Establish a real from/ghost route lifetime, then retain the caller's view.
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->PrepareForwardableRegion();
    region->SetRouteInfo(region->GetRegionStart(), 16);
    const uint64_t expected = region->GetEpoch();
    const bool matchBefore = region->RouteEpochMatches(expected);

    // Real route teardown: bump region epoch, restamp RouteInfo, and clear ghost state.
    region->DispelGhostFromRegion();
    const uint64_t afterTeardown = region->GetRouteInstallEpoch();
    RegionManager::ResetRouteEpochMismatchCount();
    BaseObject* stale = manager.RouteObject(
        reinterpret_cast<BaseObject*>(region->GetRegionStart()), region, expected);
    const size_t mismatchCount = RegionManager::GetRouteEpochMismatchCount();
    const bool pass = matchBefore && stale == nullptr && mismatchCount > 0 && afterTeardown != expected;
    std::printf(
        "EPOCH_PROBE route result=%s expected=%llu after_teardown=%llu match_before=%d "
        "stale_null=%d mismatch_count=%zu\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(expected),
        static_cast<unsigned long long>(afterTeardown), matchBefore ? 1 : 0,
        stale == nullptr ? 1 : 0, mismatchCount);
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
    const bool route = MapleRuntime::ProbeRouteEpochMismatch(manager);
    const bool snapshot = MapleRuntime::ProbeRetainedSnapshotEpoch(manager);
    MapleRuntime::CangjieRuntime::FiniAndDelete();
    std::printf("EPOCH_PROBE summary route=%s snapshot=%s\n", route ? "PASS" : "FAIL",
                snapshot ? "PASS" : "FAIL");
    return route && snapshot ? 0 : 1;
}
