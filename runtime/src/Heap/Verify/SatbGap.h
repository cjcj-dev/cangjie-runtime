// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_SATB_GAP_H
#define MRT_HEAP_SATB_GAP_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

class BaseObject;

// B-3 SATB gap forensics (default off):
//   MRT_GCV2_SATB_GAP=1         mark holders scanned; log post-scan ref writes
//   MRT_GCV2_SATB_POSCTRL=1     once: suppress SATB for a post-scan new-edge write
// Does not widen IsValid*/IsMarked/TryUntag; evidence only. field/target as void*.
class SatbGap {
public:
    static bool Enabled();
    static bool PosCtrlEnabled();

    // After TraceObjectRefFields finishes walking holder (major TRACE path).
    static void NoteScanDone(void* holder);

    // True if holder was NoteScanDone this process (best-effort set).
    static bool IsScanned(void* holder);

    // Mutator ref write during ENUM/TRACE/CLEAR_SATB.
    // satbLoggedNew=1 if RememberNewReference / RememberObjectInSatbBuffer(new) ran.
    // Returns true if POSCTRL asks caller to SKIP the SATB enqueue for newRef.
    static bool NoteWrite(void* holder, void* field, void* oldRef, void* newRef, int satbLoggedNew,
                          const char* site);

    // barestore T2: value observed at TraceRefField when walking a slot (default off via SATB_GAP).
    // pushed=1 if workStack.push_back ran for this target.
    static void NoteTraceSlot(void* holder, void* field, void* value, int pushed, const char* site);

    // POSCTRL: should suppress SATB for this post-scan write of newRef? (once).
    static bool ShouldSkipSatbNew(void* holder, void* field, void* newRef, const char* site);

    // At F3 abort: correlate holder/target with post-scan write ledger.
    static void DumpAtAbort(void* holder, void* field, void* target, char* verdictBuf, size_t verdictBufSize);

    static bool PosCtrlMatch(void* holder, void* target);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_SATB_GAP_H
