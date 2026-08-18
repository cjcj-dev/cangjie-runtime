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
#ifndef MRT_GARBREGION_DIAG_H
#define MRT_GARBREGION_DIAG_H

#include <cstddef>

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// garbregion: live-slot census into a region at CollectRegion / F3 join.
// Gate: MRT_GCV2_GARBREGION=1 or MRT_GCV2_DIAG token garbregion. Default off.
//
// CensusBeforeForward walks the heap once (ForEachObj, same shape as FysAuditDiag)
// and counts heap slots whose target lands in each region. NoteCollectEnter
// joins that snapshot to the region being marked GARBAGE. NoteF3Join joins
// FixOldTaggedRefField's region_garbage / region_free reasons to the same row.

namespace GarbRegionDiag {

bool Enabled();

void CensusBeforeForward(const char* where);

void NoteCollectEnter(RegionInfo* region);

void NoteF3Join(RegionInfo* latestRegion, BaseObject* latest, const char* reason);

void Report(const char* point);

} // namespace GarbRegionDiag
} // namespace MapleRuntime

#endif // MRT_GARBREGION_DIAG_H
