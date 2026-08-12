// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_SEAL_CHECK_H
#define MRT_SEAL_CHECK_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// sealcheck: after a region's mark face is sealed for geometry, any further paint is a
// contract violation (M3 / S′ write side). GetRoute geometry is a prefix-sum over the
// sealed face — late paint shifts to-addresses for all later objects in the region.
//
// Gate (default off; product path early-return BEFORE any work):
//   MRT_GCV2_VERIFY_SEALCHECK=1
// Positive control (after seal, trip NotePaint once per process; does not
// mutate the product mark face):
//   MRT_GCV2_VERIFY_SEALCHECK_INJECT=1
// Optional abort on late paint:
//   MRT_GCV2_VERIFY_SEALCHECK_FATAL=1
// Sample cap on detail lines: MRT_GCV2_VERIFY_SEALCHECK_MAX=<N> (default 64)
//
// Seal point (product): RegionManager::RouteRegion after TryLockRouting succeeds
// (FORWARDABLE→ROUTING), before RouteOrCompactRegionImpl reads liveByteCount.
// Flag lives on RegionInfo (markFaceSealed).
//
// Summary line: SEALCHECK sealed_regions=N late_paint=M

namespace SealCheck {

bool Enabled();

// Called once per region when its mark face freezes for geometry.
void NoteSeal(RegionInfo* region);

// Called at every mark-face paint entry when gate is on.
// site: stable C string naming the paint call site.
void NotePaint(RegionInfo* region, size_t offset, size_t byteCnt, const char* site);

// Optional positive control: if inject env is set and region just sealed, paint once.
void MaybeInjectLatePaint(RegionInfo* region);

void DumpSummary();

} // namespace SealCheck
} // namespace MapleRuntime

#endif // MRT_SEAL_CHECK_H
