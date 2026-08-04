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

// Read-only first-bad-step probe for heap size-walks.
// Gate (default off): MRT_GCV2_WALK_ALIGN=1
// Optional: MRT_GCV2_WALK_ALIGN_MAX_DUMPS=<N>  (default 1; first dump is the critical one)
//
// Before GetAllocSize/GetSize is used to advance a size-walk cursor, check that the
// object header looks like a real object. On first failure dump predecessor +
// next words, then return and let the original path run (and crash as before).
//
// holewide: each size-walk entry passes a stable tag string; ARMED is emitted
// once per tag so "entry never called" vs "called but no FIRST_BAD" is separable.
//
// Does not modify IsValidObject / IsVaildType / IsMarked.
struct WalkAlignProbe {
    // Stable entry tags (string literals; compared by pointer identity in ARMED map).
    static constexpr const char* TAG_VISIT_ALL = "VisitAll";
    static constexpr const char* TAG_VISIT_LIVE = "VisitLive";
    static constexpr const char* TAG_COMPACT = "Compact";
    static constexpr const char* TAG_COMPACT_PARTIAL = "CompactPartial";
    static constexpr const char* TAG_COMPACT_PARTIAL_TAIL = "CompactPartialTail";

    static bool Enabled();

    // Returns true if the object at `position` looks walkable; dumps at most
    // MAX_DUMPS times process-wide. `prev` may be null on the first object.
    // `entryTag` appears in ARMED / FIRST_BAD lines (use TAG_* constants).
    static bool CheckBeforeSize(RegionInfo* region, uintptr_t position, uintptr_t allocPtr, size_t stepInRegion,
                                BaseObject* prev, size_t prevSize, size_t& totalStepsOut,
                                const char* entryTag = TAG_VISIT_ALL);

    static size_t TotalSteps();
    static size_t BadSteps();
};

} // namespace MapleRuntime

#endif // MRT_WALK_ALIGN_PROBE_H
