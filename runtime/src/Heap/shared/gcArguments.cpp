#include "Heap/shared/gcArguments.hpp"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <limits>

#include "Base/Log.h"
#include "Base/Globals.h"
#include "Common/ColourEncoding.h"
#include "Heap/shared/jvmFlagConstraintsGC.hpp"
#include "Heap/z/zGlobals.hpp"
#include "RuntimeConfig.h"

namespace MapleRuntime {
namespace {
// HotSpot utilities/parseInteger.hpp:80-89, unsigned 64-bit specialization.
bool parse_integer_impl(const char* s, char** endptr, int base, size_t* result)
{
    if (s[0] == '-') {
        return false;
    }
    errno = 0;
    *result = std::strtoull(s, endptr, base);
    return errno == 0;
}

// HotSpot utilities/parseInteger.hpp:94-103.
bool multiply_by_1k(size_t& n)
{
    if (n >= std::numeric_limits<size_t>::min() / 1024 &&
        n <= std::numeric_limits<size_t>::max() / 1024) {
        n *= 1024;
        return true;
    } else {
        return false;
    }
}

// HotSpot utilities/parseInteger.hpp:120-166. No whitespace normalization.
bool parse_integer(const char* s, char** endptr, size_t* result)
{
    if (!std::isdigit(static_cast<unsigned char>(s[0])) && s[0] != '-') {
        return false;
    }
    size_t n = 0;
    const bool is_hex = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ||
        (s[0] == '-' && s[1] == '0' && (s[2] == 'x' || s[2] == 'X'));
    char* remainder;
    if (!parse_integer_impl(s, &remainder, is_hex ? 16 : 10, &n)) {
        return false;
    }
    if (remainder == s) {
        return false;
    }
    switch (*remainder) {
        case 'T': case 't':
            if (!multiply_by_1k(n)) return false;
            [[fallthrough]];
        case 'G': case 'g':
            if (!multiply_by_1k(n)) return false;
            [[fallthrough]];
        case 'M': case 'm':
            if (!multiply_by_1k(n)) return false;
            [[fallthrough]];
        case 'K': case 'k':
            if (!multiply_by_1k(n)) return false;
            ++remainder;
            break;
        default:
            break;
    }
    *result = n;
    *endptr = remainder;
    return true;
}

bool parse_integer(const char* s, size_t* result)
{
    char* remainder;
    bool rc = parse_integer(s, &remainder, result);
    rc = rc && (*remainder == '\0');
    return rc;
}
}

// HotSpot runtime/arguments.cpp:1669-1674; constraints are a separate stage.
GCArguments::ArgsRange GCArguments::parse_memory_size(const char* s, size_t* value,
                                                     size_t minimum, size_t maximum)
{
    if (!parse_integer(s, value)) return arg_unreadable;
    return check_memory_size(*value, minimum, maximum);
}

GCArguments::ArgsRange GCArguments::check_memory_size(size_t value, size_t minimum, size_t maximum)
{
    if (value < minimum) return arg_too_small;
    if (value > maximum) return arg_too_big;
    return arg_in_range;
}

bool GCArguments::initialize_heap_flags_and_sizes(HeapParam& param)
{
    size_t maximum = 0;
    size_t soft = 0;
    if (!CheckedMulSize(param.heapSize, KB, maximum)) {
        LOG(RTLOG_ERROR, "Heap size conversion overflows bytes");
        return false;
    }
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
