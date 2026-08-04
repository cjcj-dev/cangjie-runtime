// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_ROUTE_ACCOUNT_PROBE_H
#define MRT_HEAP_ROUTE_ACCOUNT_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

class RegionInfo;

// Read-only dual-ledger probe for H1 (reserve uses liveByteCount, route uses bitmap popcount).
// Gate (default off): MRT_GCV2_ROUTE_ACCOUNT=1
// Optional abort on imbalance: MRT_GCV2_ROUTE_ACCOUNT_FATAL=1
// Optional null-liveInfo/markBitmap path counter around MarkObject CHECK:
//   MRT_GCV2_MARK_BITMAP_NULL=1 (default off)
class RouteAccountProbe {
public:
    static bool AccountEnabled();
    static bool MarkBitmapNullEnabled();

    // GetRoute else branch: preLiveBytes vs toRegion1UsedBytes (and from-region ledgers if provided).
    static void NoteGetRouteElse(uint64_t preLiveBytes, uint32_t toRegion1UsedBytes, RegionInfo* fromRegion);

    // After RouteOrCompactRegionImpl finishes a route: counter reserve vs bitmap total.
    static void NoteRouteReserve(RegionInfo* fromRegion, size_t fromBytesCounter, size_t usedBytes1, size_t usedBytes2,
                                 bool twoRegion);

    // Before CHECK(IsMarkedObject): count null liveInfo / null markBitmap / TEMPORARY races.
    // Returns true if IsMarkedObject would see a usable markBitmap.
    static bool NoteMarkBitmapCheck(RegionInfo* region, size_t offset, const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_ROUTE_ACCOUNT_PROBE_H
