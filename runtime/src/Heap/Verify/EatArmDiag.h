// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_EAT_ARM_DIAG_H
#define MRT_EAT_ARM_DIAG_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class BaseObject;

// eatarm: dynamic attribution of which mark/fixpoint early-return arm dropped
// an edge target T that later IOR's at GetRoute (liveInfo0 surv=0).
//
// Gate (default off; product path early-return BEFORE any counter/table work):
//   MRT_GCV2_EATARM=1  OR  MRT_GCV2_DIAG contains eatarm|all
// Stamp bits: MRT_GCV2_EATARM_STAMP_BITS=<16..22> (default 18)
// Sample cap on IOR reconcile lines: MRT_GCV2_EATARM_MAX_IOR=<N> (default 32)
// Self-test: MRT_GCV2_EATARM_SELFTEST=1
//
// Arms recorded (fixface §1.3 D5/D6/D8):
//   ARM_WAS_MARKED   — TraceYoungClosure young already-marked: continue without field scan
//   ARM_FYS_REMSET   — RescanRememberedSet fullYoungScan: slot ∉ reachableSlots → continue
//   ARM_FIXPOINT     — post-mark fixpoint ForEachRefField early return (reason code)
//   ARM_NONYOUNG_DEDUP — non-young holder already claimed under FYS: skip re-scan fields
//
// On GetRoute null (ROUTED/FORWARDABLE/ROUTING): look up T in stamp table and emit
// [GCV2][eatarm][IOR] with hit arm(s). Table cleared each minor (OnMinorBegin).
// No TLS. Fail-open when gate off.

namespace EatArmDiag {

enum class Arm : uint8_t {
    NONE = 0,
    WAS_MARKED = 1,
    FYS_REMSET = 2,
    FIXPOINT = 3,
    NONYOUNG_DEDUP = 4,
};

enum class FixpointReason : uint8_t {
    NONE = 0,
    TARGET_NULL = 1,
    TARGET_NONHEAP = 2,
    PLAUSIBLE_FAIL = 3,
    NOT_YOUNG = 4,
    ALREADY_MARKED = 5,
    ADMIT = 6,
};

bool Enabled();

// Call at start of each minor mark (after ClearLiveInfo). Clears stamp gen.
void OnMinorBegin(size_t minorRunIndex);

// Holder H already marked (young): fields not re-scanned. Record H as eater of future T.
void NoteWasMarkedSkipFields(BaseObject* holder);

// Non-young holder already in set under FYS: fields not re-scanned.
void NoteNonYoungDedupSkipFields(BaseObject* holder);

// FYS remset filter dropped slot; record resolved target if heap young.
void NoteFysRemsetSkip(MAddress slot, BaseObject* target);

// Fixpoint edge decision on target (only meaningful when gate on).
void NoteFixpointEdge(BaseObject* holder, BaseObject* target, FixpointReason reason);

// Fix path: set current host while FixMinorObjectSlots walks fields (TLS).
// No-op when gate off.
void SetFixHost(BaseObject* host);
BaseObject* GetFixHost();

// IOR site: GetRoute returned null for fromObj (edge target T). Reconcile vs stamps.
// host may be null if unknown (falls back to GetFixHost). fieldOff may be 0.
void NoteIorTarget(BaseObject* targetT, BaseObject* host, size_t fieldOff);

// Per-minor summary (counters + health). Call end of minor mark / after fixpoint.
void DumpMinorSummary(size_t minorRunIndex);

void RunSelfTest();

} // namespace EatArmDiag
} // namespace MapleRuntime

#endif // MRT_EAT_ARM_DIAG_H
