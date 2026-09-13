// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zVirtualMemory.hpp"
#include <vector>
namespace MapleRuntime {
class ReservationRegistry;
class NumaTopology;
struct NumaPartitionRange {
    MemoryRange range;
    uint32_t node{ 0 };
};

class NumaPartitionRegistry {
public:
    bool Initialize(const ReservationRegistry& reservations, const NumaTopology& topology);
    bool Owns(uintptr_t start, size_t size, uint32_t node) const;
    const std::vector<NumaPartitionRange>& Ranges() const { return ranges; }

private:
    std::vector<NumaPartitionRange> ranges;
};

}
