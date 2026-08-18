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
#ifndef MRT_IDLE_EDGE_DIAG_H
#define MRT_IDLE_EDGE_DIAG_H

#include "Common/TypeDef.h"
#include "Heap/Collector/Collector.h"

namespace MapleRuntime {
class BaseObject;

// idleedge: quantify old→young edges present on the heap at minor STW that are
// NOT in the mutator remset (pre-RecordPinnedCrossGenEdges). Those are the edges
// only census / FYS would recover — the Idle bare-store window plus other write
// gaps.
//
// Gate (default off; product path early-return before counters):
//   MRT_GCV2_IDLEEDGE=1  OR  MRT_GCV2_DIAG contains idleedge|all
// Cost control: MRT_GCV2_IDLEEDGE_EVERY=<N> (1-based invoke skip, default 1)
// Sample cap:   MRT_GCV2_IDLEEDGE_MAX_SAMPLES=<N> (default 8)
// Stamp size:   MRT_GCV2_IDLEEDGE_STAMP_BITS=<16..22> (default 18)
// Self-test:    MRT_GCV2_IDLEEDGE_SELFTEST=1 / MRT_GCV2_DIAG_SELFTEST=1 / DIAG+=selftest
// No TLS.
//
// stampfix: write-stamp table is cleared after each census (per-minor generation).
// Cross-minor stamp retention was the root of missRecordedLost false positives
// (fwdlost) and end-of-run 100% saturation (idlewrite). Table only needs one
// minor window of barrier decisions (~oldToYoungEdges scale under load).
//
// bare / no_stamp is split:
//   missBareNeverSeen  — census edge with no stamp and key not a store-eviction victim
//   missBareDisplaced  — census edge with no stamp but key was evicted by open-address
//                        force-overwrite (probe fail). Process totals keep missBare =
//                        neverSeen + displaced for allocblack compatibility.
//
// Counter health expectations are emitted once as [GCV2][diag][LEGEND] and each
// census as [GCV2][diag][HEALTH]; stamp occupancy >50% shouts INSTRUMENT_SATURATED.
// When saturated, missBare reclass is untrustworthy (refuse as numbers, not silent).
//
// When gated off every entry is a no-op (gates-off equivalence).

namespace IdleEdgeDiag {

bool Enabled();

// holderGen/targetGen: 0=unknown 1=young 2=old 3=nonheap (promoteedge).
// skipReason (idlewrite; same numbering as RemsetPhaseProbe::SkipReason):
//   0=recorded 1=no_young 2=ref_null_or_nonheap 3=ref_not_young
//   4=holder_null_or_nonheap 5=holder_young 6=unknown 7=no_stamp(miss-only)
// holderObjGen: generation of `obj` argument when present (else 0); used to detect
// field-addr gen vs object-header gen mismatch at the early-exit site.
// Called from Barrier::RecordCrossGenEdge when an edge is evaluated.
// Records write-time GC phase + gen + skip arm for later miss attribution.
// Fail-open: no-op when gate off.
void NoteBarrierDecision(MAddress fieldAddress, GCPhase phase, bool recorded, uint8_t holderGen,
                         uint8_t targetGen, uint8_t skipReason = 0, uint8_t holderObjGen = 0);

// STW census: snapshot remset, walk all non-young holders, count old→young
// edges vs remset membership. Call immediately BEFORE RecordPinnedCrossGenEdges
// so census/FYS-only edges are still visible as remset misses.
void CensusPrePinnedStamp(size_t minorRunIndex);

// Process-level totals (also printed each census when enabled).
void DumpProcessTotals(const char* tag);

// fullclear: stamp promote-time target generation for a field slot.
// Gate: MRT_GCV2_FULLCLEAR_PROBE=1 OR MRT_GCV2_DIAG contains fullclear|all.
// Early-return before any counter.
// targetGen: 0=unknown 1=young 2=old 3=null/nonheap.
// recorded: whether promote path called RememberedSet::Record.
void NotePromoteTimeTarget(MAddress fieldAddress, uint8_t targetGen, bool recorded);

// Positive-control arm: force stamp stress + synthetic miss classification.
// Gate: selftest envs (see DiagGate). Safe no-op when off.
void RunSelfTest();

} // namespace IdleEdgeDiag
} // namespace MapleRuntime

#endif // MRT_IDLE_EDGE_DIAG_H
