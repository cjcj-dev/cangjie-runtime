// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_INTERIOR_SRC_PROBE_H
#define MRT_HEAP_INTERIOR_SRC_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Read-only probe: classify every young workStack push (via PushYoungObject funnel)
// as object-base vs interior pointer, and attribute by source tag.
// Gate (default off): MRT_GCV2_INTERIOR_SRC=1
// Optional dump cap:   MRT_GCV2_INTERIOR_SRC_DUMP_MAX=<N> (default 64)
class InteriorSrcProbe {
public:
    static bool Enabled();

    // Call immediately before workStack.push_back(object) on the minor young path.
    // source: alloc_buffer | minor_root | closure_edge | remset | <other named>
    // slot: optional source slot address (0 if unknown); slotVal may equal object.
    static void NotePush(const char* source, void* object, uintptr_t slot = 0, uintptr_t slotVal = 0);

    static void FlushSummary(const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_INTERIOR_SRC_PROBE_H
