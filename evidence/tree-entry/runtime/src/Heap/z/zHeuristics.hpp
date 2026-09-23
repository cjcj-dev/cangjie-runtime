// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#ifndef SHARE_GC_Z_ZHEURISTICS_HPP
#define SHARE_GC_Z_ZHEURISTICS_HPP

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class ZHeuristics {
public:
    static void set_medium_page_size();
    static size_t relocation_headroom();
    static bool use_per_cpu_shared_small_pages();
    static uint32_t nparallel_workers();
    static uint32_t nconcurrent_workers();
    static size_t significant_heap_overhead();
    static size_t significant_young_overhead();
    static void set_max_heap_size(size_t bytes);
    static size_t max_heap_size();
};
}

#endif
