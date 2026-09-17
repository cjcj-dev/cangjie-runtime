#include "Heap/z/zArguments.hpp"

#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
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
    (void)ZHeuristics::nconcurrent_workers();
    (void)ZHeuristics::nparallel_workers();
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
