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
#ifndef MRT_GOOD_PRED_DIAG_H
#define MRT_GOOD_PRED_DIAG_H

#include <cstdint>

// goodpred: which definition of load-good the read barriers run, and how often the two
// definitions in this tree disagree on live data.
//
// The tree carries two:
//   legacy  Collector.h:159        !is_null && is_young_load_good && is_old_load_good
//   zgc     ColourPredicates.h:116 has_address && !is_load_bad && is_remapped
//             == OpenJDK ZPointer::is_load_good (zAddress.inline.hpp:631-633)
//
// They differ on a from-object reference that carries the current remap colour
// (legacy answers good because both generation bits are set; zgc answers by the
// published load-bad mask). Mid-evacuation is no longer a pointer bit.
//
// Mode is published once at library load and read on the barrier fast path.
//   MRT_GCV2_ZGC_LOADGOOD=1    barriers use the ZGC definition (default off)
//   MRT_GCV2_LOADGOOD_AUDIT=1  evaluate both and census the disagreement; the answer
//                              returned still follows MRT_GCV2_ZGC_LOADGOOD, so
//                              AUDIT=1 alone is an observation arm with product behaviour
// Both off costs one byte load and one predicted-not-taken branch per call.

namespace MapleRuntime {
namespace GoodPredDiag {

enum Mode : uint8_t {
    kLegacy = 0, // product definition, no counters
    kZgc = 1,    // ZGC definition, no counters
    kAudit = 2,  // both definitions evaluated + censused; answer follows kApply below
};

// Which caller asked. Only the first two can diverge: is_mark_good/is_store_good reach the
// predicate having already required (value & g_cjMarkBadMask) == 0, and g_cjMarkBadMask is a
// superset of g_cjLoadBadMask, so the only remap bit such a value may carry is the current one
// -- which is exactly what both definitions then test for. Counting them in one total hides a
// zero denominator behind tens of millions of reads that were never at risk.
enum Site : uint8_t {
    kSiteBarrier = 0,      // the six barrier fast paths
    kSiteMakeLoadGood = 1, // Collector::make_load_good
    kSiteMarkGood = 2,
    kSiteStoreGood = 3,
    kSiteCount = 4,
};

// Definition, not just declaration, lives in GoodPredDiag.cpp; both are dynamically
// initialised at load time, before any mutator or collector thread exists.
extern uint8_t g_mode;
extern bool g_applyZgc; // in kAudit: return the zgc answer rather than the legacy one

// Out-of-line: only reached in kAudit.
bool NoteAudit(uintptr_t value, bool legacy, bool zgc, uint8_t site);

// One-shot self-test, armed by MRT_GCV2_LOADGOOD_SELFTEST=1 and run on the first switched
// call. It hands the *product* virtuals a synthetic tagged-with-current-colour word and the
// same word without the tagged bits, and prints both answers from both definitions.
//
// It exists because the disagreement is rare: a run that reports diverge=0 has two readings --
// the two definitions agree on this data, or nothing selected between them. The self-test
// separates those without waiting for the workload to produce the construct.
bool SelfTestPending();
void ReportSelfTest(uintptr_t taggedValue, bool taggedLegacy, bool taggedZgc, uintptr_t plainValue,
                    bool plainLegacy, bool plainZgc);

void Report(const char* why);

} // namespace GoodPredDiag
} // namespace MapleRuntime

#endif // MRT_GOOD_PRED_DIAG_H
