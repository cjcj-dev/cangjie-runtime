// Managed heap, root registry and non-moving mark-sweep collector for the
// wasm32-wasi Cangjie runtime (single instance, single thread).
//
// What this provides (M1 foundation):
//   * an arena carved from WebAssembly linear memory with memory.grow
//     (chunks of whole wasm pages, 4 KiB blocks, segregated size classes,
//     large objects as block runs, side alloc/mark bitmaps per 8-byte granule);
//   * a shadow root stack ABI for compiled code, a static root registry and
//     runtime-internal root slots;
//   * a precise, non-moving mark-sweep collector that traces objects with the
//     TypeInfo/GCTib metadata emitted by cjcj + the Cangjie LLVM for 32-bit
//     targets (see CjWasmObjectModel.h);
//   * explicit allocation and collection hooks.
//
// Soundness gate (read first): code compiled by the M0 toolchain does NOT push
// shadow root frames and does not register its static reference slots, so a
// collection inside such a program would free live objects. The collector
// therefore never runs unless the embedder (or, later, compiled code) has
// declared the roots precise with cjw_gc_declare_precise_roots(). Until then
// cjw_collect() exits with CJW_RC_UNSUPPORTED and allocation only grows the
// heap. Unsupported features are listed at the end of this header.
#ifndef CJ_WASM_HEAP_H
#define CJ_WASM_HEAP_H
#include <stddef.h>
#include <stdint.h>

// ---- shadow root stack (compiler <-> runtime ABI) --------------------------
// A function that keeps managed references live across a call that may
// allocate owns one frame record in linear memory (typically on the C shadow
// stack at __stack_pointer). Prologue: frame.prev = __cj_root_top;
// __cj_root_top = &frame. Every exit restores __cj_root_top = frame.prev
// (catch landing pads restore the value saved at entry). Before each call that
// may allocate, every live managed base pointer is stored into its slot; after
// the call it is reloaded (keeps a future moving collector correct).
// Slots hold NULL or the *start* of a managed object; derived/interior
// pointers must be rematerialised from their base and are rejected by the
// collector (GC integrity failure).
typedef struct CjwRootFrame {
    struct CjwRootFrame* prev;
    const void* funcDesc;   // opaque, diagnostics only (may be NULL)
    uint32_t nslots;
    void* slots[];
} CjwRootFrame;
_Static_assert(sizeof(void*) == 4, "wasm32 root frame layout");
_Static_assert(__builtin_offsetof(CjwRootFrame, nslots) == 8, "root frame: nslots at 8");
_Static_assert(__builtin_offsetof(CjwRootFrame, slots) == 12, "root frame: slots at 12");

extern CjwRootFrame* __cj_root_top;
void cjw_root_frame_push(CjwRootFrame* frame);
// Pops `frame`, which must be the current top (GC integrity failure otherwise).
void cjw_root_frame_pop(CjwRootFrame* frame);

// ---- static and runtime-internal roots ------------------------------------
// A static root range is `count` consecutive reference slots outside the
// managed heap (package globals, static fields). Registration is permanent.
void cjw_static_roots_register(void** slots, size_t count);
// A runtime-internal root is one slot owned by runtime C code that holds a
// managed reference across an allocation (exception in flight, init state).
void cjw_add_root(void** slot);
void cjw_remove_root(void** slot);

// ---- allocation ------------------------------------------------------------
// Zeroed, 8-byte aligned managed memory; never returns NULL (out of memory
// exits with CJW_RC_UNSUPPORTED like the M0 arena). With precise roots it may
// collect first: when the automatic threshold is reached, or when no free
// cell/block run fits the request (collect before growing linear memory).
// Growth policy: once the live set fills the heap, precise mode therefore runs a
// full collection roughly every chunk's worth (1 MiB) of allocation, then grows.
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
// Declares that every managed reference that is live at an allocation or
// collection point is reachable from the shadow root stack, the static root
// ranges or the internal root slots. Irreversible for the instance.
void cjw_gc_declare_precise_roots(void);
int cjw_gc_roots_precise(void);
// Bytes allocated since the last collection that trigger an automatic
// collection (only with precise roots). 0 disables automatic collection.
void cjw_gc_set_auto_threshold(size_t bytes);
#define CJW_GC_DEFAULT_AUTO_THRESHOLD (8u << 20)
// Runs one full collection; requires precise roots (exits with
// CJW_RC_UNSUPPORTED otherwise). Returns the number of bytes reclaimed.
size_t cjw_gc_collect_precise(void);

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
// a live object). Never marks or frees, so it is sound without precise roots and
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
//   in the compiler and a static root table; until then cjw_collect() exits.
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
