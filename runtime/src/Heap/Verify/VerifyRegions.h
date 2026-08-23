// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
// Region/heap structural verifier (HotSpot G1HeapVerifier::verify_region_sets intent).
// Gated by MRT_GCV2_VERIFY_REGIONS=1 (default off). Optional fatal: MRT_GCV2_VERIFY_REGIONS_FATAL=1.
// Optional start-at: MRT_GCV2_VERIFY_REGIONS_START_AT=<N> (skip until N-th young GC, 1-based).

#ifndef MRT_VERIFY_REGIONS_H
#define MRT_VERIFY_REGIONS_H

#include <cstddef>
#include <unordered_set>

#include "Allocator/RegionInfo.h"

namespace MapleRuntime {
class RegionManager;

// Standalone region-set verifier. Does not mutate region selection or marking.
class VerifyRegions {
public:
    using CandidateSet = std::unordered_set<RegionInfo*>;

    // After PrepareYoungGarbageCandidates: list membership + young/candidate relation.
    static void VerifyAfterPrepareYoung(RegionManager& manager, const CandidateSet& candidates, size_t youngRunIndex,
                                        const char* point);

    // After young marking: objects that are young+marked but whose region is not a candidate
    // (positive control for the 4138-class defect).
    static void VerifyAfterYoungMark(RegionManager& manager, const CandidateSet& candidates, size_t youngRunIndex,
                                     const char* point);

    static bool IsEnabled();
    static bool IsFatal();
    static bool ShouldRunAt(size_t youngRunIndex);

private:
    static void ReportAndMaybeAbort(bool failed, const char* detail);
};

} // namespace MapleRuntime

#endif // MRT_VERIFY_REGIONS_H
