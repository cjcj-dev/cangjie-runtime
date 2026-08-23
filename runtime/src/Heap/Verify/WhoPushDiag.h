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
#ifndef MRT_WHO_PUSH_DIAG_H
#define MRT_WHO_PUSH_DIAG_H

#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// whopush: who pushed a non-start address onto the young work stack.
// Gate: MRT_GCV2_WHOPUSH=1 or MRT_GCV2_DIAG token whopush. Default off.
namespace WhoPushDiag {

bool Enabled();

void NotePush(BaseObject* object, const char* site, const void* slot = nullptr, BaseObject* holder = nullptr);

void NoteCrashRdi(uintptr_t rdi);

void Report(const char* point);

} // namespace WhoPushDiag
} // namespace MapleRuntime

#endif // MRT_WHO_PUSH_DIAG_H
