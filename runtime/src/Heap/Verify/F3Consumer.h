// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_F3_CONSUMER_H
#define MRT_HEAP_F3_CONSUMER_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {

// F3 consumer ledger (default off): MRT_GCV2_F3_CONSUMER=1
// Records major/minor mark vs field-scan vs edge-follow for holders that later
// appear in TryUntagRefField F3_DEATH dumps.
// Positive control bucket-2 (default off): MRT_GCV2_F3C_POSCTRL=1
//   Intentionally skips following one holder→target edge after mark so the
//   apparatus must classify the natural F3 as bucket-2 (marked holder, dead target).
// Positive control bucket-1 (default off): MRT_GCV2_F3M_POSCTRL=1
//   Intentionally skips MarkObject once (returns wasMarked without setting bit)
//   so holder stays unmarked while a child target can die → bucket-1 F3.
//   ⚠ Does NOT enable the F3_CONSUMER hot-path ledger (no NoteMark mutex thrash).
class F3Consumer {
public:
    static bool Enabled();
    static bool PosCtrlEnabled();
    // Bucket-1 positive control only (no ledger hot path).
    static bool MarkPosCtrlEnabled();

    // Called when MarkObject first claims obj (wasMarked==false).
    static void NoteMark(void* obj, const char* site, unsigned int young, unsigned int major);

    // Called on entry to TraceObjectRefFields / young ForEachRefField scan.
    static void NoteScan(void* obj, const char* site, unsigned int young, unsigned int major);

    // Called when a ref edge is actually pushed onto the work stack.
    static void NoteEdgeFollow(void* holder, void* field, void* target, const char* site);

    // Positive control: if returns true, caller must NOT follow this edge.
    // Fires at most once per process when POSCTRL=1 and holder is freshly marked.
    static bool ShouldSkipEdge(void* holder, void* field, void* target, const char* site);

    // Bucket-1 positive control: if returns true, MarkObject must leave the mark
    // bitmap untouched and return wasMarked=true (skip scan). Fires once / process.
    static bool ShouldSkipMark(void* obj, unsigned int major);

    // At F3 abort: dump ledger row for holder/field/target into VLOG and fill
    // a one-line verdict (bucket a/b/c sketch) into verdictBuf.
    static void DumpAtAbort(void* holder, void* field, void* target, char* verdictBuf, size_t verdictBufSize);

    static void NotePosCtrlFired(void* holder, void* field, void* target);

    // True if F3M_POSCTRL fired and holder matches the skipped object.
    static bool MarkPosCtrlMatch(void* holder);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_F3_CONSUMER_H
