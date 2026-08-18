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
#ifndef MRT_VERIFY_REMEMBERED_SET_H
#define MRT_VERIFY_REMEMBERED_SET_H

#include <cstddef>
#include <unordered_set>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class BaseObject;

// Independent remset completeness check for invariant R:
//   ∀ root-reachable non-young holder o, ∀ ref field f of o:
//     region(*f) is young  ⇒  addr(&f) ∈ rememberedSet
//
// Enumerates holders via full heap region walk (ForEachObjUnsafe / VisitAllObjects).
// Does NOT reuse minor reachableObjects, TraceYoungClosure, or remset as the object
// enumeration source. MISSING_TOTAL inventories all allocated holders; MISSING and
// MISSING_ROOT_REACHABLE count the correctness-relevant root-reachable subset.
// Counts direct field edges only (no reachability cascade).
//
// Gate: MRT_GCV2_VERIFY_REMSET=1 (default off). Report-only by default.
// Optional abort: MRT_GCV2_VERIFY_REMSET_FATAL=1
// Optional start-at: MRT_GCV2_VERIFY_REMSET_START_AT=<N> (1-based invoke count)
// Optional every: MRT_GCV2_VERIFY_REMSET_EVERY=<N>
// Detailed failure cap: MRT_GCV2_VERIFY_REMSET_MAX_FAILURES=<N> (default 20)
//
// remsetSnapshot: non-owning view of remset slots at the verification point
// (typically the post-AcquireRecordsForMinor local set; live remset is empty then).
// force=true: run even when MRT_GCV2_VERIFY_REMSET is unset (post-evac hook uses this).
// rootReachableHolders: independent full-root closure, or null when unavailable.
void VerifyRememberedSetInvariant(const char* point, const std::unordered_set<MAddress>& remsetSnapshot,
                                  bool force = false,
                                  const std::unordered_set<BaseObject*>* rootReachableHolders = nullptr);
} // namespace MapleRuntime

#endif // MRT_VERIFY_REMEMBERED_SET_H
