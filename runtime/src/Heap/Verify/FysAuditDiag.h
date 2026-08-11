// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FYS_AUDIT_DIAG_H
#define MRT_FYS_AUDIT_DIAG_H

#include <cstddef>
#include <unordered_set>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class BaseObject;

// fysreport: FYS as auditor (not safety net).
//
// Gate (default OFF — O(heap); product early-return before any walk):
//   MRT_GCV2_FYS_AUDIT=1  OR  MRT_GCV2_DIAG contains fysaudit|all
// Sample cap:   MRT_GCV2_FYS_AUDIT_SAMPLES=<N> (default 32)
// Dedup cap:    MRT_GCV2_FYS_AUDIT_DEDUP=<N> (default 4096)
//
// Semantics when on:
//   product mark/rescan = FYS=0 (ForceProductFullYoungScanFalse)
//   audit walk = full non-young holder enumeration (stronger than FYS root-closure)
//   missing O→Y edges are RECORDED only — never admitted into the mark closure
//
// Classification (REPORT-fysaudit.md §3.1; no invented classes):
//   D1 producer never recorded (mutator remset miss at pre-pinned)
//   D2 recorded edge would be dropped by FYS0 retained-holder liveness
//   D3 weak holder referent slot (weak/strong type loss under FYS0)
//   D4 liveRemembered vs consumed ledger gap (post-rescan)
//   unclassified — keep raw fields; do not force into D1–D4
//
// D5 note: this auditor does NOT filter by reachableSlots (FYS remset filter).
// Young concurrent is default OFF; no concurrent-remset blind zone in the audit walk.

namespace FysAuditDiag {

bool Enabled();

// When audit is on, product path must behave as FYS=0 (observe only).
bool ForceProductFullYoungScanFalse();

void OnMinorBegin(size_t minorRunIndex);

// Pre-RecordPinnedCrossGenEdges: mutator remset snapshot + full non-young walk.
// Classifies D1/D2/D3 (and unclassified). Does not mutate remset or mark state.
void CensusPrePinned(size_t minorRunIndex);

// After RescanRememberedSet under product FYS=0: D4 ledger gap + retained-drop D2.
void PostRescan(const std::unordered_set<MAddress>& rememberedSlots,
                const std::unordered_set<MAddress>& liveRememberedSlots,
                const std::unordered_set<MAddress>& consumedSlots,
                const std::unordered_set<MAddress>& weakSlots);

void Report(const char* tag);

void DumpProcessTotals(const char* tag);

} // namespace FysAuditDiag
} // namespace MapleRuntime

#endif // MRT_FYS_AUDIT_DIAG_H
