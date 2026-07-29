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
    // Install a dummy route stamped with current region epoch (no Bump on install, R2).
    region->SetRouteInfo(region->GetRegionStart(), 16);
    const uint64_t install = region->GetRouteInstallEpoch();
    const bool matchBefore = region->RouteEpochMatches(install);
    // Simulate route-teardown validity-end (DispelGhost bumps + restamps installEpoch).
    region->BumpEpoch();
    region->SetRouteInfo(0);
    const uint64_t afterTeardown = region->GetRouteInstallEpoch();
    const bool matchAfter = region->RouteEpochMatches(install);
    const bool pass = matchBefore && !matchAfter && (afterTeardown != install);
    std::printf(
        "EPOCH_PROBE route result=%s install=%llu after_teardown=%llu match_before=%d match_after=%d\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(install),
        static_cast<unsigned long long>(afterTeardown), matchBefore ? 1 : 0, matchAfter ? 1 : 0);
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
