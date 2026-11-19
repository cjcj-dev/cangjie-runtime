// Runtime-held root test: links CjWasmRuntime.c + CjWasmHeap.c (the full archive contents) and supplies the
// Cangjie entry symbol `main` itself. Checks that the managed Thread object CjWasmRuntime.c keeps in a C static
// (CJ_MCC_SetCurrentCJThreadObject) is an internal GC root. Exit status = number of failed checks.
#include "../CjWasmHeap.h"
#include "../CjWasmObjectModel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* CJ_MCC_NewObject(const void* typeInfo, uint32_t size);
void CJ_MCC_SetCurrentCJThreadObject(void* thread);

typedef struct { _Alignas(8) uint8_t b[56]; } TestTI;
static TestTI tiLeaf;

void cjw_root_test_main(int argc, char** argv) __asm__("main");
void cjw_root_test_main(int argc, char** argv)
{
    (void)argc; (void)argv;
    memset(&tiLeaf, 0, sizeof tiLeaf);
    *(int8_t*)(tiLeaf.b + CJW_TI_KIND_OFFSET) = (int8_t)-128;   // class without references
    *(uint32_t*)(tiLeaf.b + CJW_TI_SIZE_OFFSET) = 12;
    int fails = 0;
    void* thread = CJ_MCC_NewObject(&tiLeaf, 12);
    void* control = CJ_MCC_NewObject(&tiLeaf, 12);              // only a C local: not a root
    CJ_MCC_SetCurrentCJThreadObject(thread);
    cjw_gc_declare_precise_roots();
    cjw_collect();
    if (!cjw_heap_is_object(thread)) { puts("check failed: thread object freed while it is the current thread"); fails++; }
    if (cjw_heap_is_object(control)) { puts("check failed: unrooted control object survived"); fails++; }
    CJ_MCC_SetCurrentCJThreadObject(NULL);
    cjw_collect();
    if (cjw_heap_is_object(thread)) { puts("check failed: thread object survived after the slot was cleared"); fails++; }
    printf("THREAD_ROOT_FAILS=%d\n", fails);
    fflush(stdout);
    exit(fails);
}
