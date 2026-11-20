// Root-completeness gate of the wasm32-wasi runtime (CjWasmHeap.h), linked without the legacy
// frame member, so only the module records decide.   root_gate_test.wasm <mode>
#include "../CjWasmHeap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Noreturn void cjw_unsupported(const char* feature, int code)
{
    fprintf(stderr, "cangjie wasm32 runtime: %s is not supported on the wasm32-wasi target yet\n", feature);
    fflush(stderr);
    exit(code);
}

static void* g_a;
static void* g_b;
static void** const kTable[2] = {&g_a, &g_b};

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "";
    const char* want;
    if (strcmp(mode, "complete") == 0) {
        cjw_static_root_table_register(kTable, 2);
        cjw_roots_module_complete(kTable);
        cjw_roots_module_complete(NULL);
        want = "complete";
    } else if (strcmp(mode, "incomplete") == 0) {
        cjw_static_root_table_register(kTable, 2);
        cjw_roots_module_complete(kTable);
        cjw_roots_module_incomplete(NULL);
        want = "incomplete-module";
    } else if (strcmp(mode, "unrecorded_table") == 0) {
        cjw_static_root_table_register(kTable, 2);
        cjw_roots_module_complete(NULL);
        want = "unrecorded-static-table";
    } else if (strcmp(mode, "no_record") == 0) {
        want = "no-root-record";
    } else {
        return 99;
    }
    cjw_gc_evaluate_roots_gate();
    int precise = cjw_gc_roots_precise();
    int ok = strcmp(cjw_gc_roots_gate(), want) == 0 && precise == (strcmp(want, "complete") == 0);
    printf("%s gate_%s roots=%s precise=%d\n", ok ? "PASS" : "FAIL", mode, cjw_gc_roots_gate(), precise);
    return ok ? 0 : 1;
}
