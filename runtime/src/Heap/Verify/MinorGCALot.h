// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

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
