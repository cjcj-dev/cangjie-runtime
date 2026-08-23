// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_PERM_WHO_ADMIT_H
#define MRT_PERM_WHO_ADMIT_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// permwho: population behind the permhole CHECK.
//
// The CHECK only fires when a route answer with no valid tip reaches the read barrier, which
// is rare. But every RouteObject answer can be classified without aborting, and the classes
// that matter are visible in ordinary runs:
//   - route state at answer time (ROUTED = copy still in flight, FORWARDED = published)
//   - whether the from object is itself object-FORWARDED (a copy happened for this address)
//   - whether the derived to is a valid object (a tip is actually there)
// A FORWARDED answer with fromFwd=0 and an invalid to is exactly the permhole precondition;
// counting it says whether the invariant is broken continuously or only in the crashing run.
//
// The route metadata is not retired when the region is collected: CollectRegion
// (RegionManager.h:338) leaves routeState / the ghost bit / liveInfo0 alone, and only
// DispelGhostFromRegion (RegionInfo.h:1215, reached from PrepareFromRegionList) clears them
// at the *next* cycle's POST_TRACE (WCollector.cpp:2067). Answers served inside that window
// are counted separately (rtype=GARBAGE).
//
// Gate (default off; product path returns before any work):
//   MRT_GCV2_PERMWHO_ADMIT=1
// Sample cap on detail lines: MRT_GCV2_PERMWHO_ADMIT_MAX=<N> (default 32)
namespace PermWhoAdmit {

bool Enabled();

// Called with the result of RegionManager::RouteObject (to may be null).
void NoteRoute(RegionInfo* region, BaseObject* from, BaseObject* to);

// Called at route planning time (RouteOrCompactRegionImpl, right where fromBytes is read).
//
// Two things are decided here from two different sources:
//   - the reservation size comes from the *counter*: fromBytes = GetLiveByteCount()
//     (RegionManager.cpp:1813), stored as toRegion1UsedBytes by SetRouteInfo
//   - each object's destination comes from the *bitmap*: GetRoute(preLiveBytes) is a
//     popcount prefix-sum (LiveInfo.h:229-241 → LiveInfo.cpp:15-24), and it switches to
//     to-region2 exactly when preLiveBytes >= toRegion1UsedBytes
// If the two disagree in magnitude, the reservation does not match the placement. Nothing
// checks that: VerifyLiveBooks (RegionInfo.h:1683) only tests the zero / non-zero homology
// (liveBytes==0 ⇔ IsKnownEmpty()), never equality of the two magnitudes.
//
// The tipnull densify block just above (RegionManager.cpp:1699-1808) makes them agree by
// construction — it rebuilds both faces from one size-walk — but only when the walk
// completes. densifyOutcome records whether it ran and, if not, why not.
//   0 = densified   1 = gate (not small / no ghost / knownEmpty)   2 = first walk incomplete
//   3 = nStarts==0  4 = malloc failed        5 = second walk broke on the gate
//   6 = second walk collected every start but stopped short of allocPtr (its own loop bound)
void NoteRoutePlan(RegionInfo* region, size_t fromBytes, unsigned densifyOutcome);

// Called on the abandon arm (RegionManager.cpp:2238), where a region that failed the receipt
// gate is dispelled and exempted instead of published.
//
// The arm's stated assumption is "GetGhostFromRegionAt null ⇒ RouteObject miss ⇒ mutator
// keeps from". That holds only for objects that were never copied. Objects the same pass
// already copied carry ObjectState::FORWARDED in their own header (set at
// WCollector.cpp:6218), and nothing ever clears it — every SetStateCode(NORMAL) in the tree
// is applied to the to-copy, never to a from object. The region survives (Exempt), so next
// cycle PrepareForwardableRegion can route it again, and RegionInfo.h:1155 wipes the
// RouteInfo those headers were receipts for. A consumer that then reaches such an object
// takes the from->IsForwarded() branch and asks for a to under the *new* plan.
//
// forwardedObjects is how many stale receipts this abandoned region leaves behind.
void NoteAbandon(RegionInfo* region, size_t walkedObjects, size_t forwardedObjects);

void DumpSummary();

} // namespace PermWhoAdmit
} // namespace MapleRuntime

#endif // MRT_PERM_WHO_ADMIT_H
