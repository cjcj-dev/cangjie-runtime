// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_BITMAP_INTERSECT_PROBE_H
#define MRT_BITMAP_INTERSECT_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

class RegionManager;

// E4: read-only stats for markBitmap ∧ resurrectBitmap granule overlap (H1b).
// Gate (default off): MRT_GCV2_BITMAP_INTERSECT=1
// Call after DoResurrection while both bitmaps are still live on regions.
struct BitmapIntersectProbe {
    static bool Enabled();

    // Walk all non-free regions; count granules where both bitmaps mark the same 8B unit.
    // Logs one SUMMARY line; returns intersectGranules.
    static size_t ScanAfterResurrection(RegionManager& manager, size_t resurrectedObjectsReported);
};

} // namespace MapleRuntime

#endif // MRT_BITMAP_INTERSECT_PROBE_H
