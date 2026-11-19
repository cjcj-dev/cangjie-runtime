// Cangjie runtime entry points for wasm32-wasi (M0 subset). See CjWasmRuntime.h.
#include "CjWasmRuntime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Unsupported features
_Noreturn void cjw_unsupported(const char* feature, int code)
{
    fprintf(stderr, "cangjie wasm32 runtime: %s is not supported on the wasm32-wasi target yet\n", feature);
    fflush(stderr);
    exit(code);
}

// ---------------------------------------------------------------------------
// Arena (collector boundary)
static size_t g_arenaBytes = 0;

void* cjw_alloc(size_t size, size_t align)
{
    if (align < 8) { align = 8; }
    size_t rounded = (size + align - 1) & ~(align - 1);
    void* p = aligned_alloc(align, rounded == 0 ? align : rounded);
    if (p == NULL) {
        fprintf(stderr, "cangjie wasm32 runtime: out of memory allocating %zu bytes (arena %zu bytes)\n",
                size, g_arenaBytes);
        exit(CJW_RC_UNSUPPORTED);
    }
    memset(p, 0, rounded);
    g_arenaBytes += rounded;
    return p;
}

void cjw_collect(void) { cjw_unsupported("garbage collection (arena only, M0)", CJW_RC_UNSUPPORTED); }

size_t cjw_arena_bytes(void) { return g_arenaBytes; }

// ---------------------------------------------------------------------------
// Allocation entry points used by compiled code
void* CJ_MCC_NewArray8(const void* arrayInfo, int64_t nElems, int64_t unused)
{
    (void)unused;
    if (nElems < 0) { cjw_unsupported("negative array length", CJW_RC_UNSUPPORTED); }
    uint8_t* obj = (uint8_t*)cjw_alloc(CJW_ARRAY_HEADER_SIZE + (size_t)nElems, 8);
    *(const void**)obj = arrayInfo;
    *(int64_t*)(obj + CJW_ARRAY_LENGTH_OFFSET) = nElems;
    return obj;
}

// ---------------------------------------------------------------------------
// Static references: no barriers are needed for a non-moving, single-threaded
// arena; reads and writes are plain loads/stores.
void CJ_MCC_WriteStaticRef(void* ref, void** field) { *field = ref; }
void* CJ_MCC_ReadStaticRef(void** field) { return *field; }

// No per-thread data, no stack growth: the wasm engine bounds the native stack.
void CJ_MCC_CheckThreadLocalDataOffset(void) {}
void CJ_MCC_StackCheck(void) {}

// ---------------------------------------------------------------------------
// Package initialization (single thread). Results mirror the native
// PackageInitResult: Execute=0, Ready=1, Failed=2, Reentrant=3.
enum { CJW_PI_EXECUTE = 0, CJW_PI_READY = 1, CJW_PI_FAILED = 2, CJW_PI_REENTRANT = 3 };
enum { CJW_ST_RUNNING = 1, CJW_ST_DONE = 2, CJW_ST_FAILED = 3 };
typedef struct { const void* pkg; const void* unit; uint32_t phase; uint32_t state; } CjwInitSlot;
#define CJW_MAX_INIT_SLOTS 4096
static CjwInitSlot g_initSlots[CJW_MAX_INIT_SLOTS];
static uint32_t g_initSlotCount = 0;

static CjwInitSlot* FindInitSlot(const void* pkg, const void* unit, uint32_t phase, int create)
{
    for (uint32_t i = 0; i < g_initSlotCount; ++i) {
        CjwInitSlot* s = &g_initSlots[i];
        if (s->pkg == pkg && s->unit == unit && s->phase == phase) { return s; }
    }
    if (!create) { return NULL; }
    if (g_initSlotCount == CJW_MAX_INIT_SLOTS) { cjw_unsupported("more than 4096 package init units", CJW_RC_PKGINIT); }
    CjwInitSlot* s = &g_initSlots[g_initSlotCount++];
    s->pkg = pkg; s->unit = unit; s->phase = phase; s->state = 0;
    return s;
}

uint32_t CJ_MCC_PackageInitBegin(const void* pkg, const void* unit, uint32_t phase, void** ownerToken)
{
    *ownerToken = NULL;
    CjwInitSlot* s = FindInitSlot(pkg, unit, phase, 1);
    switch (s->state) {
        case CJW_ST_DONE: return CJW_PI_READY;
        case CJW_ST_FAILED: return CJW_PI_FAILED;
        case CJW_ST_RUNNING: return CJW_PI_REENTRANT;
        default: break;
    }
    s->state = CJW_ST_RUNNING;
    *ownerToken = s;
    return CJW_PI_EXECUTE;
}

void CJ_MCC_PackageInitComplete(void* ownerToken) { ((CjwInitSlot*)ownerToken)->state = CJW_ST_DONE; }
void CJ_MCC_PackageInitFail(void* ownerToken, uint32_t code) { (void)code; ((CjwInitSlot*)ownerToken)->state = CJW_ST_FAILED; }
_Noreturn void CJ_MCC_PackageInitAbort(const void* pkg, const void* unit, uint32_t phase, uint32_t result)
{
    (void)pkg; (void)unit;
    fprintf(stderr, "cangjie wasm32 runtime: package initialization aborted (phase %u, result %u)\n", phase, result);
    exit(CJW_RC_PKGINIT);
}

// ---------------------------------------------------------------------------
// Exceptions: wasm EH is not enabled in this stage; a throw ends the program.
_Noreturn void CJ_MCC_ThrowException(void* exception)
{
    (void)exception;
    cjw_unsupported("throwing an exception (wasm exception handling, M1)", CJW_RC_EXCEPTION);
}

// ---------------------------------------------------------------------------
// Process entry. wasi-libc crt1-command.o: _start -> __main_argc_argv(argc, argv).
// cjcj emits `void main(i32, i8**)` which calls CJ_MRT_CjRuntimeInit,
// CJ_MRT_SetCommandLineArgs and CJ_MRT_CjRuntimeStart(cj_entry$).
static int g_argc = 0;
static const char** g_argv = NULL;

void CJ_MRT_CjRuntimeInit(void) {}
void CJ_MRT_SetCommandLineArgs(int argc, const char** argv) { g_argc = argc; g_argv = argv; }
int cjw_argc(void) { return g_argc; }
const char** cjw_argv(void) { return g_argv; }
void CJ_MRT_PreInitializePackage(void* packageInfo) { (void)packageInfo; }

void CJ_MRT_CjRuntimeStart(int (*entry)(void))
{
    int rc = entry();
    fflush(stdout);
    fflush(stderr);
    exit(rc);
}

extern void cjw_cangjie_main(int argc, char** argv) __asm__("main");
int __main_argc_argv(int argc, char** argv)
{
    cjw_cangjie_main(argc, argv);
    return 0;
}
