#include "Heap/shared/gcArguments.hpp"

#include <limits>

#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/Globals.h"
#include "Common/ColourEncoding.h"
#include "Heap/shared/jvmFlagConstraintsGC.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "RuntimeConfig.h"

namespace MapleRuntime {
// Cangjie environment syntax is defined by CString::ParseSizeFromEnv
// (upstream Base/CString.cpp:394-423), not HotSpot's -Xmx parser.
// The official parser returns KB; GC flags and their constraints use bytes.
GCArguments::ArgsRange GCArguments::parse_memory_size(const char* s, size_t* value,
                                                     size_t minimum, size_t maximum)
{
    const size_t kilobytes = CString::ParseSizeFromEnv(CString(s));
    if (kilobytes == 0 || !CheckedMulSize(kilobytes, KB, *value)) return arg_unreadable;
    return check_memory_size(*value, minimum, maximum);
}

GCArguments::ArgsRange GCArguments::check_memory_size(size_t value, size_t minimum, size_t maximum)
{
    if (value < minimum) return arg_too_small;
    if (value > maximum) return arg_too_big;
    return arg_in_range;
}

bool GCArguments::parse_vm_init_args(HeapParam& param)
{
    size_t maximum = 0;
    size_t soft = 0;
    // HotSpot arguments.cpp:2112-2125: parse the selected maximum in bytes,
    // then publish the flag before GC sizing and soft-limit constraints.
    if (const char* value = GetRuntimeConfigValue("cjHeapSize")) {
        if (parse_memory_size(value, &maximum, 1, std::numeric_limits<size_t>::max()) != arg_in_range) {
            LOG(RTLOG_ERROR, "Invalid cjHeapSize");
            return false;
        }
        param.heapSizeSet = true;
    } else if (!CheckedMulSize(param.heapSize, KB, maximum)) {
        LOG(RTLOG_ERROR, "Heap size conversion overflows bytes");
        return false;
    }
    ZHeuristics::set_max_heap_size(maximum);
    // Resolve the selected source before validating it: environment > embedded table / API > default.
    if (const char* value = GetRuntimeConfigValue("cjSoftMaxHeapSize")) {
        if (parse_memory_size(value, &soft, 0, std::numeric_limits<size_t>::max()) != arg_in_range) {
            LOG(RTLOG_ERROR, "Invalid cjSoftMaxHeapSize");
            return false;
        }
        param.softHeapSizeSet = true;
    } else if (param.softHeapSizeSet && !CheckedMulSize(param.softHeapSize, KB, soft)) {
        LOG(RTLOG_ERROR, "Heap size conversion overflows bytes");
        return false;
    }
    SoftMaxHeapSize.store(soft, std::memory_order_release);
    return true;
}

bool GCArguments::initialize_heap_flags_and_sizes(HeapParam& param)
{
    size_t maximum = ZHeuristics::max_heap_size();
    size_t soft = SoftMaxHeapSize.load(std::memory_order_acquire);
    // HotSpot gc/shared/gcArguments.cpp:251-270: validate before alignment,
    // then update the maximum flag before deriving the default soft maximum.
    if (maximum < 2 * MB) {
        LOG(RTLOG_ERROR, "Invalid cjHeapSize: too small maximum heap");
        return false;
    }
    if (!CheckedRoundUpSize(maximum, ZGranuleSize, maximum)) {
        LOG(RTLOG_ERROR, "Invalid cjHeapSize: maximum heap alignment overflows bytes");
        return false;
    }
    ZHeuristics::set_max_heap_size(maximum);
    // gcArguments.cpp:282: the default soft flag first takes the hard limit.
    if (!param.softHeapSizeSet) {
        soft = maximum;
    }
    if (!SoftMaxHeapSizeConstraintFunc(soft, maximum)) {
        return false;
    }
    // The size flag is in bytes, including non-KB-aligned environment values.
    SoftMaxHeapSize.store(soft, std::memory_order_release);
    return true;
}
}
