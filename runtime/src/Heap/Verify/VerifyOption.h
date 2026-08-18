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
#ifndef MRT_VERIFY_OPTION_H
#define MRT_VERIFY_OPTION_H

#include <cstdint>

namespace MapleRuntime {

// Anchor: HotSpot gc/shared/verifyOption.hpp:30-39
// Purpose: make the mark-information source used by a verifier *explicit*, so the
// verifier is not tautologically true against the same data it is checking.
//
// Our mark-information inventory (see REPORT-gcvheap MARK_SOURCES):
//   MinorClosure       — TraceYoungClosure reachableObjects
//   RegionMarkBitmap   — RegionInfo::IsMarkedObject bits written during trace
//   IndependentRetrace — verifier-local recompute from roots (ValidateYoungMarking body)
//   RememberedSet      — slot set (edge bookkeeping, not object marks)
//   FullHeapEnum       — Heap::ForEachObj (enumeration, not reachability)
//
// Env override: MRT_GCV2_VERIFY_MARK_SOURCE=
//   independent | minor-closure | region-bitmap | default
enum class VerifyMarkSource : uint8_t {
    // Default for young-marking: compare IndependentRetrace vs RegionMarkBitmap
    // (does NOT consult MinorClosure — avoids fullYoungScan tautology).
    IndependentVsBitmap = 0,
    // Old / trivial under fullYoungScan: also require membership in MinorClosure.
    MinorClosure,
    // Authority = region mark bitmap only (no independent retrace membership).
    RegionMarkBitmap,
    // Authority = independent retrace only.
    IndependentRetrace,
};

const char* VerifyMarkSourceName(VerifyMarkSource source);
// Parse MRT_GCV2_VERIFY_MARK_SOURCE; default IndependentVsBitmap.
VerifyMarkSource ParseVerifyMarkSource();

} // namespace MapleRuntime

#endif // MRT_VERIFY_OPTION_H
