// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_F3WHY2_DIAG_H
#define MRT_F3WHY2_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// f3why2: why does CollectRegion type a region GARBAGE, and do those regions
// intersect F3 dead-arm region_garbage hits?
//
// Always-on counters (enter + class + sample). Join table is also always-on
// with a large cap so join is exact or explicitly saturated (lower bound).
//
// Lines:
//   [GCV2][f3why2][collect-enter] ... sample of first N CollectRegion
//   [GCV2][f3why2][f3-join] ... sample of F3 region_garbage hits with join bit
//   [GCV2][f3why2] point=atexit enter=... classes=... join=... sat=...

namespace F3Why2Diag {

// Call at the top of RegionManager::CollectRegion (before type→GARBAGE).
void NoteCollectEnter(RegionInfo* region);

// Call when F3 dead arm classifies reason=region_garbage (latestRegion known).
// Records join against the CollectRegion region-set of this process.
void NoteF3RegionGarbage(RegionInfo* latestRegion, BaseObject* latest);

void Report(const char* point);

} // namespace F3Why2Diag
} // namespace MapleRuntime

#endif // MRT_F3WHY2_DIAG_H
