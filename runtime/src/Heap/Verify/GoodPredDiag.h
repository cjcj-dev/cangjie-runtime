// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

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
// They differ on exactly one construct that the collector mints itself:
// GetAndTryTagRefField (WCollector.h:468-472) hands a from-object reference
// isTagged=1 | currentTagID | currentRemapColour. currentRemapColour is a member of both
// ZPointerRemappedYoungMask and ZPointerRemappedOldMask, so legacy answers good; the tagged
// bits are in TAGGED_BITS_MASK, which ComputeBadMasks folds into g_cjLoadBadMask, so zgc
// answers bad. Legacy therefore lets a mid-evacuation reference take the barrier fast path
// and hands the mutator the from address.
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

void Report(const char* why);

} // namespace GoodPredDiag
} // namespace MapleRuntime

#endif // MRT_GOOD_PRED_DIAG_H
