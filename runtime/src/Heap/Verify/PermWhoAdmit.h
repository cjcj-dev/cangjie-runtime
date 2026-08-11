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

void DumpSummary();

} // namespace PermWhoAdmit
} // namespace MapleRuntime

#endif // MRT_PERM_WHO_ADMIT_H
