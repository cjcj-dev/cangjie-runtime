// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_IDLE_EDGE_DIAG_H
#define MRT_IDLE_EDGE_DIAG_H

#include "Common/TypeDef.h"
#include "Heap/Collector/Collector.h"

namespace MapleRuntime {
class BaseObject;

// idleedge: quantify old→young edges present on the heap at minor STW that are
// NOT in the mutator remset (pre-RecordPinnedCrossGenEdges). Those are the edges
// only census / FYS would recover — the Idle bare-store window plus other write
// gaps.
//
// Gate: MRT_GCV2_IDLEEDGE=1 (default off). No TLS.
// Cost control: MRT_GCV2_IDLEEDGE_EVERY=<N> (1-based invoke skip, default 1)
// Sample cap:   MRT_GCV2_IDLEEDGE_MAX_SAMPLES=<N> (default 8)
//
// When gated off every entry is a no-op (gates-off equivalence).

namespace IdleEdgeDiag {

bool Enabled();

// holderGen/targetGen: 0=unknown 1=young 2=old 3=nonheap (promoteedge).
// Called from Barrier::RecordCrossGenEdge when an edge is evaluated.
// Records write-time GC phase + gen for later miss attribution.
// Fail-open: no-op when gate off.
void NoteBarrierDecision(MAddress fieldAddress, GCPhase phase, bool recorded, uint8_t holderGen,
                         uint8_t targetGen);

// STW census: snapshot remset, walk all non-young holders, count old→young
// edges vs remset membership. Call immediately BEFORE RecordPinnedCrossGenEdges
// so census/FYS-only edges are still visible as remset misses.
void CensusPrePinnedStamp(size_t minorRunIndex);

// Process-level totals (also printed each census when enabled).
void DumpProcessTotals(const char* tag);

// fullclear: stamp promote-time target generation for a field slot.
// Gate: MRT_GCV2_FULLCLEAR_PROBE=1 (default off). Early-return before any counter.
// targetGen: 0=unknown 1=young 2=old 3=null/nonheap.
// recorded: whether promote path called RememberedSet::Record.
void NotePromoteTimeTarget(MAddress fieldAddress, uint8_t targetGen, bool recorded);

} // namespace IdleEdgeDiag
} // namespace MapleRuntime

#endif // MRT_IDLE_EDGE_DIAG_H
