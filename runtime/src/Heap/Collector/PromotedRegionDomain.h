// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_PROMOTED_REGION_DOMAIN_H
#define MRT_PROMOTED_REGION_DOMAIN_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Common/TypeDef.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class RegionInfo;
class BaseObject;
class WCollector;

// promodomain (ZGC_CONVERGENCE_PLAN.md §A.3): durable flip-promote registry +
// restartable discharge task (ZGC zRelocationSet._flip_promoted_pages +
// ZRelocateAddRemsetForFlipPromoted / remap_and_maybe_add_remset).
//
// Young collection uses this domain as the sole flip-promoted field walk.  This
// matches ZRelocateAddRemsetForFlipPromoted (zRelocate.cpp:1257-1306): promotion
// registers the page and relocation later remaps its fields.  The synchronous
// RecordPromotedCrossGenEdges walk remains only for non-young collections, which
// have no young relocate discharge phase.
//
// Gates:
//   MRT_GCV2_PROMO_DOMAIN_RECONCILE=1    — dual-run bidirectional set diff
//   MRT_GCV2_PROMO_DOMAIN_SKIP_ONE=1     — positive: skip first domain edge
//   MRT_GCV2_PROMO_DOMAIN_INJECT_UNDISCHARGED=1 — positive: leave one undischarged
//   MRT_GCV2_PROMO_DOMAIN_FATAL=1        — CHECK on reconcile mismatch / undischarged reuse
//   MRT_GCV2_PROMO_DOMAIN_FORCE_INPLACE=1 — force ForwardRegion in-place arm (dual-run only)
//
// Lifecycle:
//   Register at in-place / abandon / residual promote sites (Promote still STW3 / copy).
//   DischargeAll after STW3 release, still in FORWARD, before IDLE
//   (ZRelocateAddRemsetForFlipPromoted, zRelocate.cpp:1257-1306).
//   ResetForNextMinor at next minor start — CHECK registered==discharged first.

namespace PromotedRegionDomain {

// ZGC registers flip-promoted pages from an already selected relocation set;
// that set has a closed livemap (zGeneration.cpp:211-221;
// zRelocate.cpp:1289-1306).  A residual page without authoritative liveness
// must remain young: scanning all of it after relocation can discover a field
// whose target was never admitted to the relocation set.
inline bool ResidualPromotionHasClosedLiveness(bool hasMarkStartAllocGap, bool liveCountAuthoritative,
                                               bool hasObjectLiveness)
{
    return !hasMarkStartAllocGap && liveCountAuthoritative && hasObjectLiveness;
}

// ZRelocate::relocate defers the field walk for young flip-promoted pages until
// after relocation (zRelocate.cpp:1289-1306).  Other collection modes have no
// corresponding discharge phase and retain their synchronous scan.
inline bool DeferPromotedFieldScan(bool isYoungCollection)
{
    return isYoungCollection;
}

enum class RegisterPath : uint8_t {
    InPlace = 0,
    Abandon = 1,
    Residual = 2,
};

bool Enabled();
bool ReconcileEnabled();
bool FatalOnMismatch();

// zRelocationSet::register_flip_promoted shape: lock + append, reject duplicate.
void Register(RegionInfo* region, RegisterPath path);

// True if region is registered and not yet discharged (must not TakeRegion/ClearUnits).
bool IsRegisteredUndischarged(const RegionInfo* region);

// Obligation ①: call before reuse / ClearUnits. CHECK if undischarged (when enabled).
void CheckNotUndischargedForReuse(const RegionInfo* region, const char* site);

// resolve + recordSlot from WCollector (RememberedSet::Record is not public here).
// resolve must be ResolveMinorReference: CAS self-heal, same shape as
// zRelocate.cpp:1242 load_barrier_on_oop_field_preloaded.
// No store-good early-exit: ColourStoreGood never Records (WCollector.h:702-730).
// No colour store here: resolve already CAS-installs (unconditional StoreColoured
// would lose a mutator write once this walk is off-STW).
size_t DischargeAll(const std::function<BaseObject*(RefField<>&)>& resolve,
                    const std::function<void(MAddress)>& recordSlot);

// Dual-run: old RecordPromotedCrossGenEdges product edge.
void NoteOldProductRecord(MAddress slot);

// Coverage buckets (domainon): count Record vs Register by GC reason.
// reason < GC_REASON_MAX; site: 0=inplace 1=abandon 2=residual 3=other
void NoteRecordCall(uint32_t reason, uint8_t site, size_t edges);
void NoteRegisterGate(uint32_t reason, uint8_t site, bool registered);

// Next minor start: CHECK all discharged (unless inject), then clear table.
void ResetForNextMinor(size_t minorRunIndex);

void DumpReconcile(size_t minorRunIndex, const char* tag);
void DumpProcessTotals(const char* tag);
void DumpCoverageByReason(const char* tag);

size_t RegisteredCount();
size_t DischargedCount();
size_t LastDischargeNs();
size_t TableBytesEstimate();
size_t LastOldEdgeCount();
size_t LastDomainEdgeCount();
size_t LastOldOnlyCount();
size_t LastDomainOnlyCount();

} // namespace PromotedRegionDomain
} // namespace MapleRuntime

#endif // MRT_PROMOTED_REGION_DOMAIN_H
