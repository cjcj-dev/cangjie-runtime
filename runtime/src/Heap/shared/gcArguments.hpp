#pragma once

#include "Cangjie.h"

namespace MapleRuntime {
class GCArguments {
public:
    enum ArgsRange { arg_unreadable, arg_too_small, arg_too_big, arg_in_range };
    static ArgsRange parse_memory_size(const char* s, size_t* value, size_t minimum, size_t maximum);
    static ArgsRange check_memory_size(size_t value, size_t minimum, size_t maximum);
    static bool parse_vm_init_args(HeapParam& param);
    static bool initialize_heap_flags_and_sizes(HeapParam& param);
};
}
