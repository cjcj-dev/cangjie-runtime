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
#ifndef MRT_TLRAW_DIAG_H
#define MRT_TLRAW_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// tlraw: uncommitted AllocBuffer tlRawPointerRegions / tlLargeRawPointerRegions
// at minor start, plus crash-rdi region class.
// Gate: MRT_GCV2_TLRAW=1 or MRT_GCV2_DIAG token tlraw. Default off.

namespace TlRawDiag {

bool Enabled();

void NoteMinorEnter(size_t minorRun);

void NoteInitRegion(RegionInfo* region);

void NoteCrashRdi(uintptr_t rdi);

void Report(const char* point);

} // namespace TlRawDiag
} // namespace MapleRuntime

#endif // MRT_TLRAW_DIAG_H
