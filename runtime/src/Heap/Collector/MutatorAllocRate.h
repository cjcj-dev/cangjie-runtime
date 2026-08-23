// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MUTATOR_ALLOC_RATE_H
#define MRT_MUTATOR_ALLOC_RATE_H

#include <cstddef>
#include <cstdint>

#include "Heap/Collector/TruncatedSeq.h"

namespace MapleRuntime {

// zStat.cpp:935-1017 — ZStatMutatorAllocRate.
struct MutatorAllocRateStats {
    double avg = 0.0;
    double predict = 0.0;
    double sd = 0.0;
};

class MutatorAllocRate {
public:
    static void initialize();
    static void sample_allocation(size_t allocationBytes);
    static MutatorAllocRateStats stats();
    static size_t sampling_granule();
    static uint64_t sample_count();
    // zDirector.cpp:867 / zHeap.cpp:61 — SoftMaxHeapSize. Trigger denominator
    // only; allocation failure still uses hard capacity.
    static size_t soft_max_heap_size();

private:
    static void update_sampling_granule();
};

} // namespace MapleRuntime
#endif // MRT_MUTATOR_ALLOC_RATE_H
