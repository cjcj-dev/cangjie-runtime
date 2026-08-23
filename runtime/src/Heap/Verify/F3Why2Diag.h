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
#ifndef MRT_F3WHY2_DIAG_H
#define MRT_F3WHY2_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// livesame / f3why2: CollectRegion enter class + knownEmpty_marked + F3 join.
// Also ORDER samples on ForwardRegion success path (live vs mark timing).

namespace F3Why2Diag {

// Gate: MRT_GCV2_F3WHY2=1 or MRT_GCV2_DIAG token f3why2. Default off.
bool Enabled();

void NoteCollectEnter(RegionInfo* region);

void NoteF3RegionGarbage(RegionInfo* latestRegion, BaseObject* latest);

// Call on FORWARDED success path: liveBeforeReset, markedBefore, liveAfterReset,
// markedAfterReset, markedAfterInvalidate (after epoch bump).
void NoteForwardOrder(RegionInfo* region, uint64_t liveBefore, size_t markedBefore, uint64_t liveAfterReset,
                      size_t markedAfterReset, size_t markedAfterInvalidate);

void Report(const char* point);

// Size-walk mark count for ORDER samples (same as enter class).
void CountMarks(RegionInfo* region, size_t& validOut, size_t& markedOut);

} // namespace F3Why2Diag
} // namespace MapleRuntime

#endif // MRT_F3WHY2_DIAG_H
