// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STW2_CURRENT_AUDIT_H
#define MRT_STW2_CURRENT_AUDIT_H

#include <cstddef>
#include <cstdint>
#include <unordered_set>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class BaseObject;
class Allocator;

// STW2 current-face drain auditor (youngconcstw2 / ZGC M5).
//
// ZGC pause_mark_end (zGeneration.cpp:897-916) does not drain the remset current
// face. Concurrent stores land on current and are consumed next cycle after flip
// (zRemembered.cpp:561-576 scans previous only). Correctness of not draining this
// cycle is: a young target of a concurrent old→young store is already live by
// allocate-black / mark-start watermark / SATB (see REPORT-youngconcstw2.md §0).
//
// This auditor classifies STW2 DrainForMinor's current-face slots WITHOUT feeding
// the mark closure. uncovered==0 is the gate to retire the drain-into-workStack
// path. Compile-time gate, no new MRT_GCV2_* env (campaign cut those from 190 to 3).
//
// Cover classes (first match wins):
//   water      — RegionInfo::AllocatedAfterMarkStart (zPage is_allocating)
//   allocblack — TRACE-window PushYoungAllocBlack ledger (RegionSpace.cpp:347)
//   marked     — Young IsMarkedObject (bitmap / watermark)
//   satb       — SATB retired + in-flight mutator node
//   skip       — null / non-heap / not-young / not a managed object
//   uncovered  — young target none of the four cover
//
// Positive control: ArmInject() forces uncovered+=1 so a zero cannot mean dead probe.
namespace Stw2CurrentAudit {

enum class Stw2Cover : uint8_t {
    Skip = 0,
    Water = 1,
    AllocBlack = 2,
    Marked = 3,
    Satb = 4,
    Uncovered = 5,
};

constexpr bool kStw2CurrentAudit = true;

bool Enabled();

// Force the next Census to count one synthetic uncovered (gc_unit / perturbation).
void ArmInject();

// Classify one young-or-not target. First match: water, alloc-black, marked, satb.
Stw2Cover ClassifyTarget(BaseObject* target, const std::unordered_set<BaseObject*>& allocBlack,
                         const std::unordered_set<BaseObject*>& satb);

// Observe-only. Call after DrainForMinor of the concurrent current face, before
// MergeYoungAllocBlack / GetRetiredObjects consume those ledgers.
// allocator may be null (gc_unit / inject-only); alloc-black peek is then empty.
void Census(const std::unordered_set<MAddress>& currentSlots, Allocator* allocator);

void Report(const char* tag);

size_t Uncovered();
size_t Water();
size_t AllocBlack();
size_t Marked();
size_t Satb();
size_t Skip();
size_t Slots();
size_t Minors();

} // namespace Stw2CurrentAudit
} // namespace MapleRuntime

#endif // MRT_STW2_CURRENT_AUDIT_H
