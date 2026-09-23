#include "Heap/shared/gcArguments.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>

#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/Globals.h"
#include "Common/ColourEncoding.h"
#include "Heap/shared/jvmFlagConstraintsGC.hpp"
#include "RuntimeConfig.h"

namespace MapleRuntime {
namespace {
// Arguments::atojulong (arguments.cpp:744): validate conversion and scaling
// before publishing a size flag. Cangjie size options use KB/MB/GB units.
bool ParseSoftHeapSize(const char* value, size_t& kb)
{
    const CString text = CString(value).RemoveBlankSpace();
    const size_t length = text.Length();
    if (length <= 2) {
        return false;
    }
    const CString number = text.SubStr(0, length - 2);
    CString unit = text.SubStr(length - 2, 2);
    unit.ToLowerCase();
    if (number.Str()[0] < '0' || number.Str()[0] > '9') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(number.Str(), &end, 0);
    if (errno == ERANGE || *end != '\0' || parsed > std::numeric_limits<size_t>::max()) {
        return false;
    }
    const size_t scale = unit == "kb" ? 1 : unit == "mb" ? KB : unit == "gb" ? MB : 0;
    return scale != 0 && CheckedMulSize(static_cast<size_t>(parsed), scale, kb);
}
}

bool GCArguments::initialize_heap_flags_and_sizes(HeapParam& param)
{
    // Resolve the selected source before validating it: environment > API > default.
    if (const char* env = std::getenv("cjSoftMaxHeapSize")) {
        if (!ParseSoftHeapSize(env, param.softHeapSize)) {
            LOG(RTLOG_ERROR, "Invalid cjSoftMaxHeapSize");
            return false;
        }
        param.softHeapSizeSet = true;
    }
    size_t maximum = 0;
    size_t soft = 0;
    if (!CheckedMulSize(param.heapSize, KB, maximum) ||
        (param.softHeapSizeSet && !CheckedMulSize(param.softHeapSize, KB, soft))) {
        LOG(RTLOG_ERROR, "Heap size conversion overflows bytes");
        return false;
    }
    // gcArguments.cpp:282: the default soft flag first takes the hard limit.
    if (!param.softHeapSizeSet) {
        param.softHeapSize = param.heapSize;
        soft = maximum;
    }
    return SoftMaxHeapSizeConstraintFunc(soft, maximum);
}
}
