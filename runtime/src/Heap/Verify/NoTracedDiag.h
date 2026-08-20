// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Restored 2026-08-20 (markport). Was HOLLOWED: Enabled() returned false and every
// sink was empty, so the one question it exists to answer produced no output at all.
//
// notraced: was TraceObjectRefFields ever called for a holder?
//
// MarkCompleteVerify finds live holders whose ref fields name unmarked objects. Three
// things can produce that, and they need different fixes:
//   1. the holder's live bit was painted by a path that does not enqueue it, so its
//      fields were never scanned  (traced == 0)
//   2. the fields were scanned but TraceRefField did not mark those targets
//      (traced > 0)
//   3. the element was stored after the holder was scanned and the insertion barrier's
//      record was dropped  (traced > 0)
// The traced count splits 1 from 2/3, which is the split no amount of reading the
// mark code settles.
//
// Protocol: a dead edge in cycle N registers its holder with Watch(); NoteTrace()
// counts hits from cycle N+1 onward. That works because the edges observed so far
// repeat across cycles with the same addresses. A holder that is never traced again
// while its dead edge persists is case 1.
//
// Table is fixed-size and direct-mapped: one load and one compare per traced object,
// no allocation on the mark path. Gated with MarkCompleteVerify so the two reports
// arm together; costs nothing when that gate is off.

#ifndef MRT_NO_TRACED_DIAG_H
#define MRT_NO_TRACED_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

namespace NoTracedDiag {

bool Enabled();

// Register a holder to watch. Idempotent; silently drops on table collision, which
// Report() states rather than hides.
void Watch(const BaseObject* obj);

// TraceObjectRefFields entry (hot path when on: one direct-mapped probe).
void NoteTrace(BaseObject* obj);

// Object copy: keep the watch list addressed correctly when a holder moves.
void NoteCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done);

// Crash join: match holder identity against the watch list.
void NoteCrashJoin(uintptr_t holderCrash, uintptr_t holderCas);

void Report(const char* point);

} // namespace NoTracedDiag
} // namespace MapleRuntime

#endif
