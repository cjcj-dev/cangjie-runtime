#include "Heap/shared/jvmFlagConstraintsGC.hpp"

#include "Base/Log.h"

namespace MapleRuntime {
// HotSpot gc/shared/jvmFlagConstraintsGC.cpp:292-299.
bool SoftMaxHeapSizeConstraintFunc(size_t value, size_t max_heap_size)
{
    if (value > max_heap_size) {
        LOG(RTLOG_ERROR, "SoftMaxHeapSize must be less than or equal to the maximum heap size");
        return false;
    }
    return true;
}
}
