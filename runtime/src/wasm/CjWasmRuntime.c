// Cangjie runtime entry points for wasm32-wasi (M0 subset). See CjWasmRuntime.h.
#define _POSIX_C_SOURCE 200809L // clock_gettime under -std=c11
#include "CjWasmRuntime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stddef.h>
#include <wasi/api.h>

// ---------------------------------------------------------------------------
// Unsupported features
_Noreturn void cjw_unsupported(const char* feature, int code)
{
    fprintf(stderr, "cangjie wasm32 runtime: %s is not supported on the wasm32-wasi target yet\n", feature);
    fflush(stderr);
    // Diagnostics: trap instead of exiting so the host prints the wasm call stack.
    const char* trap = getenv("CJ_WASM_TRAP_ON_UNSUPPORTED");
    if (trap != NULL && trap[0] == '1') { __builtin_trap(); }
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

// Element width of a RawArray TypeInfo, as MArray::GetElementSize / MArray::NewArray: the reference
// size for reference components (all negative kinds), otherwise the component TypeInfo's own size
// (TypeInfo::GetComponentSize of the component). Runtime-instantiated RawArray<T> TypeInfos carry the
// pointer size as their instance size (TypeInfoManager), so the array TypeInfo size is not the width.
static size_t ArrayElemBytes(const void* arrayInfo)
{
    const void* component = TiSuper(arrayInfo);
    if (component == NULL) { cjw_unsupported("array TypeInfo without a component TypeInfo", CJW_RC_TYPEQUERY); }
    if (TiKind(component) < 0) { return sizeof(void*); }
    return TiSize(component);
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
    if (src != NULL && size != 0) { memmove(fieldPtr, (const uint8_t*)src + CJW_OBJECT_HEADER_SIZE, size); }
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
// Unsplit struct barriers on 32-bit targets (CJBarrierLowering: dst, size, src, size, gctib) and
// array copies (dstObj, dstField, dstSize, srcObj, srcField, srcSize): plain bounded copies.
void CJ_MCC_ReadStaticStruct(void* dst, size_t dstSize, const void* src, size_t srcSize, const void* gctib)
{
    (void)gctib;
    memmove(dst, src, srcSize < dstSize ? srcSize : dstSize);
}
void CJ_MCC_WriteStaticStruct(void* dst, size_t dstSize, const void* src, size_t srcSize, const void* gctib)
{
    (void)gctib;
    memmove(dst, src, srcSize < dstSize ? srcSize : dstSize);
}
void CJ_MCC_ArrayCopyRef(void* dstObj, void* dstField, size_t dstSize, void* srcObj, const void* srcField, size_t srcSize)
{
    (void)dstObj; (void)srcObj;
    memmove(dstField, srcField, srcSize < dstSize ? srcSize : dstSize);
}
void CJ_MCC_ArrayCopyStruct(void* dstObj, void* dstField, size_t dstSize, void* srcObj, const void* srcField, size_t srcSize)
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
// TypeInfo / TypeTemplate as emitted by cjcj for 32-bit pointers (%TypeInfo,
// %TypeTemplate; stdlib typetemplate.c structs).
typedef struct CjwTI CjwTI;
typedef CjwTI* (*CjwGenericFn)(int32_t argc, CjwTI** args);
typedef struct {
    const char* name; int8_t kind; uint8_t flag; uint16_t fieldsNum; uint16_t typeArgsNum;
    CjwGenericFn* fieldsFns; CjwGenericFn superFn; void* finalizer; void* reflection;
    void** extensionDefPtr; uint16_t inheritedClassNum;
} CjwTT;
struct CjwTI {
    const char* name; int8_t kind; uint8_t flag; uint16_t fieldsNum; uint32_t size; const void* gctib;
    uint32_t uuid; uint8_t align; uint8_t typeArgNum; uint16_t inheritedClassNum; uint32_t* offsets;
    const void* sourceGeneric; CjwTI** typeArgs; CjwTI** fields; CjwTI* super; void** extensionDefPtr;
    void* mtable; void* reflection;
};
_Static_assert(sizeof(void*) == 4, "wasm32 layout");
_Static_assert(offsetof(CjwTI, size) == CJW_TI_SIZE_OFFSET, "TypeInfo.size offset");
_Static_assert(offsetof(CjwTI, super) == CJW_TI_SUPER_OFFSET, "TypeInfo.super offset");
_Static_assert(sizeof(CjwTI) == 56, "TypeInfo size");
_Static_assert(offsetof(CjwTT, fieldsFns) == 12 && sizeof(CjwTT) == 36, "TypeTemplate layout");

enum {
    K_CLASS = -128, K_INTERFACE = -127, K_RAWARRAY = -126, K_FUNC = -125, K_TEMP_ENUM = -124,
    K_WEAKREF_CLASS = -123, K_FOREIGN_PROXY = -122, K_EXPORTED_REF = -121, K_FINALREF_CLASS = -120,
    K_NOTHING = 0, K_UNIT = 1, K_CSTRING = 17, K_CPOINTER = 18, K_CFUNC = 19, K_VARRAY = 20, K_TUPLE = 21,
    K_STRUCT = 22, K_ENUM = 23, K_MAX = 24
};
#define CJW_FLAG_HAS_REF_FIELD 0x1u
#define CJW_SYNC_FLAGS ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 5)) // future, mutex, monitor, wait queue
#define CJW_SYNC_OBJECT_SIZE 168u // runtime/src/Sync/Sync.h SYNC_OBJECT_SIZE

static int IsClassLike(int8_t k)
{
    return k == K_CLASS || k == K_WEAKREF_CLASS || k == K_FINALREF_CLASS || k == K_FOREIGN_PROXY || k == K_EXPORTED_REF;
}
static uint32_t AlignUp(uint32_t v, uint32_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }

// Two TypeInfos denote the same type if they are the same object or instantiate the same
// TypeTemplate with equal arguments. wasm32 has no static-generic-TI registry, so a runtime
// instantiation and a compiler-emitted one of the same type can be distinct objects.
static int SameType(const CjwTI* a, const CjwTI* b)
{
    if (a == b) { return 1; }
    if (a == NULL || b == NULL || a->typeArgNum == 0 || a->typeArgNum != b->typeArgNum ||
        a->sourceGeneric == NULL || a->sourceGeneric != b->sourceGeneric) { return 0; }
    for (uint32_t i = 0; i < a->typeArgNum; ++i) {
        if (!SameType(a->typeArgs[i], b->typeArgs[i])) { return 0; }
    }
    return 1;
}

// TypeInfoManager::GetTypeSize
static uint32_t TypeSize(const CjwTI* ti)
{
    int8_t k = ti->kind;
    if (k == K_NOTHING || k == K_UNIT) { return 0; }
    if (k < 0) {
        if (k == -1 || k == -2) { cjw_unsupported("size of an unresolved generic TypeInfo", CJW_RC_TYPEQUERY); }
        return sizeof(void*);
    }
    if (k == K_ENUM && ti->fieldsNum == 0) { return ti->size; }
    if (k == K_ENUM || k == K_STRUCT || k == K_TUPLE) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < ti->fieldsNum; ++i) {
            const CjwTI* f = ti->fields[i];
            if (f->kind == K_NOTHING || f->kind == K_UNIT) { continue; }
            total = AlignUp(total, f->kind < 0 ? sizeof(void*) : f->align);
            total += TypeSize(f);
        }
        return AlignUp(total, ti->align);
    }
    if (k == K_VARRAY) { // fieldsNum elements of the component TypeInfo (union with super)
        const CjwTI* c = ti->super;
        if (c == NULL) { cjw_unsupported("VArray TypeInfo without a component", CJW_RC_TYPEQUERY); }
        uint32_t a = c->align, total = 0, csize = TypeSize(c);
        for (uint32_t i = 0; i < ti->fieldsNum; ++i) { total = AlignUp(total, a) + csize; }
        return AlignUp(total, a);
    }
    if (k >= K_MAX) {
        fprintf(stderr, "cangjie wasm32 runtime: TypeInfo %s has kind %d\n", ti->name ? ti->name : "?", (int)k);
        cjw_unsupported("size of a VArray/unknown TypeInfo kind", CJW_RC_TYPEQUERY);
    }
    return ti->size; // primitives, CString, CPointer, CFunc
}

// Cache of runtime instantiations, keyed by (template, args).
typedef struct { const CjwTT* tt; uint32_t argc; CjwTI** args; CjwTI* ti; } CjwInst;
static CjwInst* g_insts = NULL;
static uint32_t g_instCount = 0, g_instCap = 0;

static CjwTI* FindInst(const CjwTT* tt, uint32_t argc, CjwTI** args)
{
    for (uint32_t i = 0; i < g_instCount; ++i) {
        CjwInst* e = &g_insts[i];
        if (e->tt != tt || e->argc != argc) { continue; }
        uint32_t j = 0;
        while (j < argc && SameType(e->args[j], args[j])) { ++j; }
        if (j == argc) { return e->ti; }
    }
    return NULL;
}

static char* InstName(const CjwTT* tt, uint32_t argc, CjwTI** args)
{
    size_t n = strlen(tt->name) + 3;
    for (uint32_t i = 0; i < argc; ++i) { n += strlen(args[i]->name) + 1; }
    char* s = (char*)malloc(n);
    if (s == NULL) { cjw_unsupported("memory for a TypeInfo name", CJW_RC_UNSUPPORTED); }
    strcpy(s, tt->name);
    strcat(s, "<");
    for (uint32_t i = 0; i < argc; ++i) { if (i) { strcat(s, ","); } strcat(s, args[i]->name); }
    strcat(s, ">");
    return s;
}

// Port of TypeInfoManager::CreatedTypeInfoImpl + FillRemainingField + FillOffsets
// (runtime/src/TypeInfoManager.cpp:870-1300). Not ported: GC tib (no collector),
// interface MTable (AddMTable: interface dispatch traps in GetMTableAndOuterTI) and
// reflection/enum debug info (reflection is disabled for wasm32).
void* CJ_MCC_GetOrCreateTypeInfo(const void* typeTemplate, uint32_t argSize, void** typeArgs)
{
    const CjwTT* tt = (const CjwTT*)typeTemplate;
    CjwTI** args = (CjwTI**)typeArgs;
    CjwTI* found = FindInst(tt, argSize, args);
    if (found != NULL) { return found; } // also the in-progress TypeInfo for a recursive request
    if (tt->kind == K_VARRAY) { cjw_unsupported("instantiating a VArray TypeInfo", CJW_RC_TYPEQUERY); }

    CjwTI* ti = (CjwTI*)calloc(1, sizeof(CjwTI));
    CjwTI** argCopy = (CjwTI**)calloc(argSize ? argSize : 1, sizeof(CjwTI*));
    if (ti == NULL || argCopy == NULL) { cjw_unsupported("memory for a TypeInfo", CJW_RC_UNSUPPORTED); }
    memcpy(argCopy, args, argSize * sizeof(CjwTI*));
    ti->name = InstName(tt, argSize, args);
    ti->kind = tt->kind;
    ti->flag = tt->flag;
    ti->inheritedClassNum = tt->inheritedClassNum;
    ti->extensionDefPtr = tt->extensionDefPtr;
    ti->sourceGeneric = tt;
    ti->typeArgs = argCopy;
    if (g_instCount == g_instCap) {
        g_instCap = g_instCap ? g_instCap * 2 : 64;
        g_insts = (CjwInst*)realloc(g_insts, g_instCap * sizeof(CjwInst));
        if (g_insts == NULL) { cjw_unsupported("memory for the TypeInfo cache", CJW_RC_UNSUPPORTED); }
    }
    g_insts[g_instCount++] = (CjwInst){ tt, argSize, argCopy, ti };

    uint32_t fieldNum = tt->fieldsNum, typeArgNum = tt->typeArgsNum;
    if (tt->kind == K_CFUNC) {
        typeArgNum = argSize;
    } else if (tt->kind == K_TUPLE || tt->kind == K_FUNC || tt->kind == K_RAWARRAY) {
        fieldNum = argSize;
        typeArgNum = argSize;
    }
    ti->fieldsNum = (uint16_t)fieldNum;
    ti->typeArgNum = (uint8_t)typeArgNum;
    if (tt->kind == K_RAWARRAY || tt->kind == K_CPOINTER) {
        ti->super = args[0]; // component TypeInfo (union with super)
        ti->size = sizeof(void*);
        ti->align = sizeof(void*);
        return ti;
    }
    if (tt->kind == K_CFUNC) {
        ti->size = sizeof(void*);
        ti->align = sizeof(void*);
        return ti;
    }
    ti->offsets = (uint32_t*)calloc(fieldNum ? fieldNum : 1, sizeof(uint32_t));
    ti->fields = (CjwTI**)calloc(fieldNum ? fieldNum : 1, sizeof(CjwTI*));
    if (ti->offsets == NULL || ti->fields == NULL) { cjw_unsupported("memory for TypeInfo fields", CJW_RC_UNSUPPORTED); }

    uint32_t offset = 0, align = 1, hasRef = 0;
    int fromArgs = tt->kind == K_TUPLE || tt->kind == K_FUNC;
    for (uint32_t f = 0; f < fieldNum; ++f) {
        CjwTI* fti = fromArgs ? args[f] : tt->fieldsFns[f]((int32_t)argSize, args);
        ti->fields[f] = fti;
        if (fti->kind == K_NOTHING || fti->kind == K_UNIT) { continue; }
        if (fti->kind < 0 || (fti->flag & CJW_FLAG_HAS_REF_FIELD)) { hasRef = 1; }
        uint32_t fa = fti->kind < 0 ? sizeof(void*) : fti->align;
        offset = AlignUp(offset, fa);
        ti->offsets[f] = offset;
        if (fa > align) { align = fa; }
        offset += TypeSize(fti);
    }
    if (hasRef) { ti->flag |= CJW_FLAG_HAS_REF_FIELD; }
    ti->align = (uint8_t)(ti->kind < 0 ? sizeof(void*) : align);
    uint32_t instanceSize = AlignUp(offset, align);
    ti->size = (ti->flag & CJW_SYNC_FLAGS) ? CJW_SYNC_OBJECT_SIZE - 8u : instanceSize;
    ti->super = tt->superFn != NULL ? tt->superFn((int32_t)argSize, args) : NULL;
    return ti;
}

static int ImplementsInterface(const CjwTI* t, const CjwTI* itf); // interface dispatch section below

// TypeInfo::IsSubType: class inheritance from the super chain; interface conformance from the
// ExtensionData tables (FindExtensionData), Any always; function and tuple subtyping still trap.
uint8_t CJ_MCC_IsSubType(const void* typeInfo, const void* superTypeInfo)
{
    const CjwTI* t = (const CjwTI*)typeInfo;
    const CjwTI* sup = (const CjwTI*)superTypeInfo;
    if (SameType(t, sup)) { return 1; }
    if (t == NULL || sup == NULL) { return 0; }
    if (IsClassLike(sup->kind)) {
        if (!IsClassLike(t->kind)) {
            if (t->kind == K_INTERFACE || t->kind == -1 || t->kind == -2 || t->kind >= K_MAX) {
                cjw_unsupported("subtype query from a non-concrete TypeInfo (CJ_MCC_IsSubType)", CJW_RC_TYPEQUERY);
            }
            return 0; // values, arrays, functions and enums never extend a class
        }
        for (const CjwTI* c = t->super; c != NULL; c = IsClassLike(c->kind) ? c->super : NULL) {
            if (SameType(c, sup)) { return 1; }
        }
        return 0;
    }
    if (sup->kind >= 0 && sup->kind < K_MAX && sup->kind != K_TUPLE && sup->kind != K_VARRAY) {
        return 0; // value types have no proper subtypes (tuples: CJ_MCC_IsTupleTypeOf)
    }
    if (sup->kind == K_INTERFACE) {
        if (t->kind == K_TEMP_ENUM && t->super != NULL) { return CJ_MCC_IsSubType(t->super, sup); }
        if (sup->name != NULL && strcmp(sup->name, "std.core:Any") == 0) { return 1; }
        if (t->kind == -1 || t->kind == -2 || t->kind >= K_MAX) {
            cjw_unsupported("subtype query from a non-concrete TypeInfo (CJ_MCC_IsSubType)", CJW_RC_TYPEQUERY);
        }
        if (ImplementsInterface(t, sup)) { return 1; }
        // Native also consults the outer extension defs (`extend T <: I` in other TypeInfos' packages, e.g.
        // std.core's `extend Int64 <: ToString`) through the loader registry, which wasm32 does not build
        // yet. "Not found in the type's own tables" is therefore not a proof of `false`: trap explicitly.
        cjw_unsupported("interface subtype query that needs outer extension defs (CJ_MCC_IsSubType)", CJW_RC_TYPEQUERY);
    }
    cjw_unsupported("function or tuple subtype query (CJ_MCC_IsSubType)", CJW_RC_TYPEQUERY);
}

// ---------------------------------------------------------------------------
// Interface dispatch, M1 subset: a direct C port of the non-generic path of TypeInfo::GetMTable /
// FindExtensionData / ResolveInnerExtensionDefs / TryUpdateExtensionData (ObjectModel/MClass.cpp,
// ExtensionData.h). Each TypeInfo's vExtensionDataStart lists its class-chain ExtensionDatas
// ([0, validInheritNum)) followed by the ExtensionDatas whose target is the type itself; the super
// chain is searched next. Generic targets, interfaceFn-computed interfaces, where-conditions and
// incremental (default-method) function tables are explicit traps until they are ported.
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t argNum; uint8_t isInterfaceTypeInfo; uint8_t flag; uint16_t funcTableSize;
    const void* target; const void* interface; void* whereCondFn; void** funcTable;
} CjwExtData;
_Static_assert(sizeof(CjwExtData) == 8 + 4 * sizeof(void*), "ExtensionData layout (ATTR_PACKED(4))");
#define CJW_ED_FT_UPDATED 0x06u // ExtensionData flag bits 1&2 = 11: funcTable updated

// ExtensionData type arguments of `resolveTi` (ResolveExtensionData): the component for RawArray /
// CPointer, otherwise the TypeInfo's own type arguments.
static uint32_t EdTypeArgs(const CjwTI* resolveTi, CjwTI*** args)
{
    if (resolveTi->kind == K_RAWARRAY || resolveTi->kind == K_CPOINTER) {
        *args = (CjwTI**)&((CjwTI*)resolveTi)->super;
        return 1;
    }
    *args = resolveTi->typeArgs;
    return resolveTi->typeArgNum;
}

// ResolveExtensionData(needCheckStop): does `ed` (past the class-chain prefix) still belong to `cur`?
static int EdTargetsType(const CjwExtData* ed, const CjwTI* cur)
{
    if (ed->argNum == 0) { return ed->target == (const void*)cur || SameType((const CjwTI*)ed->target, cur); }
    return cur->sourceGeneric != NULL && ed->target == cur->sourceGeneric;
}

// TypeInfo::GetValidInheritNum: bit 15 of validInheritNum is a flag, the count is the low 15 bits.
static uint32_t ValidInheritNum(const CjwTI* ti) { return ti->inheritedClassNum & 0x7FFFu; }

// A TypeInfo's ExtensionData list and class-chain count. Compiler-emitted static instances of generic
// types (linkonce_odr `Foo<Int64>.ti`) carry no list of their own; theirs is the TypeTemplate's, as the
// native outer traversal resolves it through sourceGeneric. sourceGeneric is only meaningful for
// generic instances (it shares storage with the finalizer of non-generic classes).
static CjwExtData** ExtDataList(const CjwTI* ti, uint32_t* classChain)
{
    if (ti->extensionDefPtr != NULL) { *classChain = ValidInheritNum(ti); return (CjwExtData**)ti->extensionDefPtr; }
    if (ti->typeArgNum != 0 && ti->sourceGeneric != NULL) {
        const CjwTT* tt = (const CjwTT*)ti->sourceGeneric;
        *classChain = tt->inheritedClassNum & 0x7FFFu;
        return (CjwExtData**)tt->extensionDefPtr;
    }
    *classChain = 0;
    return NULL;
}

static CjwExtData* FindExtData(const CjwTI* ti, const CjwTI* itf)
{
    for (const CjwTI* cur = ti; cur != NULL; cur = IsClassLike(cur->kind) ? cur->super : NULL) {
        uint32_t classChain = 0;
        CjwExtData** v = ExtDataList(cur, &classChain);
        if (v == NULL) { continue; }
        CjwTI** args = NULL;
        uint32_t argc = EdTypeArgs(cur, &args);
        for (uint32_t i = 0;; ++i) {
            CjwExtData* ed = v[i];
            if (ed == NULL) { break; }
            if (i >= classChain && !EdTargetsType(ed, cur)) { break; }
            if (ed->whereCondFn != NULL &&
                (((uintptr_t(*)(int32_t, CjwTI**))ed->whereCondFn)((int32_t)argc, args) & 0x1u) == 0) {
                continue;
            }
            const CjwTI* edItf = ed->isInterfaceTypeInfo ? (const CjwTI*)ed->interface
                                                         : ((CjwGenericFn)ed->interface)((int32_t)argc, args);
            if (edItf == itf || SameType(edItf, itf)) { return ed; }
        }
        if (cur->kind == K_RAWARRAY || cur->kind == K_CPOINTER) { break; }
    }
    return NULL;
}

static int ImplementsInterface(const CjwTI* t, const CjwTI* itf) { return FindExtData(t, itf) != NULL; }

static void TryUpdateExtData(const CjwTI* ti, const CjwTI* itf, CjwExtData* ed)
{
    if ((ed->flag & CJW_ED_FT_UPDATED) == CJW_ED_FT_UPDATED || ti == itf) { return; }
    uint32_t itfChain = 0;
    CjwExtData** iv = ExtDataList(itf, &itfChain);
    if (iv == NULL) { cjw_unsupported("interface TypeInfo without extension data", CJW_RC_TYPEQUERY); }
    const CjwExtData* itfEd = itf->kind == K_INTERFACE ? iv[0] : iv[itfChain - 1];
    if (itfEd->funcTableSize == ed->funcTableSize) {
        ed->flag |= CJW_ED_FT_UPDATED;
        return;
    }
    cjw_unsupported("incremental interface function table (default methods from a super type)", CJW_RC_TYPEQUERY);
}

void CJ_MCC_UpdateVMT(void* ti, void* itf, void* extensionData)
{
    TryUpdateExtData((const CjwTI*)ti, (const CjwTI*)itf, (CjwExtData*)extensionData);
}

void* CJ_MCC_GetMTable(const void* ti, const void* itf)
{
    const CjwTI* t = (const CjwTI*)ti;
    if (t->kind == K_TEMP_ENUM && t->super != NULL) { t = t->super; }
    CjwExtData* ed = FindExtData(t, (const CjwTI*)itf);
    if (ed == NULL) { cjw_unsupported("interface method table lookup: no extension data (CJ_MCC_GetMTable)", CJW_RC_TYPEQUERY); }
    TryUpdateExtData(t, (const CjwTI*)itf, ed);
    return ed->funcTable;
}

// TypeInfo::GetMethodOuterTI, non-generic path: the TypeInfo that defines method `index` of `itf`
// for `ti`. With the codegen fast path (flag bit 0) the concrete outer TypeInfo follows the function
// table (OuterTiUnion array); otherwise walk the supers for the same function pointer, else `ti`.
void* CJ_MCC_GetMethodOuterTI(const void* ti, const void* itf, uint64_t index)
{
    const CjwTI* t = (const CjwTI*)ti;
    if (ti == itf) { return (void*)ti; }
    if (t->kind == K_TEMP_ENUM && t->super != NULL) { return CJ_MCC_GetMethodOuterTI(t->super, itf, index); }
    CjwExtData* ed = FindExtData(t, (const CjwTI*)itf);
    if (ed == NULL) { cjw_unsupported("interface method outer TypeInfo: no extension data (CJ_MCC_GetMethodOuterTI)", CJW_RC_TYPEQUERY); }
    TryUpdateExtData(t, (const CjwTI*)itf, ed);
    if (index >= ed->funcTableSize) { cjw_unsupported("interface method index out of the function table", CJW_RC_TYPEQUERY); }
    if (ed->flag & 0x1u) { // ExtensionData::GetOuterTi
        void* const* outer = (void* const*)(ed->funcTable + ed->funcTableSize);
        if (t->typeArgNum == 0) {
            if (outer[index] != NULL) { return outer[index]; }
        } else if (outer[index] != NULL) {
            if (!EdTargetsType(ed, t)) { cjw_unsupported("generic outer TypeInfo via a super type (CJ_MCC_GetMethodOuterTI)", CJW_RC_TYPEQUERY); }
            return ((CjwTI*(*)(const CjwTI*))outer[index])(t);
        }
    }
    void* fn = ed->funcTable[index];
    for (const CjwTI* s = IsClassLike(t->kind) ? t->super : NULL; s != NULL; s = IsClassLike(s->kind) ? s->super : NULL) {
        CjwExtData* sed = FindExtData(s, (const CjwTI*)itf);
        if (sed != NULL && index < sed->funcTableSize && sed->funcTable[index] == fn) {
            return CJ_MCC_GetMethodOuterTI(s, itf, index);
        }
    }
    return (void*)ti;
}

// TypeInfo::GetMTableAndOuterTI: outside the native stable-table fast path it is GetMTable followed by
// GetMethodOuterTI.
void* CJ_MCC_GetMTableAndOuterTI(const void* ti, const void* itf, uint64_t index, void** outerTI)
{
    void* funcTable = CJ_MCC_GetMTable(ti, itf);
    *outerTI = CJ_MCC_GetMethodOuterTI(ti, itf, index);
    return funcTable;
}


uint8_t CJ_MCC_IsTupleTypeOf(const void* obj, const void* typeInfo, const void* targetTypeInfo)
{
    (void)typeInfo;
    if (obj == NULL || targetTypeInfo == NULL) { return 0; }
    cjw_unsupported("tuple type test (CJ_MCC_IsTupleTypeOf)", CJW_RC_TYPEQUERY);
}

// ---------------------------------------------------------------------------
// Threads and monitors: one wasm instance runs one Cangjie thread. Locks can never
// be contended; waiting would block forever and is reported instead.
// Sync.cpp MCC_MutexInit / MCC_WaitQueueForMonitorInit: int status, 0 = success.
int32_t CJ_MCC_MutexInit(void* mutex) { (void)mutex; return 0; }
void CJ_MCC_MutexLock(void* mutex) { (void)mutex; }
void CJ_MCC_MutexUnlock(void* mutex) { (void)mutex; }
int32_t CJ_MCC_WaitQueueForMonitorInit(void* monitor) { (void)monitor; return 0; }
// Cangjie threads (spawn) need wasm threads or a cooperative scheduler (M1+).
void* CJ_MCC_NewCJThread(void* entry, void* closure, void* typeInfo)
{
    (void)entry; (void)closure; (void)typeInfo;
    cjw_unsupported("spawning a Cangjie thread", CJW_RC_THREADS);
}
void CJ_MCC_FutureInit(void* future) { (void)future; } // Sync.cpp MCC_FutureInit: no runtime state here
void CJ_MCC_FutureNotifyAll(void* future) { (void)future; }
uint8_t CJ_MCC_FutureIsComplete(const void* future)
{
    (void)future;
    cjw_unsupported("querying a Future (no spawned threads)", CJW_RC_THREADS);
}
void CJ_MCC_FutureWait(void* future, int64_t timeoutNs)
{
    (void)future; (void)timeoutNs;
    cjw_unsupported("waiting on a Future (no spawned threads)", CJW_RC_THREADS);
}
static void* g_currentThreadObject = NULL;
void CJ_MCC_SetCurrentCJThreadObject(void* thread) { g_currentThreadObject = thread; }
int64_t CJ_MRT_GetCJThreadId(void* handle) { (void)handle; return 1; } // the only Cangjie thread
void CJ_MRT_Sleep(int64_t ns)
{
    if (ns <= 0) { return; }
    // One relative CLOCK_MONOTONIC subscription, as wasi-libc clock_nanosleep does.
    __wasi_subscription_t sub;
    memset(&sub, 0, sizeof sub);
    sub.u.tag = __WASI_EVENTTYPE_CLOCK;
    sub.u.u.clock.id = __WASI_CLOCKID_MONOTONIC;
    sub.u.u.clock.timeout = (__wasi_timestamp_t)ns;
    __wasi_event_t ev;
    __wasi_size_t n = 0;
    (void)__wasi_poll_oneoff(&sub, &ev, 1, &n);
}

// Nobody else can be waiting, so notification has no one to wake.
void CJ_MCC_MonitorNotify(void* monitor) { (void)monitor; }
void CJ_MCC_MonitorNotifyAll(void* monitor) { (void)monitor; }
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
void* CJ_MCC_ReferenceWaitPending(void) { cjw_unsupported("waiting for reference processing (no collector thread)", CJW_RC_THREADS); }
int64_t CJ_MCC_ReferenceNanoTime(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Stack traces: no unwinder/stackmaps on wasm32 yet. Exceptions carry no trace.
// The trace is an empty array of the requested array TypeInfo (never NULL).
void* CJ_MCC_FillInStackTrace(void* arrayTypeInfo, void* exception)
{
    (void)exception;
    if (arrayTypeInfo == NULL) { cjw_unsupported("stack traces without an array TypeInfo", CJW_RC_UNSUPPORTED); }
    return NewArrayOf(arrayTypeInfo, 0, 0);
}
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

// IEEE-754 binary16 <-> binary32 (Float16), compiler-rt __extendhfsf2/__truncsfhf2 ABI:
// the half value travels in the low 16 bits of an i32.
float cjw_extendhfsf2(uint32_t h) __asm__("__extendhfsf2");
float cjw_extendhfsf2(uint32_t h)
{
    uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1fu, mant = h & 0x3ffu, bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign; }
        else { // subnormal half -> normal float
            int e = -1;
            do { ++e; mant <<= 1; } while ((mant & 0x400u) == 0);
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((mant & 0x3ffu) << 13);
        }
    } else if (exp == 0x1fu) {
        bits = sign | 0x7f800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
uint32_t cjw_truncsfhf2(float f) __asm__("__truncsfhf2");
uint32_t cjw_truncsfhf2(float f)
{
    uint32_t x;
    memcpy(&x, &f, sizeof x);
    uint32_t sign = (x >> 16) & 0x8000u, absx = x & 0x7fffffffu;
    if (absx >= 0x7f800000u) { // Inf or NaN (keep NaN quiet and non-zero)
        return sign | 0x7c00u | (absx > 0x7f800000u ? (0x200u | ((absx >> 13) & 0x3ffu)) : 0u);
    }
    if (absx >= 0x477ff000u) { return sign | 0x7c00u; } // rounds to >= 65520 -> Inf
    if (absx < 0x38800000u) { // result is subnormal or zero
        if (absx < 0x33000000u) { return sign; }
        uint32_t m = (absx & 0x7fffffu) | 0x800000u;
        uint32_t shift = 126u - (absx >> 23); // 14..24
        uint32_t half = m >> shift, rem = m & ((1u << shift) - 1u), mid = 1u << (shift - 1u);
        if (rem > mid || (rem == mid && (half & 1u))) { ++half; }
        return sign | half;
    }
    uint32_t half = ((absx >> 13) - ((127u - 15u) << 10));
    uint32_t rem = absx & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) { ++half; }
    return sign | half;
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
