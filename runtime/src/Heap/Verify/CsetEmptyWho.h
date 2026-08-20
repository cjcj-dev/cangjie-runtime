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

// oldroots: who points at objects on CSet-empty keep pages
// (live=0, residual headers not all FORWARDED).
//
// ZGC old mark is a full old-generation closure from strong roots
// (zGeneration.cpp:1086-1090 concurrent_mark → mark_roots + mark_follow;
//  zMark.cpp:938-941 mark_old_roots). Ours seeds only young→old
// (SeedOldMarkFromYoungSurvivors). Keep pages with marked=0 mean the
// old TRACE never reached those objects.
//
// Observe-only. Samples recorded at CSet select (PostTrace); classified in
// Preforward ScopedLightSync (mutators stopped, before relocate-start flip):
//   - Decode slots with GetTargetObject (peel colour only).
//   - Match every keep page by RegionInfo* (O(1)), not first-two-page range.
//   - Heap walk + static + stack + finalizer + export + concurrency roots.
//
// Classes: STATIC / STACK / EXTRA_ROOT / YOUNG / OLD_UNMARKED / OLD_MARKED / NONE.
// holdersVisited/fieldsSeen/pageHits/stackSeen/extraSeen are live-probe
// counters: sampled>0 ∧ holdersVisited=0 ⇒ dead probe, not "true garbage".
namespace CsetEmptyWho {

void BeginCycle();
void NoteKeep(RegionInfo* region, size_t residual, size_t residualFwd, size_t marked);
void ClassifyCycle();
void Report(const char* tag);

} // namespace CsetEmptyWho
} // namespace MapleRuntime

#endif // MRT_CSET_EMPTY_WHO_H
