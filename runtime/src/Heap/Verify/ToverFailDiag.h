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
#ifndef MRT_TOVERFAIL_DIAG_H
#define MRT_TOVERFAIL_DIAG_H

#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// toverfail: why the read-barrier resolve path fails to produce a to-version.
// Gate MRT_GCV2_TOVERFAIL=1 or MRT_GCV2_DIAG token toverfail. Default off; early-return
// before any counter when off (DiagGate rule).
//
// Arms (fromver §8④ + task):
//   甲 route_miss     — RouteObject returned null (plan gone)
//   乙 no_ghost       — GetGhostFromRegionAt null / wrong generation (DispelGhost-class)
//   丙 tip_unfilled   — WaitRoutedTipReady / ForwardObject soft-miss kept from
//   丁 unmovable_skip — IsUnmovableFromObject short-circuits the whole resolve
//                      (counted separately; not folded into "fail")
//
// Positive controls sit next to the signature counters so a zero cannot mean "probe dead":
//   slow_enter / resolve_enter / remap_call / remap_ok / fwd_ok / loadgood_fast.

namespace ToverFailDiag {

bool Enabled();

// ForwardBarrier::ReadReference / AtomicReadReference: after is_load_good test.
// unmovable and stateCode are taken at the barrier decision moment (not post-return).
void NoteSlowEnter();
void NoteLoadGoodFast();
void NoteUnmovableSkip(BaseObject* oldTarget, unsigned stateCode, unsigned isForwarded);
void NoteResolveEnter();
// after make_load_good + optional ForwardObject: did we hand back the same address?
void NoteResolveOutcome(BaseObject* oldTarget, BaseObject* loadGood, unsigned moved);

// Collector::make_load_good
void NoteMlgEnter();
void NoteMlgKeepFrom(); // remapped == nullptr || remapped == target
void NoteMlgMoved();    // remapped != target

// WCollector::relocate_or_remap_object arms
void NoteRemapCall();
void NoteRemapNonHeap();
void NoteRemapNoGhost();   // 乙
void NoteRemapRouteNull(); // 甲
void NoteRemapReceipt();   // tip-valid to (or non-heap to)
void NoteRemapWait();      // entered WaitRoutedTipReady
void NoteRemapWaitTip();
void NoteRemapWaitGiveUp(); // 丙 (wait bound / plan gone → keep from)

// WCollector::ForwardObject
void NoteFwdEnter();
void NoteFwdOk();   // returned a different address
void NoteFwdNull(); // soft miss → nullptr (tipnull keep-from)
void NoteFwdSame(); // returned obj

void Report(const char* why);

} // namespace ToverFailDiag
} // namespace MapleRuntime

#endif // MRT_TOVERFAIL_DIAG_H
