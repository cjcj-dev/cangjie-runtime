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
#ifndef MRT_OFFPAST_DIAG_H
#define MRT_OFFPAST_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;
class RegionInfo;

// offpast: same-target pregrant → Route/Compact → Fix face (UNSURE1).
// Gate default off. Product path early-returns before any table work.
//   MRT_GCV2_OFFPAST=1
// Heartbeat: one "[GCV2][offpast] heartbeat" line when first armed.
namespace OffpastDiag {

bool Enabled();

// Every young root seen at pregrant / grant-pass. Caps silently.
void NotePregrant(BaseObject* obj, const char* site);
void NotePregrantSlot(void* slot, BaseObject* obj, const char* site);

// Region about to freeze geometry (RouteOrCompactRegionImpl entry).
void NoteRouteEnter(RegionInfo* region);

// After CompactRegion packed + memset (both overloads).
void NoteCompactDone(RegionInfo* region);

// Fix admit_miss / leave-alone on a root target.
void NoteFixMiss(BaseObject* obj);
void NoteFixMissSlot(void* slot, BaseObject* obj);

} // namespace OffpastDiag
} // namespace MapleRuntime

#endif
