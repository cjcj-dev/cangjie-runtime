// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_WALK_ALIGN_PROBE_H
#define MRT_WALK_ALIGN_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

class RegionInfo;
class BaseObject;

// Read-only first-bad-step probe for VisitAllObjects size-walk misalignment.
// Gate (default off): MRT_GCV2_WALK_ALIGN=1
// Optional: MRT_GCV2_WALK_ALIGN_MAX_DUMPS=<N>  (default 1; first dump is the critical one)
//
// Before RegionSpace::GetAllocSize is used to advance the cursor, check that the
// object header looks like a real object (TypeInfo region, size non-zero, does
// not overrun region allocPtr, alignment). On first failure dump predecessor +
// next words, then return and let the original path run (and crash as before).
//
// Does not modify IsValidObject / IsVaildType / IsMarked.
struct WalkAlignProbe {
    static bool Enabled();

    // Returns true if the object at `position` looks walkable; always dumps at
    // most once per process (first bad step) unless MAX_DUMPS > 1.
    // `prev` may be null on the first object of a region.
    static bool CheckBeforeSize(RegionInfo* region, uintptr_t position, uintptr_t allocPtr, size_t stepInRegion,
                                BaseObject* prev, size_t prevSize, size_t& totalStepsOut);

    static size_t TotalSteps();
    static size_t BadSteps();
};

} // namespace MapleRuntime

#endif // MRT_WALK_ALIGN_PROBE_H
