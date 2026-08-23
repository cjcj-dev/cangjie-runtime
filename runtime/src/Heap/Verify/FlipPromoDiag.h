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
#ifndef MRT_FLIP_PROMO_DIAG_H
#define MRT_FLIP_PROMO_DIAG_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// flippromo: measure whether immediate promotion remset replay leaks vs oracle / broad old.
//
// Gate (default OFF — product early-return before any set work):
//   MRT_GCV2_FLIPPROMO=1  OR  MRT_GCV2_DIAG contains flippromo|all
//
// Two same-round diffs (task flippromo §③/§④):
//   (1) promote-site oracle: full O→Y walk of the region being demoted, set-diff vs product records
//   (2) next-minor broad subset: RecordPinnedCrossGenEdges edges on flip-promoted holders
//       vs the product set saved for those holders last minor
//
// Positive control (same dump line): oracleEdges / broadPromoEdges / promoRegions must be non-zero
// under natural_wave before a zero-leak claim is valid.

namespace FlipPromoDiag {

bool Enabled();

// Product path just recorded (or considered) an O→Y slot during promotion.
// path: 0=RecordPromotedCrossGenEdges 1=ForwardRegion-inline 2=residual(same as 0)
void NoteProductRecord(MAddress slot, unsigned path);

// Region is about to leave young via promote (before or after product walk).
// Call after product walk for that region so product set is complete.
// Runs oracle O→Y walk and accumulates set-diff counters.
void NotePromotedRegion(RegionInfo* region, unsigned path, size_t productRecorded);

// Start of minor: broad old scan about to run — reconcile last-cycle flip-promoted set
// against edges the broad walk finds on those regions (observe only; does not mutate remset).
void OnBroadScanBegin(size_t minorRunIndex);

// Broad scan found an O→Y edge (called from RecordPinnedCrossGenEdges when probe on).
void NoteBroadRecord(RegionInfo* holderRegion, MAddress slot);

// After residual promote + Consume in EvacuateFromSpace finish.
void OnPromotePhaseEnd(size_t minorRunIndex, size_t promoteReplay, size_t residualPromote);

void DumpProcessTotals(const char* tag);

} // namespace FlipPromoDiag
} // namespace MapleRuntime

#endif // MRT_FLIP_PROMO_DIAG_H
