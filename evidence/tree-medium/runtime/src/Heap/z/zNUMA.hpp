// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// NUMA topology value object. ZGC keeps a static ZNUMA class
// (zNUMA.hpp:30-58); moving to that shape is A03n. calculate_share follows
// ZNUMA::calculate_share (zNUMA.inline.hpp:45-59) so the memory managers can
// already split capacity per partition the ZGC way.

#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace MapleRuntime {
class NumaTopology {
public:
    static NumaTopology Seal(const std::vector<uint32_t>& nodeIds);
    static NumaTopology SealProcessTopology();

    static size_t calculate_share(uint32_t numa_id, size_t total, size_t granule, uint32_t ignore_count = 0);

    static void numa_make_local(void* addr, size_t size, uint32_t numa_id);

    bool IsSealed() const { return sealed; }
    size_t Count() const;
    uint32_t NodeAt(size_t index) const { return nodes[index]; }
    bool Contains(uint32_t node) const;

private:
    std::vector<uint32_t> nodes;
    bool sealed{ false };
};

}
