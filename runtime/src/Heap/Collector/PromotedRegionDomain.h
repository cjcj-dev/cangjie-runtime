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
// Default OFF. Product still runs RecordPromotedCrossGenEdges until a later lane
// deletes it. Dual-run reconcile proves edge-set equivalence first.
//
// Gates:
//   MRT_GCV2_PROMO_DOMAIN=1              — register + discharge task
//   MRT_GCV2_PROMO_DOMAIN_RECONCILE=1    — dual-run bidirectional set diff
//   MRT_GCV2_PROMO_DOMAIN_SKIP_ONE=1     — positive: skip first domain edge
//   MRT_GCV2_PROMO_DOMAIN_INJECT_UNDISCHARGED=1 — positive: leave one undischarged
//   MRT_GCV2_PROMO_DOMAIN_FATAL=1        — CHECK on reconcile mismatch / undischarged reuse
//
// Lifecycle:
//   Register at in-place / abandon / residual promote sites (with old scan still on).
//   DischargeAll at young.evac_finish (v1 STW; same window as today's sync walk).
//   ResetForNextMinor at next minor start — CHECK registered==discharged first.

namespace PromotedRegionDomain {

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

// resolve / isStoreGood / colorStoreGood / recordSlot from WCollector call site
// (RememberedSet::Record is private to Barrier/WCollector/RegionManager).
// Visitor = ZGC remap_and_maybe_add_remset: store-good early-exit; else resolve;
// target still young ⇒ Record + store-good colour.
size_t DischargeAll(const std::function<BaseObject*(RefField<>&)>& resolve,
                    const std::function<bool(RefField<>&)>& isStoreGood,
                    const std::function<void(RefField<>&, BaseObject*)>& colorStoreGood,
                    const std::function<void(MAddress)>& recordSlot);

// Dual-run: old RecordPromotedCrossGenEdges product edge.
void NoteOldProductRecord(MAddress slot);

// Next minor start: CHECK all discharged (unless inject), then clear table.
void ResetForNextMinor(size_t minorRunIndex);

void DumpReconcile(size_t minorRunIndex, const char* tag);
void DumpProcessTotals(const char* tag);

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
