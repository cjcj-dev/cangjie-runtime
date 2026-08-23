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
