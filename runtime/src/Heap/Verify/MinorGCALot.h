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
#ifndef MRT_MINOR_GC_ALOT_H
#define MRT_MINOR_GC_ALOT_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// MinorGCALot — HotSpot ScavengeALot intent (force young GC every N mutator allocs).
// Gate: MRT_GCV2_MINOR_GC_ALOT=<N>  (N>0 enables; default unset/0 = off)
// Trigger: AllocBuffer::Allocate after a successful mutator allocation (RegionSpace.cpp).
// Why safe: same mutator-allocation RequestGC(YOUNG) surface as TakeRegion heuristic
// (RegionManager.cpp); async enqueue; never on GC thread; no locks held around the call.
class MinorGCALot {
public:
    // N from env; 0 = disabled. Read once at first call.
    static size_t Interval();
    static bool Enabled();

    // Count one successful mutator allocation; may RequestGC(GC_REASON_YOUNG, true).
    static void AfterSuccessfulAlloc(size_t allocBytes);

    static size_t TriggerCount();
    static size_t AllocCount();
};

} // namespace MapleRuntime

#endif // MRT_MINOR_GC_ALOT_H
