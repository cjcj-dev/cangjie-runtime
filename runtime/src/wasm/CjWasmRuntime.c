// Cangjie runtime entry points for wasm32-wasi (M0 subset). See CjWasmRuntime.h.
#define _POSIX_C_SOURCE 200809L // clock_gettime under -std=c11
#include "CjWasmRuntime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

void* CJ_MCC_NewArray16(const void* arrayInfo, int64_t nElems, int64_t unused);
void* CJ_MCC_NewArray32(const void* arrayInfo, int64_t nElems, int64_t unused);

static void* NewArrayOf(const void* arrayInfo, int64_t nElems, size_t elemBytes)
{
    if (nElems < 0) { cjw_unsupported("negative array length", CJW_RC_UNSUPPORTED); }
    if (elemBytes != 0 && (uint64_t)nElems > (SIZE_MAX - CJW_ARRAY_HEADER_SIZE) / elemBytes) {
        fprintf(stderr, "cangjie wasm32 runtime: array of %lld elements does not fit wasm32 memory\n", (long long)nElems);
        exit(CJW_RC_UNSUPPORTED);
    }
    uint8_t* obj = (uint8_t*)cjw_alloc(CJW_ARRAY_HEADER_SIZE + (size_t)nElems * elemBytes, 8);
    *(const void**)obj = arrayInfo;
    *(int64_t*)(obj + CJW_ARRAY_LENGTH_OFFSET) = nElems;
    return obj;
}

static int8_t TiKind(const void* ti) { return *(const int8_t*)((const uint8_t*)ti + CJW_TI_KIND_OFFSET); }
static uint32_t TiSize(const void* ti) { return *(const uint32_t*)((const uint8_t*)ti + CJW_TI_SIZE_OFFSET); }
static const void* TiSuper(const void* ti) { return *(const void* const*)((const uint8_t*)ti + CJW_TI_SUPER_OFFSET); }

// Element width of a RawArray TypeInfo: reference size for object kinds (all negative),
// otherwise the component size stored in the array TypeInfo (MClass.h union, MArray.inline.h).
static size_t ArrayElemBytes(const void* arrayInfo)
{
    const void* component = TiSuper(arrayInfo);
    if (component != NULL && TiKind(component) < 0) { return sizeof(void*); }
    return TiSize(arrayInfo);
}

void* CJ_MCC_NewArray16(const void* arrayInfo, int64_t nElems, int64_t unused) { (void)unused; return NewArrayOf(arrayInfo, nElems, 2); }
void* CJ_MCC_NewArray32(const void* arrayInfo, int64_t nElems, int64_t unused) { (void)unused; return NewArrayOf(arrayInfo, nElems, 4); }
void* CJ_MCC_NewArray64(const void* arrayInfo, int64_t nElems, int64_t unused) { (void)unused; return NewArrayOf(arrayInfo, nElems, 8); }
void* CJ_MCC_NewObjArray(const void* arrayInfo, int64_t nElems, int64_t unused) { (void)unused; return NewArrayOf(arrayInfo, nElems, sizeof(void*)); }
void* CJ_MCC_NewArray(const void* arrayInfo, int64_t nElems, int64_t unused) { (void)unused; return NewArrayOf(arrayInfo, nElems, ArrayElemBytes(arrayInfo)); }
void* CJ_MCC_NewArrayGeneric(const void* arrayInfo, int64_t nElems) { return NewArrayOf(arrayInfo, nElems, ArrayElemBytes(arrayInfo)); }

// size already includes the object header (MCC_NewObject DCHECK).
void* CJ_MCC_NewObject(const void* typeInfo, uint32_t size)
{
    uint8_t* obj = (uint8_t*)cjw_alloc(size < CJW_OBJECT_HEADER_SIZE ? CJW_OBJECT_HEADER_SIZE : size, 8);
    *(const void**)obj = typeInfo;
    return obj;
}
// The arena never moves objects, so pinning is the default.
void* CJ_MCC_NewPinnedObject(const void* typeInfo, uint32_t size) { return CJ_MCC_NewObject(typeInfo, size); }
// Finalizers are never run in M0 (no collector); registration keeps the object as is.
void* CJ_MCC_OnFinalizerCreated(void* obj) { return obj; }

// ---------------------------------------------------------------------------
// Heap barriers: the arena is non-moving and single threaded, so every barrier is
// the plain memory operation it guards.
void* CJ_MCC_ReadRefField(void* obj, void** field) { (void)obj; return *field; }
void CJ_MCC_WriteRefField(void* ref, void* obj, void** field) { (void)obj; *field = ref; }
void CJ_MCC_WriteRefField_Strong(void* ref, void* obj, void** field) { (void)obj; *field = ref; }
void* CJ_MCC_AtomicReadReference(void* obj, void** field, int32_t order) { (void)obj; (void)order; return *field; }
void CJ_MCC_AtomicWriteReference(void* ref, void* obj, void** field, int32_t order) { (void)obj; (void)order; *field = ref; }
void CJ_MCC_WriteStructField(void* obj, void* dst, size_t dstLen, const void* src, size_t srcLen, const void* gctib)
{
    (void)obj; (void)gctib;
    memmove(dst, src, srcLen < dstLen ? srcLen : dstLen);
}
void CJ_MCC_ReadStructField(void* dst, void* obj, const void* src, size_t size, const void* gctib)
{
    (void)obj; (void)gctib;
    memmove(dst, src, size);
}
// Generic (boxed) values: the payload follows the object header.
void CJ_MCC_WriteGeneric(void* obj, void* fieldPtr, const void* src, size_t size)
{
    (void)obj;
    if (size != 0) { memmove(fieldPtr, (const uint8_t*)src + CJW_OBJECT_HEADER_SIZE, size); }
}
void CJ_MCC_ReadGeneric(void* dst, void* obj, const void* fieldPtr, size_t size)
{
    (void)obj;
    if (size != 0) { memmove((uint8_t*)dst + CJW_OBJECT_HEADER_SIZE, fieldPtr, size); }
}
void CJ_MCC_AssignGeneric(void* dst, const void* src, const void* typeInfo)
{
    if (typeInfo == NULL) { return; }
    size_t n = TiSize(typeInfo);
    if (n != 0) { memmove((uint8_t*)dst + CJW_OBJECT_HEADER_SIZE, (const uint8_t*)src + CJW_OBJECT_HEADER_SIZE, n); }
}
void CJ_MCC_ArrayCopyGeneric(void* dstObj, void* dstField, size_t dstSize, void* srcObj, const void* srcField, size_t srcSize)
{
    (void)dstObj; (void)srcObj;
    memmove(dstField, srcField, srcSize < dstSize ? srcSize : dstSize);
}
// Raw data of primitive arrays is addressed in place (no copy, nothing to unpin).
void* CJ_MCC_AcquireRawData(void* array, uint8_t* isCopy)
{
    if (isCopy != NULL) { *isCopy = 0; }
    return (uint8_t*)array + CJW_ARRAY_HEADER_SIZE;
}
void CJ_MCC_ReleaseRawData(void* array, void* rawPtr, int32_t unused) { (void)array; (void)rawPtr; (void)unused; }

// ---------------------------------------------------------------------------
// Type queries. Class inheritance is answered from the TypeInfo super chain;
// interface conformance and runtime generic instantiation need the MTable /
// TypeTemplate machinery of the native runtime and are explicit gaps (M1).
uint8_t CJ_MCC_IsSubType(const void* typeInfo, const void* superTypeInfo)
{
    if (typeInfo == superTypeInfo) { return 1; }
    if (superTypeInfo == NULL || typeInfo == NULL) { return 0; }
    if (TiKind(superTypeInfo) == CJW_KIND_CLASS) {
        for (const void* t = typeInfo; t != NULL; t = TiKind(t) == CJW_KIND_CLASS ? TiSuper(t) : NULL) {
            if (t == superTypeInfo) { return 1; }
        }
        return 0;
    }
    cjw_unsupported("interface or generic subtype query (CJ_MCC_IsSubType)", CJW_RC_TYPEQUERY);
}
void* CJ_MCC_GetOrCreateTypeInfo(const void* typeTemplate, uint32_t argSize, void** typeArgs)
{
    (void)typeTemplate; (void)argSize; (void)typeArgs;
    cjw_unsupported("runtime generic type instantiation (CJ_MCC_GetOrCreateTypeInfo)", CJW_RC_TYPEQUERY);
}
void* CJ_MCC_GetMethodOuterTI(const void* ti, const void* itf, uint64_t index)
{
    (void)ti; (void)itf; (void)index;
    cjw_unsupported("interface method dispatch lookup (CJ_MCC_GetMethodOuterTI)", CJW_RC_TYPEQUERY);
}
void CJ_MCC_UpdateVMT(void* ti, void* itf, void* extensionData)
{
    (void)ti; (void)itf; (void)extensionData;
    cjw_unsupported("runtime virtual-method-table update (CJ_MCC_UpdateVMT)", CJW_RC_TYPEQUERY);
}

// ---------------------------------------------------------------------------
// Threads and monitors: one wasm instance runs one Cangjie thread. Locks can never
// be contended; waiting would block forever and is reported instead.
uint8_t CJ_MCC_MutexInit(void* mutex) { (void)mutex; return 1; }
void CJ_MCC_MutexLock(void* mutex) { (void)mutex; }
void CJ_MCC_MutexUnlock(void* mutex) { (void)mutex; }
uint8_t CJ_MCC_WaitQueueForMonitorInit(void* monitor) { (void)monitor; return 1; }
uint8_t CJ_MCC_MonitorWait(void* monitor, int64_t timeoutNs)
{
    (void)monitor; (void)timeoutNs;
    cjw_unsupported("waiting on a monitor in a single-threaded instance", CJW_RC_THREADS);
}

// ---------------------------------------------------------------------------
// Reference processing / finalization: no collector, so nothing is ever pending.
void CJ_MCC_SetReferenceMethods(void* a, void* b, void* c) { (void)a; (void)b; (void)c; }
void CJ_MCC_RegisterImplicitExceptionRaisers(void* raisers) { (void)raisers; }
void CJ_MCC_ReferenceInvokeFinalize(void* obj) { (void)obj; }
uint8_t CJ_MCC_ReferenceRuntimeRunning(void) { return 0; }
void* CJ_MCC_ReferenceWaitPending(void) { return NULL; }
int64_t CJ_MCC_ReferenceNanoTime(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Stack traces: no unwinder/stackmaps on wasm32 yet. Exceptions carry no trace.
void* CJ_MCC_FillInStackTrace(void* typeInfo, void* exception) { (void)typeInfo; (void)exception; return NULL; }
void CJ_MCC_DecodeStackTrace(void* out, int64_t pc, int64_t fa, int64_t id, void* ti)
{
    (void)out; (void)pc; (void)fa; (void)id; (void)ti;
    cjw_unsupported("decoding stack traces", CJW_RC_UNSUPPORTED);
}

// ---------------------------------------------------------------------------
// compiler-rt builtin: wasi-sysroot ships no libclang_rt.builtins, and the wasm
// backend lowers 128-bit multiplication (overflow checks) to __multi3.
typedef struct { uint64_t lo; uint64_t hi; } CjwU128;
CjwU128 __multi3_impl(uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi) __asm__("__multi3");
CjwU128 __multi3_impl(uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi)
{
    uint64_t a0 = alo & 0xffffffffu, a1 = alo >> 32, b0 = blo & 0xffffffffu, b1 = blo >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    CjwU128 r;
    r.lo = (mid << 32) | (p00 & 0xffffffffu);
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32) + alo * bhi + ahi * blo;
    return r;
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
