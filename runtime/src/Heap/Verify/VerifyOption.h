// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_VERIFY_OPTION_H
#define MRT_VERIFY_OPTION_H

#include <cstdint>

namespace MapleRuntime {

// Anchor: HotSpot gc/shared/verifyOption.hpp:30-39
// Purpose: make the mark-information source used by a verifier *explicit*, so the
// verifier is not tautologically true against the same data it is checking.
//
// Our mark-information inventory (see REPORT-gcvheap MARK_SOURCES):
//   MinorClosure       — TraceYoungClosure reachableObjects
//   RegionMarkBitmap   — RegionInfo::IsMarkedObject bits written during trace
//   IndependentRetrace — verifier-local recompute from roots (ValidateYoungMarking body)
//   RememberedSet      — slot set (edge bookkeeping, not object marks)
//   FullHeapEnum       — Heap::ForEachObj (enumeration, not reachability)
//
// Env override: MRT_GCV2_VERIFY_MARK_SOURCE=
//   independent | minor-closure | region-bitmap | default
enum class VerifyMarkSource : uint8_t {
    // Default for young-marking: compare IndependentRetrace vs RegionMarkBitmap
    // (does NOT consult MinorClosure — avoids fullYoungScan tautology).
    IndependentVsBitmap = 0,
    // Old / trivial under fullYoungScan: also require membership in MinorClosure.
    MinorClosure,
    // Authority = region mark bitmap only (no independent retrace membership).
    RegionMarkBitmap,
    // Authority = independent retrace only.
    IndependentRetrace,
};

const char* VerifyMarkSourceName(VerifyMarkSource source);
// Parse MRT_GCV2_VERIFY_MARK_SOURCE; default IndependentVsBitmap.
VerifyMarkSource ParseVerifyMarkSource();

} // namespace MapleRuntime

#endif // MRT_VERIFY_OPTION_H
