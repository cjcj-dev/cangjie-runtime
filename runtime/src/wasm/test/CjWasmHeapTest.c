// Unit tests for the wasm32-wasi heap, root registry and mark-sweep collector
// (CjWasmHeap.c), linked without the Cangjie entry chain of CjWasmRuntime.c.
//   heap_test.wasm            runs every positive test, exit status = failures
//   heap_test.wasm <mode>     runs one negative/gate mode (see run_heap_tests.sh)
// TypeInfos and GC tibs are built here with the wasm32 layout of CjWasmObjectModel.h.
#include "../CjWasmHeap.h"
#include "../CjWasmObjectModel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Noreturn void cjw_unsupported(const char* feature, int code)
{
    fprintf(stderr, "cangjie wasm32 runtime: %s is not supported on the wasm32-wasi target yet\n", feature);
    fflush(stderr);
    exit(code);
}

typedef struct { _Alignas(8) uint8_t b[56]; } TestTI;
typedef struct { uint32_t nBytes; uint8_t bits[8]; } TestTib;

static void TiInit(TestTI* t, int8_t kind, uint8_t flag, uint32_t size, const void* gctib, const void* super)
{
    memset(t, 0, sizeof(*t));
    *(int8_t*)(t->b + CJW_TI_KIND_OFFSET) = kind;
    t->b[CJW_TI_FLAG_OFFSET] = flag;
    *(uint32_t*)(t->b + CJW_TI_SIZE_OFFSET) = size;
    *(const void**)(t->b + CJW_TI_GCTIB_OFFSET) = gctib;
    *(const void**)(t->b + CJW_TI_SUPER_OFFSET) = super;
}

#define KIND_CLASS (-128)
#define KIND_UINT32 6
static TestTib kNodeTib = {1, {0x05}};      // payload slots 0 and 2 are references, slot 1 is an Int32/UInt32
static TestTib kStructTib = {1, {0x01}};    // struct { ref @0; u32 @4 }
static TestTib kBeyondTib = {1, {0x20}};    // slot 5: beyond an 8-byte payload
static TestTI tiLeaf, tiNode, tiRefArr, tiStruct, tiStructArr, tiU32, tiU32Arr, tiNoTib, tiBeyond;
// Runtime-instantiated TypeInfos (no GC tib; offsets/fields/sourceGeneric like CjWasmRuntime.c CjwTI).
static TestTI tiRtStruct, tiRtClass, tiVArr, tiRtVArrClass, tiRtStructArr, tiPlainStruct, tiPlainStructArr;
static uint32_t kRtStructOffs[2] = {0, 4};
static const void* kRtStructFields[2];
static uint32_t kRtClassOffs[3] = {0, 4, 8};
static const void* kRtClassFields[3];
static uint32_t kRtVArrOffs[1] = {0};
static const void* kRtVArrFields[1];
static void RtLayout(TestTI* t, uint16_t n, uint32_t* offs, const void** fields)
{
    *(uint16_t*)(t->b + CJW_TI_FIELDNUM_OFFSET) = n;
    *(uint32_t**)(t->b + CJW_TI_OFFSETS_OFFSET) = offs;
    *(const void**)(t->b + CJW_TI_SOURCE_GENERIC_OFFSET) = t;   // any non-NULL TypeTemplate
    *(const void***)(t->b + CJW_TI_FIELDS_OFFSET) = fields;
}

static void InitTypes(void)
{
    TiInit(&tiLeaf, KIND_CLASS, 0, 4 + 8, NULL, NULL);
    TiInit(&tiNode, KIND_CLASS, CJW_FLAG_HAS_REF_FIELD_BIT, 4 + 12, &kNodeTib, NULL);
    TiInit(&tiRefArr, CJW_KIND_RAWARRAY, 0, 4, NULL, &tiLeaf);
    TiInit(&tiStruct, CJW_KIND_STRUCT, CJW_FLAG_HAS_REF_FIELD_BIT, 8, &kStructTib, NULL);
    TiInit(&tiStructArr, CJW_KIND_RAWARRAY, 0, 8, NULL, &tiStruct);
    TiInit(&tiU32, KIND_UINT32, 0, 4, NULL, NULL);
    TiInit(&tiU32Arr, CJW_KIND_RAWARRAY, 0, 4, NULL, &tiU32);
    TiInit(&tiNoTib, KIND_CLASS, CJW_FLAG_HAS_REF_FIELD_BIT, 4 + 8, NULL, NULL);
    TiInit(&tiBeyond, KIND_CLASS, CJW_FLAG_HAS_REF_FIELD_BIT, 4 + 8, &kBeyondTib, NULL);
    // struct S<T> { ref @0; u32 @4 } and class C<T> { ref @0; u32 @4; S<T> @8 } instantiated at run time
    TiInit(&tiRtStruct, CJW_KIND_STRUCT, CJW_FLAG_HAS_REF_FIELD_BIT, 8, NULL, NULL);
    kRtStructFields[0] = &tiLeaf; kRtStructFields[1] = &tiU32;
    RtLayout(&tiRtStruct, 2, kRtStructOffs, kRtStructFields);
    TiInit(&tiRtClass, KIND_CLASS, CJW_FLAG_HAS_REF_FIELD_BIT, 16, NULL, NULL);
    kRtClassFields[0] = &tiLeaf; kRtClassFields[1] = &tiU32; kRtClassFields[2] = &tiRtStruct;
    RtLayout(&tiRtClass, 3, kRtClassOffs, kRtClassFields);
    // RawArray<S> instantiated at run time: its own size field is the pointer size (TypeInfoManager), the
    // element width comes from the component (MArray::GetElementSize, CjWasmRuntime.c ArrayElemBytes).
    TiInit(&tiRtStructArr, CJW_KIND_RAWARRAY, 0, 4, NULL, &tiStruct);
    TiInit(&tiPlainStruct, CJW_KIND_STRUCT, 0, 8, NULL, NULL);            // struct { u32; u32 } without references
    TiInit(&tiPlainStructArr, CJW_KIND_RAWARRAY, 0, 4, NULL, &tiPlainStruct);
    TiInit(&tiVArr, CJW_KIND_VARRAY, CJW_FLAG_HAS_REF_FIELD_BIT, 8, NULL, &tiLeaf);
    TiInit(&tiRtVArrClass, KIND_CLASS, CJW_FLAG_HAS_REF_FIELD_BIT, 8, NULL, NULL);
    kRtVArrFields[0] = &tiVArr;
    RtLayout(&tiRtVArrClass, 1, kRtVArrOffs, kRtVArrFields);
}

static void* NewObj(TestTI* ti)
{
    void* o = cjw_heap_alloc(*(uint32_t*)(ti->b + CJW_TI_SIZE_OFFSET));
    *(const void**)o = ti;
    return o;
}
static void* NewArr(TestTI* ti, int64_t len, uint32_t elem)
{
    uint8_t* o = (uint8_t*)cjw_heap_alloc(CJW_ARRAY_HEADER_SIZE + (size_t)len * elem);
    *(const void**)o = ti;
    *(int64_t*)(o + CJW_ARRAY_LENGTH_OFFSET) = len;
    return o;
}
// Raw blob of `size` bytes typed as a leaf (no references): every heap object carries a TypeInfo.
static uint8_t* NewBlob(size_t size)
{
    uint8_t* o = (uint8_t*)cjw_heap_alloc(size);
    *(const void**)o = &tiLeaf;
    return o;
}
static void** Slot(void* obj, uint32_t i) { return (void**)((uint8_t*)obj + CJW_OBJECT_HEADER_SIZE + 4 * i); }
static void** ArrSlot(void* arr, uint32_t i) { return (void**)((uint8_t*)arr + CJW_ARRAY_HEADER_SIZE + 4 * i); }
static int Alive(const void* p) { return cjw_heap_is_object(p); }

// Frame with up to 8 slots in C-stack storage (frame layout of CjWasmHeap.h).
typedef struct { uint32_t words[3 + 8]; } FrameStore;
static CjwRootFrame* Frame(FrameStore* s, uint32_t n)
{
    memset(s, 0, sizeof(*s));
    CjwRootFrame* f = (CjwRootFrame*)s;
    f->nslots = n;
    return f;
}

static int g_fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  check failed line %d: %s\n", __LINE__, #c); ok = 0; } } while (0)
static void Report(const char* name, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) { g_fails++; }
}
static CjwGcStats Stats(void) { CjwGcStats s; cjw_gc_get_stats(&s); return s; }

static void TestAllocBasic(void)
{
    int ok = 1;
    static const size_t sizes[] = {4, 8, 9, 24, 100, 2048, 2049, 4096, 5000, 70000, 1u << 21};
    enum { N = sizeof(sizes) / sizeof(sizes[0]) };
    // Rooted: with precise roots an allocation that finds no free space collects before growing.
    static uint8_t* p[N];
    cjw_static_roots_register((void**)p, N);
    for (int i = 0; i < N; ++i) {
        p[i] = (uint8_t*)cjw_heap_alloc(sizes[i]);
        CHECK(((uintptr_t)p[i] & 7u) == 0);
        CHECK(Alive(p[i]) && cjw_heap_object_bytes(p[i]) >= sizes[i]);
        for (size_t k = 0; k < sizes[i]; ++k) { if (p[i][k] != 0) { CHECK(!"allocation not zeroed"); break; } }
        *(const void**)p[i] = &tiLeaf;   // every managed object carries its TypeInfo before the next allocation
        memset(p[i] + 4, 0xA0 + i, sizes[i] - 4);
    }
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i != j) { CHECK(p[i] + sizes[i] <= p[j] || p[j] + sizes[j] <= p[i]); }
        }
        CHECK(*(const void**)p[i] == &tiLeaf);
        for (size_t k = 4; k < sizes[i]; ++k) { if (p[i][k] != (uint8_t)(0xA0 + i)) { CHECK(!"overlap"); break; } }
    }
    memset(p, 0, sizeof(p));
    Report("alloc_basic", ok);
}

static void TestUnrootedReclaimed(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    enum { N = 1000 };
    static void* objs[N];
    for (int i = 0; i < N; ++i) { objs[i] = NewObj(&tiLeaf); }
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    void* kept = NewObj(&tiLeaf);
    f->slots[0] = kept;
    cjw_root_frame_push(f);
    size_t freed = cjw_gc_collect_precise();
    cjw_root_frame_pop(f);
    int alive = 0;
    for (int i = 0; i < N; ++i) { alive += Alive(objs[i]); }
    CHECK(alive == 0);
    CHECK(freed >= N * 16u);
    CHECK(((uint8_t*)objs[N / 2])[8] == 0xDB);   // poisoned
    CHECK(Alive(kept));                           // positive control in the same cycle
    Report("unrooted_reclaimed", ok);
}

static void TestFrameRoot(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 2);
    cjw_root_frame_push(f);
    void* a = NewObj(&tiLeaf);
    *(uint64_t*)((uint8_t*)a + 4) = 0x1122334455667788ull;
    f->slots[0] = a;
    (void)cjw_gc_collect_precise();
    CHECK(Alive(a) && *(const void**)a == &tiLeaf && *(uint64_t*)((uint8_t*)a + 4) == 0x1122334455667788ull);
    CHECK(Stats().lastMarkedObjects == 1);
    cjw_root_frame_pop(f);
    (void)cjw_gc_collect_precise();
    CHECK(!Alive(a));
    CHECK(__cj_root_top == NULL);
    Report("frame_root", ok);
}

static void TestGctibFields(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    cjw_root_frame_push(f);
    void* n = NewObj(&tiNode); f->slots[0] = n;
    void* b = NewObj(&tiLeaf);
    void* c = NewObj(&tiLeaf);
    void* d = NewObj(&tiLeaf);
    *Slot(n, 0) = b;
    *(uint32_t*)Slot(n, 1) = (uint32_t)(uintptr_t)d;   // integer field that happens to equal d's address
    *Slot(n, 2) = c;
    (void)cjw_gc_collect_precise();
    CHECK(Alive(n) && Alive(b) && Alive(c));
    CHECK(!Alive(d));
    cjw_root_frame_pop(f);
    Report("gctib_fields", ok);
}

static void TestRefArray(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    cjw_root_frame_push(f);
    void* arr = NewArr(&tiRefArr, 5, 4); f->slots[0] = arr;
    void* e[5];
    for (int i = 0; i < 5; ++i) { e[i] = NewObj(&tiLeaf); *ArrSlot(arr, (uint32_t)i) = e[i]; }
    (void)cjw_gc_collect_precise();
    for (int i = 0; i < 5; ++i) { CHECK(Alive(e[i])); }
    *ArrSlot(arr, 2) = NULL;
    (void)cjw_gc_collect_precise();
    CHECK(!Alive(e[2]) && Alive(e[0]) && Alive(e[4]) && Alive(arr));
    cjw_root_frame_pop(f);
    Report("ref_array", ok);
}

static void TestStructArray(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    cjw_root_frame_push(f);
    void* arr = NewArr(&tiStructArr, 4, 8); f->slots[0] = arr;
    void* r[4]; void* x[4];
    for (int i = 0; i < 4; ++i) {
        r[i] = NewObj(&tiLeaf); x[i] = NewObj(&tiLeaf);
        *ArrSlot(arr, (uint32_t)(2 * i)) = r[i];                               // element i: ref @0
        *(uint32_t*)ArrSlot(arr, (uint32_t)(2 * i + 1)) = (uint32_t)(uintptr_t)x[i];   // u32 @4
    }
    (void)cjw_gc_collect_precise();
    for (int i = 0; i < 4; ++i) { CHECK(Alive(r[i])); CHECK(!Alive(x[i])); }
    cjw_root_frame_pop(f);
    Report("struct_array", ok);
}

static void TestRuntimeStructArray(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    cjw_root_frame_push(f);
    void* arr = NewArr(&tiRtStructArr, 4, 8); f->slots[0] = arr;   // allocated with the component width (8)
    void* r[4]; void* x[4];
    for (int i = 0; i < 4; ++i) {
        r[i] = NewObj(&tiLeaf); x[i] = NewObj(&tiLeaf);
        *ArrSlot(arr, (uint32_t)(2 * i)) = r[i];
        *(uint32_t*)ArrSlot(arr, (uint32_t)(2 * i + 1)) = (uint32_t)(uintptr_t)x[i];
    }
    (void)cjw_gc_collect_precise();
    for (int i = 0; i < 4; ++i) { CHECK(Alive(r[i])); CHECK(!Alive(x[i])); }
    cjw_root_frame_pop(f);
    Report("runtime_struct_array", ok);
}

static void TestPrimArray(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    cjw_root_frame_push(f);
    void* arr = NewArr(&tiU32Arr, 3, 4); f->slots[0] = arr;
    void* x = NewObj(&tiLeaf);
    *(uint32_t*)ArrSlot(arr, 1) = (uint32_t)(uintptr_t)x;
    (void)cjw_gc_collect_precise();
    CHECK(Alive(arr) && !Alive(x));
    cjw_root_frame_pop(f);
    Report("prim_array", ok);
}

static void TestCycles(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    void* a = NewObj(&tiNode); void* b = NewObj(&tiNode);
    *Slot(a, 0) = b; *Slot(b, 0) = a;
    (void)cjw_gc_collect_precise();
    CHECK(!Alive(a) && !Alive(b));
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    cjw_root_frame_push(f);
    a = NewObj(&tiNode); b = NewObj(&tiNode);
    *Slot(a, 2) = b; *Slot(b, 2) = a; f->slots[0] = b;
    (void)cjw_gc_collect_precise();
    CHECK(Alive(a) && Alive(b));
    cjw_root_frame_pop(f);
    Report("cycles", ok);
}

static void* g_staticSlots[3];
static void TestStaticRoots(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    static int registered = 0;
    if (!registered) { cjw_static_roots_register(g_staticSlots, 3); registered = 1; }
    void* o = NewObj(&tiLeaf);
    g_staticSlots[1] = o;
    (void)cjw_gc_collect_precise();
    CHECK(Alive(o));
    g_staticSlots[1] = NULL;
    (void)cjw_gc_collect_precise();
    CHECK(!Alive(o));
    Report("static_roots", ok);
}

static void TestInternalRoots(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    void* local = NewObj(&tiLeaf);
    cjw_add_root(&local);
    (void)cjw_gc_collect_precise();
    CHECK(Alive(local));
    cjw_remove_root(&local);
    (void)cjw_gc_collect_precise();
    CHECK(!Alive(local));
    Report("internal_roots", ok);
}

static void TestDeepChain(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    enum { N = 200000 };
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    cjw_root_frame_push(f);
    for (int i = 0; i < N; ++i) {
        void* n = NewObj(&tiNode);           // may collect: the chain so far is rooted by slot 0
        *Slot(n, 0) = f->slots[0];
        *(uint32_t*)Slot(n, 1) = (uint32_t)i;
        f->slots[0] = n;
    }
    (void)cjw_gc_collect_precise();
    CHECK(Stats().lastMarkedObjects == N);
    int count = 0, inOrder = 1;
    for (void* p = f->slots[0]; p != NULL && Alive(p); p = *Slot(p, 0)) {
        if (*(uint32_t*)Slot(p, 1) != (uint32_t)(N - 1 - count)) { inOrder = 0; }
        count++;
    }
    CHECK(count == N && inOrder);
    void* head = f->slots[0];
    cjw_root_frame_pop(f);
    (void)cjw_gc_collect_precise();
    CHECK(!Alive(head) && Stats().lastMarkedObjects == 0);
    Report("deep_chain", ok);
}

static void TestLargeReuse(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    size_t heapAfterFirst = 0;
    for (int i = 0; i < 40; ++i) {
        uint8_t* big = NewBlob(3u << 20);
        big[(3u << 20) - 1] = 1;
        (void)cjw_gc_collect_precise();
        CHECK(!Alive(big));
        if (i == 0) { heapAfterFirst = Stats().heapBytes; }
    }
    CHECK(Stats().heapBytes == heapAfterFirst);
    Report("large_reuse", ok);
}

static void TestAutoThreshold(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    cjw_gc_set_auto_threshold(1u << 20);
    CjwGcStats s0 = Stats();
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    cjw_root_frame_push(f);
    void* keep = NewObj(&tiLeaf); f->slots[0] = keep;
    *(uint64_t*)((uint8_t*)keep + 4) = 0xC0FFEEull;
    for (int i = 0; i < (64 << 20) / 64; ++i) { (void)NewBlob(60); }
    CjwGcStats s1 = Stats();
    CHECK(s1.collections - s0.collections >= 32);
    CHECK(s1.heapBytes - s0.heapBytes <= (4u << 20));
    CHECK(Alive(keep) && *(uint64_t*)((uint8_t*)keep + 4) == 0xC0FFEEull);
    cjw_root_frame_pop(f);
    cjw_gc_set_auto_threshold(CJW_GC_DEFAULT_AUTO_THRESHOLD);
    Report("auto_threshold", ok);
}

static void TestMallocCoexist(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    cjw_gc_set_auto_threshold(0);
    enum { N = 20, MS = 200000, HS = 300000 };
    uint8_t* m[N];
    static uint8_t* h[N];   // rooted: a full heap collects before growing even with the threshold off
    static int registered = 0;
    if (!registered) { cjw_static_roots_register((void**)h, N); registered = 1; }
    for (int i = 0; i < N; ++i) {
        m[i] = (uint8_t*)malloc(MS);
        CHECK(m[i] != NULL);
        memset(m[i], i, MS);
        h[i] = NewBlob(HS);
        memset(h[i] + 4, 0x80 + i, HS - 4);
    }
    for (int i = 0; i < N; ++i) {
        CHECK(!cjw_heap_contains(m[i]) && !cjw_heap_contains(m[i] + MS - 1));
        for (size_t k = 0; k < MS; k += 997) { if (m[i][k] != (uint8_t)i) { CHECK(!"malloc block clobbered"); break; } }
        for (size_t k = 4; k < HS; k += 997) { if (h[i][k] != (uint8_t)(0x80 + i)) { CHECK(!"heap block clobbered"); break; } }
        free(m[i]);
    }
    CHECK(Stats().chunks >= 2);
    memset(h, 0, sizeof(h));
    cjw_gc_set_auto_threshold(CJW_GC_DEFAULT_AUTO_THRESHOLD);
    (void)cjw_gc_collect_precise();
    Report("malloc_coexist", ok);
}

static int g_hookBegin = 0, g_hookEnd = 0;
static uint32_t g_hookEndCollections = 0;
static void Hook(int phase, const CjwGcStats* s, void* arg)
{
    (void)arg;
    if (phase == CJW_GC_PHASE_BEGIN) { g_hookBegin++; } else { g_hookEnd++; g_hookEndCollections = s->collections; }
}
static void TestHooks(void)
{
    int ok = 1;
    uint32_t before = Stats().collections;
    cjw_gc_set_hook(Hook, NULL);
    (void)cjw_gc_collect_precise();
    cjw_gc_set_hook(NULL, NULL);
    (void)cjw_gc_collect_precise();
    CHECK(g_hookBegin == 1 && g_hookEnd == 1 && g_hookEndCollections == before + 1);
    Report("hooks", ok);
}

static void TestM0Boundary(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    void* p = cjw_alloc(16, 8);
    *(const void**)p = &tiLeaf;
    CHECK(Alive(p) && cjw_arena_bytes() == Stats().liveBytes);
    cjw_collect();          // precise roots declared: collects instead of exiting
    CHECK(!Alive(p));
    Report("m0_boundary", ok);
}

static void TestRuntimeTypeInfoTib(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    uint32_t derived0 = Stats().derivedGCTibs;
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    cjw_root_frame_push(f);
    // instance size 16 + header: payload {ref a @0, u32 @4 = &c, S{ref b @8, u32 @12}}
    void* o = cjw_heap_alloc(4 + 16); *(const void**)o = &tiRtClass; f->slots[0] = o;
    void* a = NewObj(&tiLeaf); void* b = NewObj(&tiLeaf); void* c = NewObj(&tiLeaf);
    *Slot(o, 0) = a; *(uint32_t*)Slot(o, 1) = (uint32_t)(uintptr_t)c; *Slot(o, 2) = b; *(uint32_t*)Slot(o, 3) = 9;
    (void)cjw_gc_collect_precise();
    CHECK(Alive(o) && Alive(a) && Alive(b) && !Alive(c));
    const CjwGCTib* tib = (const CjwGCTib*)cjw_ti_ensure_gctib(&tiRtClass);
    CHECK(tib != NULL && tib->nBytes == 1 && tib->bits[0] == 0x05);          // slots 0 and 2
    const CjwGCTib* stib = (const CjwGCTib*)cjw_ti_ensure_gctib(&tiRtStruct);
    CHECK(stib != NULL && stib->bits[0] == 0x01);
    CHECK(Stats().derivedGCTibs - derived0 == 2);                            // class + nested struct, cached once
    (void)cjw_gc_collect_precise();
    CHECK(Stats().derivedGCTibs - derived0 == 2);
    CHECK(cjw_ti_ensure_gctib(&tiRtVArrClass) == NULL);                      // VArray field: not derivable
    CHECK(cjw_ti_ensure_gctib(&tiNoTib) == NULL);                            // compiled TypeInfo without metadata
    cjw_root_frame_pop(f);
    Report("runtime_ti_gctib", ok);
}

static uint64_t g_nonHeapThing[2];
static void TestVerifyHeap(void)
{
    int ok = 1;
    (void)cjw_gc_collect_precise();
    CjwHeapVerifyStats v;
    cjw_gc_verify_heap(&v);
    CHECK(v.objects == 0 && v.heapRefs == 0);
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 6);
    cjw_root_frame_push(f);
    void* n = NewObj(&tiNode); f->slots[0] = n;
    void* b = NewObj(&tiLeaf); f->slots[1] = b;
    void* c = NewObj(&tiLeaf); f->slots[2] = c;
    *Slot(n, 0) = b; *Slot(n, 2) = &g_nonHeapThing;                 // heap ref + non-heap ref
    *(uint32_t*)Slot(n, 1) = (uint32_t)(uintptr_t)c;                // integer field equal to an object start
    void* arr = NewArr(&tiRefArr, 3, 4); f->slots[3] = arr;
    *ArrSlot(arr, 0) = b; *ArrSlot(arr, 2) = c;
    void* sarr = NewArr(&tiStructArr, 2, 8); f->slots[4] = sarr;
    *ArrSlot(sarr, 0) = c; *(uint32_t*)ArrSlot(sarr, 1) = 7;
    f->slots[5] = NewObj(&tiNoTib);
    cjw_gc_verify_heap(&v);
    CHECK(v.objects == 6 && v.heapRefs == 4 && v.nonHeapRefs == 1 && v.untraceable == 1);
    CHECK(v.unmarkedHeapWords == 1);                                   // the completeness probe sees it
    // a flagless struct array whose element nevertheless holds an object start (wrong has-ref classification)
    void* parr = NewArr(&tiPlainStructArr, 2, 8); f->slots[5] = parr;
    *(uint32_t*)ArrSlot(parr, 3) = (uint32_t)(uintptr_t)b;
    cjw_gc_verify_heap(&v);
    CHECK(v.unmarkedHeapWords == 2 && v.untraceable == 1);        // the NoTib object is unrooted but not yet swept
    CHECK(Stats().collections > 0 && Alive(n) && Alive(b) && Alive(c));   // verification frees nothing
    f->slots[5] = NULL;
    cjw_root_frame_pop(f);
    Report("verify_heap", ok);
}

// ---- gate and negative modes -------------------------------------------------
static int ModeGateNoAutoCollect(void)
{
    // No precise roots: allocation must never collect, the heap only grows.
    int ok = 1;
    for (int i = 0; i < (48 << 20) / 64; ++i) { *(const void**)cjw_alloc(60, 8) = &tiLeaf; }
    CjwGcStats s = Stats();
    CHECK(s.collections == 0 && s.heapBytes >= (48u << 20) && s.freedBytesTotal == 0);
    Report("gate_no_auto_collect", ok);
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    InitTypes();
    const char* mode = argc > 1 ? argv[1] : "all";
    if (strcmp(mode, "gate") == 0) { return ModeGateNoAutoCollect(); }
    if (strcmp(mode, "verify_interior") == 0) {   // verifier without precise roots, metadata points mid-object
        void* n = NewObj(&tiNode); void* b = NewObj(&tiLeaf);
        *Slot(n, 2) = (uint8_t*)b + 8;
        CjwHeapVerifyStats v; cjw_gc_verify_heap(&v);
        return 0;
    }
    if (strcmp(mode, "collect_unprecise") == 0) { (void)cjw_alloc(16, 8); cjw_collect(); return 0; }
    cjw_gc_declare_precise_roots();
    if (strcmp(mode, "all") == 0) {
        TestAllocBasic(); TestUnrootedReclaimed(); TestFrameRoot(); TestGctibFields(); TestRefArray();
        TestStructArray(); TestRuntimeStructArray(); TestPrimArray(); TestCycles(); TestStaticRoots(); TestInternalRoots();
        TestDeepChain(); TestLargeReuse(); TestAutoThreshold(); TestMallocCoexist(); TestHooks(); TestM0Boundary(); TestVerifyHeap(); TestRuntimeTypeInfoTib();
        // Every object the suite left behind (live or garbage) must satisfy the TypeInfo contract.
        CjwHeapVerifyStats v;
        cjw_gc_verify_heap(&v);
        printf("PASS verify_all_leftovers objects=%zu heap_refs=%zu unmarked_heap_words=%zu\n", v.objects, v.heapRefs,
               v.unmarkedHeapWords);
        printf("HEAP_UNIT_FAILS=%d\n", g_fails);
        return g_fails;
    }
    FrameStore fs; CjwRootFrame* f = Frame(&fs, 1);
    cjw_root_frame_push(f);
    if (strcmp(mode, "interior_root") == 0) {
        f->slots[0] = (uint8_t*)NewObj(&tiLeaf) + 4;
    } else if (strcmp(mode, "wild_ti") == 0) {
        void* o = NewObj(&tiLeaf);
        *(uintptr_t*)o = 0xA0A0A0A0u;
        f->slots[0] = o;
    } else if (strcmp(mode, "rt_varray") == 0) {
        void* o = cjw_heap_alloc(4 + 8); *(const void**)o = &tiRtVArrClass;
        f->slots[0] = o;
    } else if (strcmp(mode, "no_gctib") == 0) {
        f->slots[0] = NewObj(&tiNoTib);
    } else if (strcmp(mode, "tib_beyond") == 0) {
        f->slots[0] = NewObj(&tiBeyond);
    } else if (strcmp(mode, "array_bounds") == 0) {
        void* arr = NewArr(&tiRefArr, 2, 4);
        *(int64_t*)((uint8_t*)arr + CJW_ARRAY_LENGTH_OFFSET) = 100000;
        f->slots[0] = arr;
    } else if (strcmp(mode, "pop_order") == 0) {
        FrameStore fs2; CjwRootFrame* g = Frame(&fs2, 0);
        cjw_root_frame_push(g);
        cjw_root_frame_pop(f);
        return 0;
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 90;
    }
    (void)cjw_gc_collect_precise();
    return 0;
}
