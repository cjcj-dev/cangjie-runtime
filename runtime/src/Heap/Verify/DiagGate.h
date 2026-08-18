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
#ifndef MRT_DIAG_GATE_H
#define MRT_DIAG_GATE_H

// Unified diagnostic gate for GC instruments.
//
// Master switch (optional CSV of tokens, case-sensitive, comma/space separated):
//   MRT_GCV2_DIAG=idleedge,promote,fullclear,nullslot,selftest
//   MRT_GCV2_DIAG=all
//
// Legacy per-probe envs remain authoritative aliases (in-flight recipes must keep working):
//   MRT_GCV2_IDLEEDGE=1
//   MRT_GCV2_IDLEEDGE_STAMP_BITS=<16..22>
//   MRT_GCV2_PROMOTEGAP_PROBE=1
//   MRT_GCV2_FULLCLEAR_PROBE=1
//   MRT_GCV2_NULLSLOT=1
//   MRT_GCV2_IDLEEDGE_SELFTEST=1  /  MRT_GCV2_DIAG_SELFTEST=1
//
// Discovery:
//   MRT_GCV2_DIAG_HELP=1    — print registered tokens + legacy aliases once
//   MRT_GCV2_DIAG_ACTIVE=1  — print which probes this process has enabled
//
// Rule: product path must early-return before any counter when both master and
// legacy gates are off (zero extra cost).

namespace MapleRuntime {
namespace DiagGate {

// True if MRT_GCV2_DIAG contains token, or equals "all"/"1".
bool TokenOn(const char* token);

// True if legacy env is "1" OR TokenOn(token).
bool LegacyOrToken(const char* legacyEnv, const char* token);

// True if self-test requested (legacy or token "selftest" / DIAG_SELFTEST).
bool SelfTestOn();

// Emit HELP / ACTIVE banners once if those envs are set. Safe to call often.
void MaybeAnnounce();

// Emit one-shot counter health legend (meanings / healthy values / false high-low).
// Loud enough for recipe logs (RTLOG_ERROR). Idempotent.
void EmitCounterLegend();

} // namespace DiagGate
} // namespace MapleRuntime

#endif // MRT_DIAG_GATE_H
