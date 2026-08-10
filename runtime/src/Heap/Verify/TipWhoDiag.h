// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_TIP_WHO_DIAG_H
#define MRT_TIP_WHO_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;
class RegionInfo;

// tipwho: classify survivors that reach region FORWARDED without a tip receipt.
//
// Gate (default off; product path early-return BEFORE any work):
//   MRT_GCV2_TIPWHO=1
// Sample cap: MRT_GCV2_TIPWHO_MAX=<N> (default 64)
//
// Sites:
//   NoteSoftReturn  — ForwardObject returned without object FORWARDED (or null)
//   NoteVisitGate   — VisitLiveObjectsUntilFalse Plausible reject mid-walk
//   NotePrePublish  — census before SetRouteState(FORWARDED)
//   NotePermHole    — WaitRoutedTipReady permanent-hole path (enrich from)

namespace TipWhoDiag {

bool Enabled();

// branch: short tag for soft-return arm (e.g. "fwd_plausible", "fwd_null_ghost", "fwd_keep_from")
void NoteSoftReturn(BaseObject* obj, RegionInfo* region, const char* branch, BaseObject* toObj);

// VisitLive gate rejected obj at position; walk returns true without visiting rest.
// reason: product gate arm (null-tip / tip-small-int / tip-in-heap / dead-region / non-heap).
// tipWord0/1: first 16B of object header raw (little-endian host words).
void NoteVisitGate(BaseObject* obj, RegionInfo* region, size_t offset, size_t position,
                   const char* reason, uintptr_t tipAddr, uint64_t tipWord0, uint64_t tipWord1,
                   size_t nextSurvOff, size_t survAfter);

// Before FORWARDED: count size-walk starts vs object-FORWARDED vs multi-bit interiors.
void NotePrePublish(RegionInfo* region);

// Permanent hole: classify `from` that still admits a geometric to with null tip.
void NotePermHole(BaseObject* from, RegionInfo* region, BaseObject* geometricTo, const char* reason);

// Mark-side paint ring: who called MarkObject/MarkBits for (region, offset).
// site: short tag e.g. "WCollector::MarkObject"
void NotePaint(RegionInfo* region, BaseObject* obj, size_t offset, size_t objSize, const char* site,
               void* ra0, void* ra1);

// At walk_break: look up paint for nextSurvOff (and log).
void NotePaintLookup(RegionInfo* region, size_t queryOff, const char* context);

} // namespace TipWhoDiag
} // namespace MapleRuntime

#endif // MRT_TIP_WHO_DIAG_H
