// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REMSET_PHASE_PROBE_H
#define MRT_REMSET_PHASE_PROBE_H

#include "Common/TypeDef.h"
#include "Heap/Collector/Collector.h"

// On Windows this header is parsed *after* <windows.h>, which reaches it as
//   VerifyRememberedSet.cpp
//     -> Heap/Allocator/RegionSpace.h -> Mutator/Mutator.h
//     -> os/Windows/UnwindWin.h -> os/Windows/WinModuleManager.h -> <windows.h>
// and <windows.h> defines a family of REASON_* macros (winreg.h:200 defines
// REASON_UNKNOWN, which expands through reason.h down to the integer literal
// 0x000000ff).  A macro-expanded enumerator name fails as "expected identifier"
// pointing at winreg.h, which is a long way from the mistake.  Trip here
// instead, at the declaration site, if a platform header ever claims one of the
// names below.
#if defined(REASON_RECORDED) || defined(REASON_NO_YOUNG) || \
    defined(REASON_REF_NULL_OR_NONHEAP) || defined(REASON_REF_NOT_YOUNG) || \
    defined(REASON_HOLDER_NULL_OR_NONHEAP) || defined(REASON_HOLDER_YOUNG)
#error "a platform header defines one of the REASON_* names declared below; \
rename the alias rather than #undef-ing the platform macro"
#endif

namespace MapleRuntime {
namespace RemsetPhaseProbe {

// Product diagnostic gate (default off): MRT_GCV2_RECORD_REMSET_EVENTS=1.
// MRT_GCPHASE_PROBE=1 remains accepted for compatibility with earlier evidence scripts.

// skipReason for NoteWrite / MISSING attribution (observation-only names).
enum class SkipReason : uint8_t {
    Recorded = 0,
    NoYoung = 1,
    RefNullOrNonheap = 2,
    RefNotYoung = 3,
    HolderNullOrNonheap = 4,
    HolderYoung = 5,
    Unknown = 6,
    NoStamp = 7, // MISSING slot never seen by RecordCrossGenEdge
};

// Compatibility names used by the write barrier; the Windows-colliding
// REASON_UNKNOWN name is intentionally not exposed.
constexpr SkipReason REASON_RECORDED = SkipReason::Recorded;
constexpr SkipReason REASON_NO_YOUNG = SkipReason::NoYoung;
constexpr SkipReason REASON_REF_NULL_OR_NONHEAP = SkipReason::RefNullOrNonheap;
constexpr SkipReason REASON_REF_NOT_YOUNG = SkipReason::RefNotYoung;
constexpr SkipReason REASON_HOLDER_NULL_OR_NONHEAP = SkipReason::HolderNullOrNonheap;
constexpr SkipReason REASON_HOLDER_YOUNG = SkipReason::HolderYoung;

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
