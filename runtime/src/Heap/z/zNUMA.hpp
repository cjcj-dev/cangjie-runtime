// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace MapleRuntime {
class NumaTopology {
public:
    static NumaTopology Seal(const std::vector<uint32_t>& nodeIds);
    static NumaTopology SealProcessTopology();

    bool IsSealed() const { return sealed; }
    size_t Count() const;
    uint32_t NodeAt(size_t index) const { return nodes[index]; }
    bool Contains(uint32_t node) const;

private:
    std::vector<uint32_t> nodes;
    bool sealed{ false };
};

}
