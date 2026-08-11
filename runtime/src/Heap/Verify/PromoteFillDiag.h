// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_PROMOTE_FILL_DIAG_H
#define MRT_PROMOTE_FILL_DIAG_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// promotefill: quantify RecordPromotedCrossGenEdges early-returns that depend on
// mark/survivor ledger (IsSafeKnownEmpty / useLiveOnly&&!survived), capture
// liveInfo pointer identity vs mark-time, and join skipped slots to idleedge
// neverSeen misses.
//
// Gate (default off; product path early-return before counters):
//   MRT_GCV2_PROMOTEFILL=1  OR  MRT_GCV2_DIAG contains promotefill|all
// Sample cap: MRT_GCV2_PROMOTEFILL_SAMPLES=<N> (default 8)
//
// When gated off every entry is a no-op (gates-off equivalence).

namespace PromoteFillDiag {

bool Enabled();

// Mark-period: first paint of a region records metadata.liveInfo pointer.
// Call from WCollector::MarkObject after region mark succeeds (was unmarked).
void NoteMarkLiveInfo(RegionInfo* region, void* liveInfoPtr);

// Early-return 1: whole region skipped by IsSafeKnownEmpty.
// liveInfoAtPromote / liveInfo0AtPromote: raw pointers at the early-return site.
void NoteSafeKnownEmpty(RegionInfo* region, void* liveInfoAtPromote, void* liveInfo0AtPromote,
                        uint64_t liveByteRaw, unsigned hasBitmap, unsigned isLarge,
                        size_t regionBytes);

// Early-return 2: per-object skip under useLiveOnly && !survived.
// For each young-target ref field of the skipped object, also NoteSkipSlot.
void NoteSkipDeadObject(RegionInfo* region, BaseObject* object, void* liveInfoAtPromote,
                        void* liveInfo0AtPromote, size_t offset, unsigned survivedBit,
                        unsigned useLiveOnly, unsigned hasObjectLiveness);

// Record a slot that promote-fill refused to put into remset (dead-object skip arm).
void NoteSkipSlot(MAddress fieldAddress, RegionInfo* holderRegion, BaseObject* holder);

// Census join: if a neverSeen miss was previously NoteSkipSlot'd, count causal.
// Returns true when the miss joins a promote-skip stamp.
bool NoteCensusNeverSeen(MAddress fieldAddress);

// Process totals (also printed on demand).
void DumpProcessTotals(const char* tag);

} // namespace PromoteFillDiag
} // namespace MapleRuntime

#endif // MRT_PROMOTE_FILL_DIAG_H
