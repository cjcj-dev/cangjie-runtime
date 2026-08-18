// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZGC_INVARIANTS_H
#define MRT_ZGC_INVARIANTS_H

#include "Common/TypeDef.h"

namespace MapleRuntime {
class BaseObject;
class Collector;

// Machine-checked global invariants, as opposed to ported functions.
//
// Why this file exists.  Every mechanism ZGC uses is already in this tree and is faithful to it:
// ZgcSelfHeal is a line-by-line port of ZBarrier::self_heal, DrainScope carries ZForwardingLife's
// own names for detach_page/claim_page, is_young_load_good / is_old_load_good / remap_generation
// all exist including the double-remap side-table path, ColourTypes.h mirrors zAddress.hpp's three
// enum classes, and the flip masks start from the same values ZGC starts from.  Twelve separate
// "we must be missing ZGC's X" hypotheses were checked against the source in one session and every
// one of them found X present.
//
// The defect that survived all twelve is a *state* ZGC never enters, and no ZGC file says so --
// correctness there lives in the states the design excludes, not in any function you can copy.
// Copying more functions cannot find it; asserting the excluded states can.
//
// So this subsystem states those exclusions directly.  Each invariant is one predicate, checked
// where the state would first become observable, counted rather than assumed.
//
//   I1  a load-good slot names a live to-version
//       -- the barrier's fast path hands the value straight to the mutator without looking at it,
//          so if the colour says good the target must actually be usable.  Measured violations:
//          slotGood=1 with the target in a ghost-from region and its header still FORWARDED, 20+
//          per run, which the compiler then reads as one 64-bit word and faults on (stateCode 3 at
//          bits 48-49 becomes 3 << 48 inside an address).
//
// Discipline for anything added here: it must be *live*.  41 of the 51 gated subsystems under
// Heap/Verify are `return false;` and empty bodies behind headers that still document their gates,
// which makes a zero from them unreadable.  A new invariant that is not implemented must say
// HOLLOWED in this header (runtime/tests/check_diag_not_hollow.py enforces that), and every
// counter must emit its zero case so a silent guard cannot be mistaken for a clean run.
namespace ZgcInvariants {

bool Enabled();

// I1: called at the one point every reference reaches the mutator.
void CheckLoadGoodTarget(BaseObject* target, const Collector& collector, uint8_t phase);

// Full cross product of (slot colour, current good colour, target state code, route state, ghost,
// young) -- the table, not a sample.  Twelve hypotheses were each a guess at which combination is
// the bad one; enumerating removes the guessing.
void NoteState(uintptr_t slotRaw, uintptr_t slotRawSecondRead, uintptr_t goodMask, BaseObject* target);
void DumpCensus(const char* why);

// Called from the read barrier's load-good fast path, with the exact value that arm accepted.
void NoteFastPathAccept(uintptr_t slotRaw, BaseObject* target);

// staleguard fired: the hand-out named an object whose header says it has moved, and we resolved.
void NoteStaleGuardFired(bool zeroHeader, bool resolved, BaseObject* target);

// Totals, including the zero case.
void DumpSummary(const char* why);

} // namespace ZgcInvariants
} // namespace MapleRuntime

#endif // MRT_ZGC_INVARIANTS_H
