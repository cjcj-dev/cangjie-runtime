#pragma once

namespace MapleRuntime {
class Heap;

class ZArguments {
public:
    static void initialize_alignments();
    static void select_max_gc_threads();
    static void initialize();
    static Heap* create_heap();
    static bool is_supported();
    static bool gc_enabled();
};
}
