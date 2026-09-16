// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zNUMA.hpp"
#include "Heap/z/zValue.inline.hpp"

#include <cassert>

namespace MapleRuntime {
inline size_t NumaTopology::Count() const { return nodes.size(); }

// ZGC zNUMA.inline.hpp:45-59 (ZNUMA::calculate_share).
inline size_t NumaTopology::calculate_share(uint32_t numa_id, size_t total, size_t granule, uint32_t ignore_count)
{
    // A03n: use the same disabled-NUMA (one partition) domain as P06 storage.
    const uint32_t count = ZPerNUMAStorage::count();
    assert(total % granule == 0);
    assert(ignore_count < count);
    assert(numa_id < count - ignore_count);
    (void)numa_id;

    const uint32_t num_nodes = count - ignore_count;
    const size_t base_share = ((total / num_nodes) / granule) * granule;

    const size_t extra_share_nodes = (total - base_share * num_nodes) / granule;
    if (numa_id < extra_share_nodes) {
        return base_share + granule;
    }

    return base_share;
}
}
