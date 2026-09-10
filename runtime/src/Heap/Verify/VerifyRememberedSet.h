// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_VERIFY_REMEMBERED_SET_H
#define MRT_VERIFY_REMEMBERED_SET_H

#include <cstddef>
#include <unordered_set>
#include <vector>

#include "Common/TypeDef.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Barrier/RememberedSet.h"

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
// Gate: unified VerifyFace::Remembered (legacy MRT_GCV2_VERIFY_REMSET=1 is an alias).
// Report-only, every invocation,
// with the historical detailed-failure cap fixed at its default of 20.
//
// remsetSnapshot: non-owning view of remset slots at the verification point
// (typically the post-AcquireRecordsForMinor local set; live remset is empty then).
// rootReachableHolders: completed independent full-root closure, or nullptr when not measured.
void VerifyRememberedSetInvariant(const char* point, const std::unordered_set<MAddress>& remsetSnapshot,
                                  const std::unordered_set<BaseObject*>* rootReachableHolders = nullptr);

// Phase-local network. These checks share VerifyFace::Remembered with the bulk
// invariant above, but retain per-buffer/per-forwarding evidence instead of a
// whole-heap count.
void VerifyRememberedBeforeColorFlip();
void VerifyRememberedBeforeForwarding(const std::vector<RememberedSet::InPlaceSlot>& slots,
                                      MAddress fromBase, size_t size,
                                      const RememberedSet& rememberedSet);
void VerifyRememberedAfterForwarding(const std::vector<RememberedSet::InPlaceSlot>& slots,
                                     MAddress fromBase, MAddress toBase, size_t size,
                                     const ForwardingTable::Publication& publication);
#if defined(MRT_GC_UNIT_TESTS)
struct RememberedNetworkTestReceipt {
    size_t beforeColorFlip{ 0 };
    size_t afterScanComplete{ 0 };
    size_t beforeForwardingSlots{ 0 };
    size_t afterForwardingSlots{ 0 };
};
void ResetRememberedNetworkTestReceipt();
RememberedNetworkTestReceipt ReadRememberedNetworkTestReceipt();
void NoteRememberedAfterScanCompleteForTest();
using RememberedOldForwardHook = void (*)(void* manager, void* context);
void ArmRememberedOldForwardHookForTest(RememberedOldForwardHook hook, void* context);
void RunRememberedOldForwardHookForTest(void* manager);
#endif
} // namespace MapleRuntime

#endif // MRT_VERIFY_REMEMBERED_SET_H
