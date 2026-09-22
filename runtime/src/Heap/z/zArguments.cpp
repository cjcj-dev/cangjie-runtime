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
    const size_t maxHeap = ZHeuristics::max_heap_size();
    if (maxHeap > 0) {
        ZHeuristics::set_max_heap_size(maxHeap * 90 / 100 + (maxHeap * 10 / 100));
    }
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
    initialize_heap_flags_and_sizes();
    select_max_gc_threads();
}

Heap* ZArguments::create_heap() { return &Heap::GetHeap(); }

bool ZArguments::is_supported() { return true; }
}
