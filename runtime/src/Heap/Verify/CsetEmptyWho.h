// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_CSET_EMPTY_WHO_H
#define MRT_CSET_EMPTY_WHO_H

#include <cstddef>

namespace MapleRuntime {
class RegionInfo;

// oldroots2: who points at objects on CSet-empty keep pages
// (live=0, residual headers not all FORWARDED).
//
// ZGC mark_old_roots walks StrongColored + StrongUncolored thread roots,
// including derived bases (zMark.cpp:797-836 / zUncoloredRoot.inline.hpp).
// GcPhaseEnum already visits derived via VisitHeapReferences
// (Mutator.cpp:980-1001, site GcPhaseEnum.derivedBase).
// CsetEmptyWho previously used VisitMutatorRoots (reg/slot only) and
// LoadPlain without peel, so coloured stack/derived roots classified NONE.
//
// Observe-only. Samples recorded at CSet select (PostTrace); classified in
// Preforward ScopedLightSync (mutators stopped, before relocate-start flip):
//   - Decode slots with GetTargetObject (peel colour only).
//   - Match every keep page by RegionInfo* (O(1)).
//   - Heap walk + static + VisitHeapReferences (stack+derived+exception+raw)
//     + finalizer + export + concurrency roots.
//
// Classes: STATIC / STACK / DERIVED / EXTRA_ROOT / YOUNG / OLD_UNMARKED /
// OLD_MARKED / NONE.
// holdersVisited/fieldsSeen/pageHits/stackSeen/derivedSeen/extraSeen are
// live-probe counters: sampled>0 ∧ holdersVisited=0 ⇒ dead probe, not
// "true garbage". Compile-time gate kCsetEmptyWho.
namespace CsetEmptyWho {

void BeginCycle();
void NoteKeep(RegionInfo* region, size_t residual, size_t residualFwd, size_t marked);
void ClassifyCycle();
void Report(const char* tag);

} // namespace CsetEmptyWho
} // namespace MapleRuntime

#endif // MRT_CSET_EMPTY_WHO_H
