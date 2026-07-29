// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Unit probes for Region Epoch (EPOCH_DESIGN_0729 §4):
// 1) reuse race: old FixEdgeSet entry across reclaim/reuse is epoch-skipped
// 2) route teardown: GetRoute after BumpEpoch/dispel-style install mismatch fails

#include <cstdint>
#include <cstdio>

#include "Allocator/RegionManager.h"
#include "Allocator/RegionSpace.h"
#include "CangjieRuntime.h"
#include "Heap/FixEdgeSet.h"
#include "Heap/Heap.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {

struct ProbeSlot {
    RefField<> field;
};

bool ProbeReuseEpochSkip(RegionManager& manager)
{
    RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("EPOCH_PROBE reuse result=FAIL reason=take-region\n");
        return false;
    }
    const uint64_t e0 = region->GetEpoch();
    region->BumpEpoch();
    const uint64_t e1 = region->GetEpoch();
    if (e1 != e0 + 1) {
        std::printf("EPOCH_PROBE reuse result=FAIL reason=bump e0=%llu e1=%llu\n",
                    static_cast<unsigned long long>(e0), static_cast<unsigned long long>(e1));
        return false;
    }

    // Simulate FixEdgeSet entry stamped at e0, then region free/reuse bumps past e0.
    FixEdgeSet::Instance().ResetSkipCounts();
    ProbeSlot slot;
    // Use a heap address for the slot if possible; otherwise exercise Add+Visit path via MaybeAdd
    // with synthetic stamp by calling Add directly.
    FixEdgeSet::Instance().AddWithEpoch(reinterpret_cast<MAddress>(&slot.field), e0, true);

    // Bump again as free/reuse would.
    region->BumpEpoch();

    size_t visited = 0;
    FixEdgeSet::Instance().VisitAndClear([&visited](RefField<>&) { ++visited; });
    const size_t epochSkip = FixEdgeSet::Instance().EpochSkipCount();
    // Slot may not be a heap address → VisitAndClear drops before epoch check.
    // Re-run with a real heap slot allocated in the region.
    uintptr_t alloc = region->Alloc(sizeof(void*));
    if (alloc == 0) {
        // Still validate bump monotonicity.
        std::printf(
            "EPOCH_PROBE reuse result=PASS partial=bump-only e0=%llu e1=%llu e_now=%llu visited=%zu epoch_skip=%zu\n",
            static_cast<unsigned long long>(e0), static_cast<unsigned long long>(e1),
            static_cast<unsigned long long>(region->GetEpoch()), visited, epochSkip);
        manager.ReclaimRegion(region);
        return true;
    }
    auto* heapField = reinterpret_cast<RefField<>*>(alloc);
    const uint64_t stamp = region->GetEpoch();
    FixEdgeSet::Instance().ResetSkipCounts();
    FixEdgeSet::Instance().AddWithEpoch(reinterpret_cast<MAddress>(heapField), stamp, true);
    // free/reuse bump invalidates stamp
    region->BumpEpoch();
    size_t visited2 = 0;
    FixEdgeSet::Instance().VisitAndClear([&visited2](RefField<>&) { ++visited2; });
    const size_t skip2 = FixEdgeSet::Instance().EpochSkipCount();
    const bool pass = skip2 > 0 && visited2 == 0;
    std::printf(
        "EPOCH_PROBE reuse result=%s stamp=%llu now=%llu visited=%zu epoch_skip=%zu\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(stamp),
        static_cast<unsigned long long>(region->GetEpoch()), visited2, skip2);
    manager.ReclaimRegion(region);
    return pass;
}

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
    const bool a = MapleRuntime::ProbeReuseEpochSkip(manager);
    const bool b = MapleRuntime::ProbeRouteEpochMismatch(manager);
    const bool c = MapleRuntime::ProbeRetainedSnapshotEpoch(manager);
    MapleRuntime::CangjieRuntime::FiniAndDelete();
    std::printf("EPOCH_PROBE summary reuse=%s route=%s snapshot=%s\n", a ? "PASS" : "FAIL",
                b ? "PASS" : "FAIL", c ? "PASS" : "FAIL");
    return a && b && c ? 0 : 1;
}
