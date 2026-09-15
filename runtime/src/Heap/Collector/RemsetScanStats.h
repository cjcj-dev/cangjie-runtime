// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_REMSET_SCAN_STATS_H
#define MRT_REMSET_SCAN_STATS_H

#include <cstddef>

namespace MapleRuntime {

// Product ledger shared by remembered-set rescan, concurrent young marking,
// and evacuation. It belongs to the collector data flow, not a verifier.
struct RemsetScanStats {
    size_t recorded = 0;
    size_t live = 0;
    size_t consumed = 0;
    size_t skippedNotHeap = 0;
    size_t skippedWeak = 0;
    size_t skippedFysFilter = 0;
};

} // namespace MapleRuntime

#endif // MRT_REMSET_SCAN_STATS_H
