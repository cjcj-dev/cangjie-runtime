// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_GARBAGE_VERDICT_H
#define MRT_HEAP_GARBAGE_VERDICT_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {

class RegionInfo;

// Dump region state at the moment a region is judged garbage / kept from CollectRegion.
// Gate (default off): MRT_GCV2_GARBAGE_VERDICT=1
// Blocking (default off): MRT_GCV2_BLOCK_NEVEREXAMINED=1
//   — neverExamined regions never CollectRegion (keep path at ForwardRegion + CollectRegion entry)
// Blocking (default off): MRT_GCV2_BLOCK_FORWARDED_RESIDUAL=1
//   — after RouteState::FORWARDED, skip CollectRegion (keep unmovable) to test residual UAF
class GarbageVerdict {
public:
    static bool Enabled();
    static bool BlockNeverExamined();
    static bool BlockForwardedResidual();

    // site: "collect" | "fwd_empty_keep" | "fwd_empty_collect" | "fwd_after" | "fwd_after_keep" | "pinned_empty" | "large"
    static void Dump(const char* site, RegionInfo* region, const char* predicate);

    // At invalid-minor-root: match crash addr against recent dumps.
    static void CrossCheck(MAddress crashAddr);

    // Independent of live counter: walk headers while IsValidObject.
    static size_t CountValidObjectHeaders(RegionInfo* region);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_GARBAGE_VERDICT_H
