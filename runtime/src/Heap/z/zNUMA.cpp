// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zNUMA.inline.hpp"

#include <algorithm>

namespace MapleRuntime {

NumaTopology NumaTopology::Seal(const std::vector<uint32_t>& nodeIds)
{
    NumaTopology topology;
    topology.nodes = nodeIds;
    std::sort(topology.nodes.begin(), topology.nodes.end());
    topology.nodes.erase(std::unique(topology.nodes.begin(), topology.nodes.end()), topology.nodes.end());
    if (topology.nodes.empty()) {
        topology.nodes.push_back(0);
    }
    topology.sealed = true;
    return topology;
}

bool NumaTopology::Contains(uint32_t node) const
{
    return std::binary_search(nodes.begin(), nodes.end(), node);
}

}
