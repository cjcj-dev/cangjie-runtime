// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REMSET_PHASE_PROBE_H
#define MRT_REMSET_PHASE_PROBE_H

#include "Common/TypeDef.h"
#include "Heap/Collector/Collector.h"

#ifdef REASON_UNKNOWN
#undef REASON_UNKNOWN
#endif

namespace MapleRuntime {
namespace RemsetPhaseProbe {

// Product diagnostic gate (default off): MRT_GCV2_RECORD_REMSET_EVENTS=1.
// MRT_GCPHASE_PROBE=1 remains accepted for compatibility with earlier evidence scripts.

// skipReason for NoteWrite / MISSING attribution (observation-only names).
enum SkipReason : uint8_t {
    REASON_RECORDED = 0,
    REASON_NO_YOUNG = 1,
    REASON_REF_NULL_OR_NONHEAP = 2,
    REASON_REF_NOT_YOUNG = 3,
    REASON_HOLDER_NULL_OR_NONHEAP = 4,
    REASON_HOLDER_YOUNG = 5,
    REASON_UNKNOWN = 6,
    REASON_NO_STAMP = 7, // MISSING slot never seen by RecordCrossGenEdge
};

// Barrier class derived from InstallBarrier phase mapping (not a separate type).
enum BarrierClass : uint8_t {
    BAR_UNDEF = 0,
    BAR_IDLE = 1,
    BAR_ENUM = 2,
    BAR_TRACE = 3, // TRACE + CLEAR_SATB share TraceBarrier
    BAR_POST_TRACE = 4,
    BAR_PREFORWARD = 5,
    BAR_FORWARD = 6,
    BAR_OTHER = 7,
};

bool Enabled();
BarrierClass PhaseToBarrierClass(GCPhase phase);
const char* PhaseName(GCPhase phase);
const char* BarrierClassName(BarrierClass bc);
const char* SkipReasonName(SkipReason r);

// Called from RecordCrossGenEdge on every old→young candidate decision.
void NoteWrite(MAddress fieldAddress, GCPhase phase, SkipReason reason, bool recorded);

// Called when VERIFY_REMSET counts a MISSING slot.
void NoteMissing(MAddress fieldAddress);

// Dump cumulative counters to VLOG REPORT (call at end of remset verify or process exit).
void DumpSummary(const char* tag);

// Optional: clear per-slot stamps between minors (keeps process totals).
void ClearSlotStamps();

// Force-record gate for blocking experiment: when set, skip early returns that drop edges.
// MRT_GCPHASE_FORCE_RECORD=1 ⇒ treat as always-record when edge is old→young.
bool ForceRecordEnabled();

} // namespace RemsetPhaseProbe
} // namespace MapleRuntime

#endif // MRT_REMSET_PHASE_PROBE_H
