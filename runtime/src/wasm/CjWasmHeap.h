// Managed heap, root registry and non-moving mark-sweep collector for the
// wasm32-wasi Cangjie runtime (single instance, single thread).
//
// What this provides (M1 foundation):
//   * an arena carved from WebAssembly linear memory with memory.grow
//     (chunks of whole wasm pages, 4 KiB blocks, segregated size classes,
//     large objects as block runs, side alloc/mark bitmaps per 8-byte granule);
//   * a shadow root stack ABI for compiled code, a static root registry and
//     runtime-internal root slots;
//   * a non-moving, single-threaded stop-the-world mark-sweep collector. Heap
//     objects are traced precisely with the TypeInfo/GCTib metadata emitted by
//     cjcj + the Cangjie LLVM for 32-bit targets (see CjWasmObjectModel.h);
//     static roots are precise slot addresses; stack roots are the words that
//     compiled code publishes in its shadow frames and the bytes of the stack
//     ranges those frames name. Stack words are interpreted conservatively by
//     address (an interior or one-past address keeps the whole allocation unit
//     alive), so this is not a precise collector;
//   * explicit allocation and collection hooks.
//
// The WASI command runs static root constructors before Cangjie startup
// enables collection. Compiled code uses one non-moving shadow-frame ABI.
#ifndef CJ_WASM_HEAP_H
#define CJ_WASM_HEAP_H
#include <stddef.h>
#include <stdint.h>

// ---- shadow root stack (compiler <-> runtime ABI) --------------------------
// One frame per activation of a compiled function that needs one (I-FRAME).
// The root pass, the last IR pass of the Cangjie pipeline, emits inline code:
// on entry it clears the frame's ranges, stores `ranges`, `nslots = 0`,
// `prev = __cj_root_top` and links `__cj_root_top = &frame`; before every call
// that may collect it stores the live managed values in slots[0..k) and
// `nslots = k`; before every return it unlinks `__cj_root_top = prev`. Slots are
// never rewritten (the collector does not move objects).
struct CjwFrameRanges;
typedef struct CjwRootFrame {
    struct CjwRootFrame* prev;
    const struct CjwFrameRanges* ranges;   // NULL or the frame's stack ranges
    uint32_t nslots;
    void* slots[];
} CjwRootFrame;
_Static_assert(sizeof(void*) == 4, "wasm32 root frame layout");
_Static_assert(__builtin_offsetof(CjwRootFrame, nslots) == 8, "root frame: nslots at 8");
_Static_assert(__builtin_offsetof(CjwRootFrame, slots) == 12, "root frame: slots at 12");

extern CjwRootFrame* __cj_root_top;
// Stack ranges are scanned byte by byte; candidate addresses found in these
// bytes can conservatively retain extra allocations.
typedef struct {
    void* start;
    uint32_t bytes;
} CjwFrameRange;
typedef struct CjwFrameRanges {
    uint32_t nranges;
    CjwFrameRange ranges[];
} CjwFrameRanges;
_Static_assert(__builtin_offsetof(CjwRootFrame, ranges) == 4, "root frame: ranges at 4");
_Static_assert(__builtin_offsetof(CjwFrameRanges, ranges) == 4, "frame ranges at 4");
_Static_assert(sizeof(CjwFrameRange) == 8, "frame range is 8 bytes");

// ---- static and runtime-internal roots ------------------------------------
// A static root table is `count` slot ADDRESSES (entries[i] is the address of one
// reference slot outside the heap), for non-contiguous globals: the form a
// compiler can emit per package and register from the package initializer.
// Entries must be non-NULL and outside the heap (GC integrity failure otherwise).
// (A linker-collected section is not used: the wasm-ld of this toolchain does not
// define __start_/__stop_ symbols for data sections.)
void cjw_static_root_table_register(void** const* entries, size_t count);
// A runtime-internal root is one slot owned by runtime C code that holds a
// managed reference across an allocation (exception in flight, init state).
void cjw_add_root(void** slot);
void cjw_remove_root(void** slot);

// ---- allocation ------------------------------------------------------------
// Zeroed, 8-byte aligned managed memory; never returns NULL (out of memory
// exits with CJW_RC_UNSUPPORTED like the M0 arena). Once collection is enabled it
// may collect first: when the automatic threshold is reached, when
// CJ_WASM_GC_EVERY=1, or when no free cell/block run fits the request (collect
// before growing linear memory). Growth rule: when the previous collection left
// more than half of the heap live, a failed fit grows the heap without another
// collection. I-SLACK: every allocation unit is strictly larger than the
// request, so any address in [base, base + size] belongs to the unit.
// Contract: the caller stores the object's TypeInfo* at offset 0 before the
// next allocation or collection point (the collector traces by TypeInfo).
void* cjw_heap_alloc(size_t size);
// 1 if p is the start of a live managed object, else 0.
int cjw_heap_is_object(const void* p);
// Bytes reserved for an object (its cell or block run), 0 if not an object.
size_t cjw_heap_object_bytes(const void* p);
// 1 if p lies anywhere in linear memory owned by the managed heap.
int cjw_heap_contains(const void* p);

// ---- collection policy and entry points -----------------------------------
// Enables collection (CJ_MRT_CjRuntimeStart, after the static root
// constructors ran). Irreversible for the instance; before it no allocation
// collects and an explicit collection does nothing.
void cjw_gc_enable(void);
int cjw_gc_enabled(void);
// Bytes allocated since the last collection that trigger an automatic
// collection (only once collection is enabled). 0 disables automatic collection.
void cjw_gc_set_auto_threshold(size_t bytes);
#define CJW_GC_DEFAULT_AUTO_THRESHOLD (8u << 20)
// Runs one full collection and returns the number of bytes reclaimed (0 and
// no collection before cjw_gc_enable).
size_t cjw_gc_collect(void);

typedef struct {
    size_t heapBytes;          // linear memory owned by the heap (all chunks)
    size_t liveBytes;          // bytes in allocated cells/runs right now
    size_t allocatedBytesTotal;
    size_t freedBytesTotal;
    size_t lastFreedBytes;
    size_t lastMarkedObjects;
    size_t lastMarkedBytes;
    size_t nonHeapRefsSeen;    // references outside the heap met while tracing (not traced)
    uint32_t collections;
    uint32_t chunks;
    uint32_t derivedGCTibs;    // GC tibs computed for runtime-instantiated TypeInfos
} CjwGcStats;
void cjw_gc_get_stats(CjwGcStats* out);

// Heap verifier: walks every allocated object and checks its TypeInfo and each
// reference slot its metadata describes (NULL, outside the heap, or the start of
// a live object). Never marks or frees, so it is sound before collection is enabled and
// can validate the metadata contract on objects of real compiled programs.
// It is one-sided for marked slots (a marked slot must be valid) and only
// probes the converse (unmarked words that look like object starts).
// Objects whose TypeInfo has references but no GC tib are counted, not fatal.
// Integrity failures exit with CJW_RC_GC_INTEGRITY. CJ_WASM_GC_VERIFY=1 runs it
// at exit and prints one line on stderr (=2 also names each untraceable TypeInfo).
typedef struct {
    size_t objects;
    size_t heapRefs;      // slots pointing at live heap objects
    size_t nonHeapRefs;   // non-NULL slots pointing outside the heap
    size_t untraceable;   // objects with references but no GC tib
    // Completeness probe: payload words the metadata does NOT mark as references
    // whose value is nevertheless the start of a live object (class payloads and
    // struct/tuple/enum array elements with or without the has-ref flag; arrays of
    // builtin primitives and boxed VArrays are not probed). A missing GC tib
    // bit shows up here; an integer that equals an object address is a false hit.
    size_t unmarkedHeapWords;
} CjwHeapVerifyStats;
void cjw_gc_verify_heap(CjwHeapVerifyStats* out);

// GC tib of a TypeInfo, computing and caching it (in the TypeInfo's gctib slot)
// for TypeInfos instantiated at run time from a TypeTemplate, from their field
// offsets and field TypeInfos (the data native TypeGCInfo::GetGCTibStr walks).
// NULL when the TypeInfo has no reference fields or its layout cannot be
// described (VArray fields, misaligned or out-of-size references).
const void* cjw_ti_ensure_gctib(const void* typeInfo);

// Hook called at the begin/end of every collection (stats are current).
enum { CJW_GC_PHASE_BEGIN = 0, CJW_GC_PHASE_END = 1 };
typedef void (*CjwGcHook)(int phase, const CjwGcStats* stats, void* arg);
void cjw_gc_set_hook(CjwGcHook hook, void* arg);

// GC integrity failures (corrupt roots, interior pointers, metadata that
// disagrees with the object) exit with this code; feature gaps use
// CJW_RC_UNSUPPORTED (135) from CjWasmRuntime.h.
#define CJW_RC_GC_INTEGRITY 139

// ---- unsupported in this M1 foundation (explicit, not silent) --------------
// * Collection in compiled Cangjie programs: needs the shadow-stack lowering
//   in the compiler and a static root table; before cjw_gc_enable cjw_collect() does nothing.
// * Interior/derived pointers as roots or fields: integrity failure.
// * References stored inside non-heap memory (data segment, malloc) are not
//   traced unless the slot is registered as a static or internal root.
// * TypeInfos with reference fields but no GCTib that are not runtime
//   instantiations with field metadata (cjw_ti_ensure_gctib), or whose fields
//   include VArrays, cannot be traced: exits 135.
// * Weak references are traced as strong references; finalizers never run;
//   VArray-of-reference array components exit 135.
// * No moving/compaction, no generations, no incremental or concurrent work,
//   no threads; memory is never returned to the host (wasm cannot shrink).
#endif
