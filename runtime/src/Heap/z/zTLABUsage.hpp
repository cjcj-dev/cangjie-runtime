// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#pragma once

#include <algorithm>
#include <functional>
#include <mutex>
#include <unordered_set>

#include "Common/MarkWorkStack.h"
#include "Heap/z/zMarkStackEntry.hpp"
#include "Heap/Allocator/RegionList.h"

namespace MapleRuntime {
class TLABAllocationAverage {
public:
    void Sample(double value)
    {
        samples = std::min(samples + 1, 100u);
        const unsigned weight = std::max(35u, 100u / samples);
        average = ((100 - weight) * average + weight * value) / 100.0;
    }
    double Average() const { return average; }
private:
    unsigned samples = 0;
    double average = 0;
};

// ThreadLocalAllocStats (threadLocalAllocBuffer.cpp:346-412), in bytes.
struct TLABStatistics {
    size_t allocatedSize = 0;
    size_t refillWaste = 0;
    size_t gcWaste = 0;
    size_t refills = 0;
    size_t allocatingThreads = 0;

    size_t Used() const { return allocatedSize - refillWaste - gcWaste; }
    void Update(const TLABStatistics& other)
    {
        allocatedSize += other.allocatedSize;
        refillWaste += other.refillWaste;
        gcWaste += other.gcWaste;
        refills += other.refills;
        allocatingThreads += other.allocatingThreads;
    }
};

// thread-local data structure
}
