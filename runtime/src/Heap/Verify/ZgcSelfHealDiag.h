// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZGC_SELF_HEAL_DIAG_H
#define MRT_ZGC_SELF_HEAL_DIAG_H

#include <cstdint>

#include "Common/ColourTypes.h"

// Instrumentation for the OpenJDK ZBarrier::self_heal port in
// ObjectModel/RefField.h (zBarrier.inline.hpp:72-110). Two gates:
//
//   MRT_GCV2_ZGC_SELFHEAL=1         run the ported loop AND census it
//   MRT_GCV2_ZGC_SELFHEAL_REPORT=1  census only; the product keeps the bounded
//                                   kSelfHealAttempts loop
//
// The second gate exists so the off arm is a real control: it prints the same
// census line with the counters at zero, which separates "the loop did not run"
// from "the census never printed".
//
// ZGC spells the transition checks as assert(), i.e. debug-build aborts. Here
// they count and sample instead. A non-monotonic transition is precisely the
// thing this port is being measured for -- ColourMask.h:202-206 records that
// our Forward-phase writers can re-tag a slot -- so aborting on one would
// destroy the observation rather than record it. MRT_GCV2_ZGC_SELFHEAL_ABORT=1
// restores ZGC's fatal behaviour for anyone who wants it.

namespace MapleRuntime {
namespace ZgcSelfHealDiag {

// True when the ported loop replaces the bounded one. Read through
// ZgcSelfHealEnabled() in RefField.h; declared here for the census.
bool Enabled();

// True when either gate is set. Cheap after first call.
bool CensusEnabled();

// zBarrier.inline.hpp:40-70 ZBarrier::assert_transition_monotonicity, as counters.
void CheckTransitionMonotonicity(zpointer oldPtr, zpointer healPtr);

// zBarrier.inline.hpp:83-86: assert(!fast_path(ptr)), assert(fast_path(heal_ptr)),
// assert(ZPointer::is_remapped(heal_ptr)).
void NotePreconditions(bool ptrFastPath, bool healFastPath, zpointer healPtr);

void NoteEnter();
void NoteNullSkip();
// zBarrier.inline.hpp:93-96 success. iterations is 0 on the first CAS.
void NoteHealed(unsigned iterations);
// zBarrier.inline.hpp:98-101 "Must not self heal": another barrier already made
// the slot acceptable to this barrier's fast path.
void NoteFastPathExit(unsigned iterations);
// zBarrier.inline.hpp:103-107 upgrade retry. This is the counter the bounded
// loop can never move: it gives up instead of re-applying the heal value.
//
// Also carries the livelock sentinel: every 1024 turns of one self-heal it
// counts a spin_alarm and samples a line. ⛔ It never breaks the loop, never
// falls back to the bounded one, and aborts only under
// MRT_GCV2_ZGC_SELFHEAL_ABORT=1 -- giving up would replace ZGC's convergence
// semantics with a different algorithm. ZGC has no such sentinel because on its
// side the state is unreachable; here the premise it relies on is not
// established, so the alarm is the evidence that it failed.
void NoteRetry(unsigned iterations);

void Report(const char* why);

} // namespace ZgcSelfHealDiag
} // namespace MapleRuntime

#endif // MRT_ZGC_SELF_HEAL_DIAG_H
