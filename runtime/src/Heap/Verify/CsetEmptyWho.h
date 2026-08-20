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
// old TRACE never reached those objects. This checker samples keep
// pages and classifies incoming edges:
//   STATIC       — static root names the object
//   YOUNG        — young-region holder field (young→old / 晋升 survivor 边)
//   OLD_MARKED   — already-marked old holder (TRACE follow 没走到这页)
//   OLD_UNMARKED — unmarked old holder (old→old 子图缺根)
//   NONE         — no heap/static incoming (true garbage, or stack-only)
// Stack roots are STACK_SKIPPED: ExemptFromRegions runs in PostTrace
// (not STW), so VisitMutatorRoots is not safe here.
//
// Compile-time gate, product ON for this lane (observe-only). No new
// MRT_GCV2_* env. Atexit keep vs sampled so sampled=0 cannot mean dead
// probe when keep>0.
namespace CsetEmptyWho {

void BeginCycle();
void NoteKeep(RegionInfo* region, size_t residual, size_t residualFwd, size_t marked);
void Report(const char* tag);

} // namespace CsetEmptyWho
} // namespace MapleRuntime

#endif // MRT_CSET_EMPTY_WHO_H
