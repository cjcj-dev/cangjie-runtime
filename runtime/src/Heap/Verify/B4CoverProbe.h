// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_B4_COVER_PROBE_H
#define MRT_HEAP_B4_COVER_PROBE_H

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "Common/TypeDef.h"

namespace MapleRuntime {

// B-4 coverage: did fix-set enumerate the slots that later show stale interiors?
// Gate (default off): MRT_GCV2_B4COVER=1
// Dump cap: MRT_GCV2_B4COVER_DUMP_MAX=<N> (default 96)
//
// Verdicts per interior holder-slot at stw-enter (vs prior minor fix ledger):
//   B4CV_IN_REMSET_NOT_FIXED  — slot ∈ remset drain, Fix visited, value not CAS-changed
//   B4CV_IN_REMSET_FIXED      — slot ∈ remset, Fix changed (should not reappear as stale)
//   B4CV_IN_REACH_NOT_FIXED   — slot ∈ reachable holder fields, Fix visited, not changed
//   B4CV_IN_REACH_FIXED       — reachable path fixed
//   B4CV_NOT_IN_FIXSET        — neither remset nor reachable field set (true coverage hole)
//   B4CV_NOT_IN_REMSET        — alias of NOT_IN_FIXSET when also not remset (record-side)
class B4CoverProbe {
public:
    static bool Enabled();

    // After DrainForMinor: snapshot remset slot addresses for this minor.
    static void NoteRemsetDrain(const std::unordered_set<MAddress>& slots);

    // After mark: snapshot every ref-field address of reachable objects (pre-fix).
    static void NoteReachableFieldSlots(const std::vector<BaseObject*>& reachableVec);

    // Inside FixMinorEvacuatedSlot: record visit + whether CAS installed a new value.
    // wasGhost: target was ghost-from at entry (needed forward).
    static void NoteFixVisit(MAddress slot, MAddress oldVal, MAddress newVal, bool casChanged, bool wasGhost,
                             bool wasOldTag);

    // STW enter: scan interiors and classify against prior minor's remset/reach/fix ledger.
    static void ScanInteriors(const char* point);

    // End of minor: rotate current ledger → prior (for next stw-enter classify).
    static void CommitMinorLedger(const char* site);

    static void FlushSummary(const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_B4_COVER_PROBE_H
