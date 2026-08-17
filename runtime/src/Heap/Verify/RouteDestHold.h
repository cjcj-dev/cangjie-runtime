// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ROUTE_DEST_HOLD_H
#define MRT_ROUTE_DEST_HOLD_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// routedest: accounting for the destination-side hold.
//
// The defect this measures: RouteInfo records a bare start address plus a used-bytes split
// (LiveInfo.h:246-248) and carries no epoch, so a to-region that was reclaimed and re-taken
// keeps answering the same geometry (RegionInfo.h, GetRouteInfoForProbe comment). Nothing on
// the reclaim path asks whether a route still names the region it is about to hand back:
// the only liveness predicate there, IsGhostFromRegion(), is asked of the region being reused,
// i.e. of a route HOLDER, never of a route DESTINATION.
//
// RegionInfo::routeDestHold is stamped on the destination inside the ROUTING critical section
// and cleared once per route generation, after PrepareFromRegionList's dispel walk has retired
// every route that could name it. The reclaim entry points refuse a held region.
//
// Gates (all default off; product path early-returns before any counter):
//   MRT_GCV2_ROUTE_DEST_ACCOUNT=1
//       Enable counting and the atexit summary. Does not change control flow.
//   MRT_GCV2_ROUTE_DEST_INJECT_HANDBACK=1
//       Negative arm on the SAME binary: HoldsBack() reports "not held" to every reclaim
//       entry point while the stamp, the clear and the counters stay live. This is what
//       makes "after = 0" distinguishable from "counter never wired": with inject on,
//       reused_while_routed must move; with inject off it must be 0 on the same workload.
//
// Headline number: reused_while_routed — a region whose payload was zeroed and whose
// RegionInfo was re-initialised by TakeRegion while a published route still named it.
// That is the defect itself, counted once per event, uncapped (a 32-sample cap is what hid
// the population from PermWhoAdmit before).

namespace RouteDestHold {

// Reclaim entry points that can take a region out of the live set.
enum class Site : uint32_t {
    ASSEMBLE_RECENT_FULL = 0, // RegionManager::AssembleSmallGarbageCandidates, recentFullRegionList
    ASSEMBLE_UNMOVABLE = 1,   // RegionManager::AssembleSmallGarbageCandidates, unmovableFromRegionList
    YOUNG_UNMOVABLE = 2,      // RegionManager::PrepareYoungGarbageCandidates, unmovable young loop
    YOUNG_RECENT_FULL = 3,    // RegionManager::PrepareYoungGarbageCandidates, recentFull young loop
    TAKE_GARBAGE = 4,         // RegionManager::TakeReclaimableGarbageRegion candidate scan
    TAKE_AFTER_DISPEL = 5,    // RegionManager::TryTakeGarbageRegionAfterDispel
    SITE_COUNT = 6
};

bool AccountOn();
bool InjectHandbackOn();

// The reclaim-side predicate. True ⇒ caller must keep the region.
//
// Returns false for an unheld region with no work done beyond one relaxed byte load, so the
// product path pays a load and a branch whether or not the gate is on.
//
// With MRT_GCV2_ROUTE_DEST_INJECT_HANDBACK=1 this returns false even for a held region, after
// counting it — that is the negative arm.
bool HoldsBack(const RegionInfo* region, Site site);

// Called from RegionInfo::InitRegionInfo when a region is re-initialised for reuse.
// held != 0 is the defect: TakeRegion already ran ClearUnits over this payload.
void NoteReuse(const RegionInfo* region, bool held);

// Called from the reclaim funnels (ReclaimRegion / ReclaimRegionToMarkQuarantine /
// ReleaseRegion) to answer the enumeration question none of the designs could close by
// reading: can a route destination reach these at all? Counts only, never aborts.
void NoteReclaimFunnel(const RegionInfo* region, const char* site);

// Called once per route generation from PrepareFromRegionList, immediately before the holds
// are dropped. regions/bytes are the gauge: holds that leak show up as monotonic growth.
void NoteClearPoint(size_t heldRegions, size_t heldBytes);

// Part A: default-off comparison of the to-region-2 arm's two resolutions. arith is
// GetUnitAddress(idx); this looks up GetRegionInfo(idx)->GetRegionStart() and counts
// mismatches, which are exactly the case where the recorded unit has since been absorbed
// as a SUBORDINATE_UNIT and the route silently relocates to a different, lower base.
void NoteTo2Resolve(uintptr_t arith, uint32_t idx);

void DumpSummary();

} // namespace RouteDestHold
} // namespace MapleRuntime

#endif // MRT_ROUTE_DEST_HOLD_H
