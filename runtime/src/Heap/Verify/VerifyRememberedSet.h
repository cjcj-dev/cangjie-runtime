// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

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
