#include "Heap/z/zArguments.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "CangjieRuntime.h"
#include "Heap/z/z_globals.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zHeap.hpp"
#include "RuntimeConfig.h"

namespace MapleRuntime {
namespace {
bool g_gcEnabled = true;
}

void ZArguments::initialize_alignments() {}

void ZArguments::initialize_heap_flags_and_sizes()
{
    const HeapParam param = CangjieRuntime::GetHeapParam();
    const bool soft_is_explicit = param.softHeapSizeSet;
    size_t soft = param.softHeapSize * KB;
    // GCArguments:282-283 initializes default soft to max. ZArguments:43-50
    // applies ergonomics only without explicit sizing. No MaxRAMPercentage
    // parameter exists in this runtime, so that origin condition is true.
    if (!param.heapSizeSet && !soft_is_explicit) {
        soft = ZHeuristics::max_heap_size() * 90 / 100;
    }
    SoftMaxHeapSize.store(soft, std::memory_order_release);
}

void ZArguments::select_max_gc_threads()
{
    // ZGC zArguments.cpp:67-118: explicit flags precede ergonomics at this entry.
    const GCParam param = CangjieRuntime::GetGCParam();
    UseDynamicNumberOfGCThreads = !param.staticGCThreads;
    ConcGCThreads = param.concGCThreads;
    ZYoungGCThreads = param.youngGCThreads;
    ZOldGCThreads = param.oldGCThreads;
    uint32_t max_nworkers_generation;
    if (param.concGCThreads == 0) {
        max_nworkers_generation = ZHeuristics::nconcurrent_workers();
        uint32_t max_nworkers = max_nworkers_generation;
        if (param.youngGCThreads != 0) {
            max_nworkers = std::max(max_nworkers, ZYoungGCThreads);
        }
        if (param.oldGCThreads != 0) {
            max_nworkers = std::max(max_nworkers, ZOldGCThreads);
        }
        ConcGCThreads = max_nworkers;
    } else {
        max_nworkers_generation = ConcGCThreads;
    }
    if (param.youngGCThreads == 0) {
        if (UseDynamicNumberOfGCThreads) {
            ZYoungGCThreads = max_nworkers_generation;
        } else {
            const uint32_t static_young_threads = std::max(uint32_t(max_nworkers_generation * 0.9), 1u);
            ZYoungGCThreads = static_young_threads;
        }
    }
    if (param.oldGCThreads == 0) {
        if (UseDynamicNumberOfGCThreads) {
            ZOldGCThreads = max_nworkers_generation;
        } else {
            const uint32_t static_old_threads = std::max(ConcGCThreads - ZYoungGCThreads, 1u);
            ZOldGCThreads = static_old_threads;
        }
    }
    CHECK_DETAIL(ConcGCThreads != 0, "ConcGCThreads must be positive");
    CHECK_DETAIL(ZYoungGCThreads > 0 && ZYoungGCThreads <= ConcGCThreads,
                 "ZYoungGCThreads must be in [1, ConcGCThreads]");
    CHECK_DETAIL(ZOldGCThreads > 0 && ZOldGCThreads <= ConcGCThreads,
                 "ZOldGCThreads must be in [1, ConcGCThreads]");
}

bool ZArguments::gc_enabled() { return g_gcEnabled; }

void ZArguments::initialize()
{
    initialize_alignments();
    const char* enableGC = std::getenv("cjEnableGC");
    if (enableGC != nullptr) {
        if (std::strlen(enableGC) == 1 && enableGC[0] == '0') {
            g_gcEnabled = false;
        } else if (std::strlen(enableGC) == 1 && enableGC[0] == '1') {
            g_gcEnabled = true;
        } else {
            LOG(RTLOG_ERROR, "Unsupported cjEnableGC, cjEnableGC should be 0 or 1.\n");
            g_gcEnabled = true;
        }
    }
    select_max_gc_threads();

    // zArguments.cpp: medium sizing precedes relocation-headroom ergonomics.
    ZHeuristics::set_medium_page_size();
    const GCParam param = CangjieRuntime::GetGCParam();
    // Map the public interval once; director rules consume the independent ZGC flags.
    const double collection_interval = param.backupGCInterval == 0 ? -1.0 :
        static_cast<double>(param.backupGCInterval) / SECOND_TO_NANO_SECOND;
    ZCollectionIntervalMinor = collection_interval;
    ZCollectionIntervalMajor = collection_interval;
    bool max_threshold_is_default = !param.maxTenuringThresholdSet;
    MaxTenuringThreshold = max_threshold_is_default ? 15 : param.maxTenuringThreshold;
    ZTenuringThreshold = param.zTenuringThresholdSet ? param.zTenuringThreshold : -1;
    CHECK_DETAIL(MaxTenuringThreshold <= 16, "MaxTenuringThreshold must be in [0, 16]");
    CHECK_DETAIL(ZTenuringThreshold >= -1 && ZTenuringThreshold <= 15,
                 "ZTenuringThreshold must be in [-1, 15]");

    if (param.zTenuringThresholdSet && ZTenuringThreshold != -1) {
        if (max_threshold_is_default) {
            MaxTenuringThreshold = static_cast<uint32_t>(ZTenuringThreshold);
            max_threshold_is_default = false;
        }
    }
    if (max_threshold_is_default) {
        uint32_t tenuring_threshold;
        for (tenuring_threshold = 0; tenuring_threshold < MaxTenuringThreshold; ++tenuring_threshold) {
            const size_t per_age_overhead = ZHeuristics::relocation_headroom();
            if (per_age_overhead * tenuring_threshold >= ZHeuristics::significant_young_overhead()) {
                break;
            }
        }
        MaxTenuringThreshold = tenuring_threshold;
    }
    // ZGC zArguments.cpp:188-191: validate after deriving the maximum.
    if (param.zTenuringThresholdSet && ZTenuringThreshold > static_cast<int32_t>(MaxTenuringThreshold)) {
        CHECK_DETAIL(false, "ZTenuringThreshold must be within bounds of MaxTenuringThreshold");
    }
}

Heap* ZArguments::create_heap() { return &Heap::GetHeap(); }

bool ZArguments::is_supported() { return true; }
}
