// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_O0HOLE_DIAG_H
#define MRT_O0HOLE_DIAG_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// o0hole: quantify ForwardRegion inline promote remset path (甲) and full remset
// rebuild-gate (乙). Join skipped promote slots to idleedge neverSeen.
//
// Gate (default off; product path early-return before counters):
//   MRT_GCV2_O0HOLE=1  OR  MRT_GCV2_DIAG contains o0hole|all
// Sample cap: MRT_GCV2_O0HOLE_SAMPLES=<N> (default 8)

namespace O0HoleDiag {

bool Enabled();

// 甲: ForwardRegion visit / field walk.
// enterRegion: young region that reached VisitLiveObjects remset path (Route ok).
// enterObj: young toObj with HasRefField under that walk.
// fieldSeen: every ref field of those objects.
// recYoung: target young → RememberedSet::Record
// skipNull: target null/nonheap
// skipOld: target region non-young (or null region)
// skipNoTo / skipNoRef: object-level early outs before ForEachRefField
void NoteFwdEnterRegion(RegionInfo* region);
void NoteFwdEnterObj(BaseObject* toObj);
void NoteFwdField(MAddress fieldAddress, BaseObject* toObj, BaseObject* target, bool recorded,
                  uint8_t skipKind);
// skipKind: 0=recorded 1=null/nonheap 2=old/non-young 3=no-to 4=no-ref

// Stamp a slot that promote-inline refused to put into remset (null or old target).
// Used for neverSeen ∩ skip join (promotefill method).
void NoteFwdSkipSlot(MAddress fieldAddress, uint8_t skipKind);

// 乙: rebuild-gate at young.evac_finish.
// youngBeforeResidual / youngAfterResidual: GetYoungRegionCount snapshots.
// residualDemoteN: regions demoted in residual loop.
// gateSkip: youngAfter==0 → skip rebuild.
// virtualWouldRecord: if gate forced open, how many old→young edges rebuild would Record.
// virtualWouldMissRemset: of those, how many are NOT already in remset (true safety-net value).
void NoteRebuildGate(size_t youngBeforeResidual, size_t youngAfterResidual, size_t residualDemoteN,
                     size_t residualPromoteRecords, size_t promoteReplay, size_t rebuiltRecords,
                     size_t virtualWouldRecord, size_t virtualWouldMissRemset, bool gateSkip);

// Census join: neverSeen miss previously stamped as promote-inline skip.
bool NoteCensusNeverSeen(MAddress fieldAddress);

void DumpProcessTotals(const char* tag);

} // namespace O0HoleDiag
} // namespace MapleRuntime

#endif // MRT_O0HOLE_DIAG_H
