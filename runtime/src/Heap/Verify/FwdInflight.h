// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
#ifndef MRT_FWD_INFLIGHT_H
#define MRT_FWD_INFLIGHT_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// fwdinflight: how wide is the window between "a thread is reading from-side route state"
// and "that state is retired"?
//
// The question this exists to answer, and why it is not already answered:
//
// ZGC's forwarding lookup touches no page memory at all. ZForwarding caches _virtual,
// _object_alignment_shift, _from_age and _partition_id by value at construction
// (zForwarding.inline.hpp:60-65) and the entry table is a ZAttachedArray owned by the
// ZForwardingAllocator (:53-57), so ZForwarding::find() reads only that table. That is why
// ZRelocate::relocate_object can call find() with no gate (zRelocate.cpp:386-390) and only
// takes ZForwarding::retain_page before relocate_object_inner, which copies out of the
// from-page (zRelocate.cpp:369). retain_page/_ref_count protect the FROM page, not the
// to-version.
//
// Our lookup is not like that. FindToVersion -> RegionManager::RouteObject ->
// RegionInfo::AdmitForRoute dereferences from-side state three times:
//   RegionInfo.h:1494      metadata.liveInfo0, then IsSurvivedObject over it
//   RegionInfo.h:1511      fromObj->GetTypeInfo() -- the from object's own payload
//   RegionInfo.h:1759/1771 fromObj->IsValidObject(), GetPreLiveBytesInGhostRegion
//
// and the three edges that retire that state do not wait for readers:
//   RegionInfo.h:1943 DispelGhostFromRegion  -- an unconditional bit-flip loop
//   RegionManager.cpp:1526 TakeRegion garbage reuse -- ClearUnits with no region rwLock
//   ForwardDataManager.cpp:41-43 -- nulls the liveInfo pointers, then madvises the arena
//
// The last one states its own guarantee in a comment: "Structural guarantee: no region
// field may still address the range about to be madvise'd." Nulling the pointers is not a
// drain: a thread that already loaded liveInfo0 into a local at RegionInfo.h:1494 is not
// covered by it. ZGC's equivalent edge, ZForwarding::detach_page (zForwarding.cpp:171-181),
// blocks on _ref_lock until _ref_count reaches zero before the page is freed.
//
// This instrument measures the exposure without changing it. A thread publishes the region
// it is resolving for the duration of the lookup; each retire edge scans the published
// slots and counts the readers it is about to invalidate. It never blocks and never alters
// control flow, so a non-zero count is evidence, not a fix.
//
// Gates (all default off; the product path early-returns before any store):
//   MRT_GCV2_FWDINFLIGHT=1  or  MRT_GCV2_DIAG=fwdinflight
//       Enable publication, scanning, and the summary.
//   MRT_GCV2_FWDINFLIGHT_INJECT=1
//       Positive control on the SAME binary. Before each per-region scan, plant a synthetic
//       published slot naming the very region being retired, so the scan MUST find at least
//       one reader. This is what makes "hits=0" distinguishable from "the scan never ran" or
//       "publication was never wired" -- with inject on, injected_seen must equal retires;
//       with inject off it must be 0. A run whose control arm does not hold is not evidence.
//
// Headline number: hits -- a retire edge that ran while a reader was inside the route lookup
// for that same region. Counted once per (retire event, reader), uncapped.

namespace FwdInflight {

// Where a reader publishes from.
enum class Site : uint32_t {
    ROUTE_WITH_REGION = 0, // RegionManager::RouteObject(fromObj, fromRegionInfo)
    ROUTE_LOOKUP = 1,      // RegionManager::RouteObject(fromObj)
    SITE_COUNT = 2
};

// Which edge is retiring the state.
enum class Retire : uint32_t {
    DISPEL_GHOST = 0,  // RegionInfo::DispelGhostFromRegion
    TAKE_GARBAGE = 1,  // RegionManager::TakeRegion garbage reuse, immediately before ClearUnits
    ARENA_RELEASE = 2, // ForwardDataManager::ClearPreviousForwardData, before ReleaseMemory
    RETIRE_COUNT = 3
};

bool Enabled();
bool InjectOn();

// Publication window. Cheap and non-blocking: one release store on entry, one on exit.
// Disabled builds/runs pay a call to Enabled() and a branch -- the same shape AdmitForRoute
// already pays for EatArmDiag::Enabled() (RegionInfo.h:1580).
class Scope {
public:
    Scope(const RegionInfo* region, Site site);
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

private:
    bool armed;
};

// Per-region retire edge. Counts published readers still inside a lookup on this region.
void NoteRetireRegion(const RegionInfo* region, Retire retire);

// Global retire edge: the liveInfo/bitmap arena madvise has no single owning region, so
// count every reader in flight anywhere. Reported separately from the per-region hits.
void NoteRetireGlobal(uintptr_t rangeStart, size_t rangeSize, Retire retire);

void DumpSummary();

} // namespace FwdInflight
} // namespace MapleRuntime

#endif // MRT_FWD_INFLIGHT_H
