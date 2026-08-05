// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_BULK_EDGE_H
#define MRT_HEAP_BULK_EDGE_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {

// Lightweight set of heap ref-slot addresses written by bulk memcpy/memmove paths
// (no per-slot WriteReference / SATB). Used at F3 abort to test whether an
// unmarked live holder is only reachable via bulk-established edges.
//
// Gate (default off): MRT_GCV2_BULKEDGE=1
// Cap: MRT_GCV2_BULKEDGE_CAP=<N> (default 1<<20 slots)
class BulkEdge {
public:
    static bool Enabled();

    // Record every pointer-aligned address in [start, start+size).
    static void NoteBulkRange(MAddress start, size_t size, const char* site);

    static bool Contains(MAddress slot);

    // Stats for hit-rate reporting.
    static void Stats(size_t& noteCalls, size_t& slotsInserted, size_t& slotsDropped, size_t& cap,
                      size_t& containsHits, size_t& containsMisses);

    // Reverse-edge scan + bulk membership for one holder. Logs [GCV2][BULKEDGE].
    // Returns number of bulk incoming edges found (among sampled).
    static size_t ClassifyHolderInEdges(void* holder, int holderValid, int holderMarked);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_BULK_EDGE_H
