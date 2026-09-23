// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#include "Heap/z/zHeuristics.hpp"

#include <algorithm>
#include <cmath>
#include <thread>
#if defined(__linux__) || defined(hongmeng)
#include <sched.h>
#endif

#include "Heap/z/zCPU.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zNUMA.inline.hpp"
#include "Heap/z/z_globals.hpp"

namespace MapleRuntime {
namespace {
size_t g_maxHeapSize = 0;

size_t round_down_pow2(size_t value)
{
    if (value <= 1) {
        return value;
    }
    return size_t(1) << (63 - __builtin_clzll(value));
}

uint32_t nworkers_based_on_ncpus(double cpu_share_in_percent)
{
    unsigned ncpu = std::max(1u, std::thread::hardware_concurrency());
#if defined(__linux__) || defined(hongmeng)
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    if (sched_getaffinity(0, sizeof(cpus), &cpus) == 0 && CPU_COUNT(&cpus) > 0) {
        ncpu = static_cast<unsigned>(CPU_COUNT(&cpus));
    }
#endif
    return static_cast<uint32_t>(std::ceil(ncpu * cpu_share_in_percent / 100.0));
}

uint32_t nworkers_based_on_heap_size(double heap_share_in_percent)
{
    if (g_maxHeapSize == 0 || ZPageSizeSmall == 0) {
        return 1;
    }
    return static_cast<uint32_t>(g_maxHeapSize * (heap_share_in_percent / 100.0) / ZPageSizeSmall);
}

uint32_t nworkers(double cpu_share_in_percent)
{
    return std::min(nworkers_based_on_ncpus(cpu_share_in_percent), nworkers_based_on_heap_size(2.0));
}
}

void ZHeuristics::set_max_heap_size(size_t bytes) { g_maxHeapSize = bytes; }
size_t ZHeuristics::max_heap_size() { return g_maxHeapSize; }

void ZHeuristics::set_medium_page_size()
{
    const size_t min = ZGranuleSize;
    const size_t max = ZGranuleSize * 16;
    const size_t unclamped = static_cast<size_t>(g_maxHeapSize * 0.03125);
    const size_t clamped = std::min(max, std::max(min, unclamped));
    const size_t size = round_down_pow2(clamped);
    if (size > ZPageSizeSmall) {
        ZPageSizeMediumMax = size;
        ZPageSizeMediumMaxShift = 63 - __builtin_clzll(ZPageSizeMediumMax);
        ZObjectSizeLimitMedium = ZPageSizeMediumMax / 8;
        ZObjectAlignmentMediumShift = ZPageSizeMediumMaxShift - 13;
        ZObjectAlignmentMedium = 1 << ZObjectAlignmentMediumShift;
        ZPageSizeMediumEnabled = true;
        ZPageSizeMediumMin = ZObjectSizeLimitMedium < ZGranuleSize ? ZGranuleSize :
                             ((ZObjectSizeLimitMedium + ZGranuleSize - 1) & ~(ZGranuleSize - 1));
    }
}

size_t ZHeuristics::relocation_headroom()
{
    const size_t medium = ZPageSizeMediumEnabled ? ZPageSizeMediumMax : 0;
    const size_t per_numa = (static_cast<size_t>(ConcGCThreads) * ZPageSizeSmall) + medium;
    return per_numa * std::max<size_t>(1, NumaTopology::SealProcessTopology().Count());
}

bool ZHeuristics::use_per_cpu_shared_small_pages()
{
    const uint32_t ncpu = std::max(1u, ZCPU::count());
    const size_t per_cpu_share = significant_heap_overhead() / ncpu;
    return per_cpu_share >= ZPageSizeSmall;
}

uint32_t ZHeuristics::nparallel_workers() { return std::max(nworkers(60.0), 1u); }
uint32_t ZHeuristics::nconcurrent_workers() { return std::max(nworkers(25.0), 1u); }

size_t ZHeuristics::significant_heap_overhead()
{
    return static_cast<size_t>(g_maxHeapSize * (ZFragmentationLimit / 100));
}

size_t ZHeuristics::significant_young_overhead()
{
    return static_cast<size_t>(g_maxHeapSize * (ZYoungCompactionLimit / 100));
}
}
