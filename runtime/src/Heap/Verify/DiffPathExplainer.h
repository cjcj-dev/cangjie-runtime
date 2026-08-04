// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_DIFF_PATH_EXPLAINER_H
#define MRT_DIFF_PATH_EXPLAINER_H

#include <cstddef>
#include <functional>
#include <unordered_set>

#include "Common/BaseObject.h"
#include "Common/TypeDef.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class RegionInfo;

// Diff-path explainer (HotSpot "why is this live" analog for young-only mark gaps).
//
// Runs TWO independent closures (must NOT share the production TraceYoungClosure visitor):
//   甲 YoungOnly: roots + remset slots, only follow into young objects
//   乙 Full:      roots, follow every heap ref
// Diff D = young(乙) \ young(甲). For each obj in D, print region attrs + one root path
// and classify every edge (holder young/old, in remset?, remset consumed?).
//
// Gate (default off):  MRT_GCV2_DIFF_PATH=1
// Cost control:        MRT_GCV2_DIFF_PATH_START_AT=<N>  (1-based minor run)
//                      MRT_GCV2_DIFF_PATH_EVERY=<N>
//                      MRT_GCV2_DIFF_PATH_MAX_FAILURES=<N>  (max |D| samples printed; default 8)
// Optional abort:      MRT_GCV2_DIFF_PATH_FATAL=1
//
// Remset consume-vs-recorded (G1SummarizeRSetStats analog) is reported whenever
// MRT_GCV2_REMSET_STATS=1 OR MRT_GCV2_DIFF_PATH=1:
//   recorded  = slots acquired for this minor
//   live      = slots surviving weak/FYS filter into Rescan
//   consumed  = slots Rescan actually walked as roots
//
// Independence: both closures live in DiffPathExplainer.cpp. Production
// TraceYoungClosure / RescanRememberedSet are never called from here.

struct DiffPathRemsetStats {
    size_t recorded = 0;
    size_t live = 0;
    size_t consumed = 0;
    size_t skippedNotHeap = 0;
    size_t skippedWeak = 0;
    size_t skippedFysFilter = 0;
};

// resolveField: map a ref field to its current target (forwarded if needed).
// visitRoots: push every minor root object into the provided callback.
// remsetSlots: the post-AcquireRecordsForMinor snapshot (recorded set).
// consumedSlots: slots Rescan actually walked (may equal live under FYS=0).
// minorRunIndex: 1-based young GC index (for START_AT / EVERY).
// candidateRegions: optional; if non-null, region "isCandidate" uses membership.
// rootReachableOut: optional; if non-null, receives the independent full-root closure
// even when MRT_GCV2_DIFF_PATH is disabled.
void RunDiffPathExplainer(size_t minorRunIndex,
                          const std::function<void(const std::function<void(BaseObject*)>&)>& visitRoots,
                          const std::function<BaseObject*(RefField<>&)>& resolveField,
                          const std::unordered_set<MAddress>& remsetSlots,
                          const std::unordered_set<MAddress>& consumedSlots,
                          const std::unordered_set<RegionInfo*>* candidateRegions,
                          const DiffPathRemsetStats& remsetStats,
                          std::unordered_set<BaseObject*>* rootReachableOut);

// Cheap remset consume-vs-recorded line (no dual closure). Default off unless
// MRT_GCV2_REMSET_STATS=1. Always safe to call; gate is inside.
void ReportRemsetConsumeStats(size_t minorRunIndex, const DiffPathRemsetStats& stats);

} // namespace MapleRuntime

#endif // MRT_DIFF_PATH_EXPLAINER_H
