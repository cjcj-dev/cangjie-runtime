// Managed heap and non-moving mark-sweep collector for wasm32-wasi. See CjWasmHeap.h.
#include "CjWasmHeap.h"
#include "CjWasmObjectModel.h"
#include "CjWasmRuntime.h"
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define CJW_WASM_PAGE 65536u
#define CJW_BLOCK_SHIFT 12u
#define CJW_BLOCK_BYTES (1u << CJW_BLOCK_SHIFT)
#define CJW_GRANULE_SHIFT 3u
#define CJW_GRANULE (1u << CJW_GRANULE_SHIFT)
#define CJW_GRANULES_PER_BLOCK (CJW_BLOCK_BYTES >> CJW_GRANULE_SHIFT)
#define CJW_CHUNK_BYTES (1u << 20)   // default chunk = 16 wasm pages
#define CJW_SMALL_MAX 2048u
#define CJW_POISON 0xDB

enum { BK_FREE = 0, BK_SMALL = 1, BK_LARGE_HEAD = 2, BK_LARGE_CONT = 3 };

typedef struct {
    uint8_t kind;
    uint8_t cls;     // size class (BK_SMALL)
    uint16_t used;   // allocated cells (BK_SMALL)
    uint32_t run;    // blocks in the run (BK_LARGE_HEAD)
} CjwBlock;

typedef struct {
    uintptr_t base;      // first byte, wasm-page aligned
    uint32_t nblocks;
    uint32_t freeBlocks;
    CjwBlock* blocks;
    uint32_t* allocBits; // 1 bit per granule: an allocated object starts here
    uint32_t* markBits;  // 1 bit per granule: marked in the current cycle
} CjwChunk;

static const uint16_t kClassBytes[] = {8,   16,  24,  32,  48,  64,  80,  96,   128,  160,
                                       192, 256, 320, 384, 512, 640, 768, 1024, 1360, 2048};
#define CJW_NCLASSES ((uint32_t)(sizeof(kClassBytes) / sizeof(kClassBytes[0])))

static CjwChunk* g_chunks = NULL;
static uint32_t g_nchunks = 0;
static uint32_t g_capChunks = 0;
static void* g_freeList[CJW_NCLASSES];

static int g_collecting = 0;   // set by cjw_gc_enable
static int g_gcEvery = 0;      // CJ_WASM_GC_EVERY=1: collect at every allocation
static int g_growFirst = 0;    // growth rule: the last collection left > 1/2 of the heap live
static int g_inGc = 0;
static size_t g_autoThreshold = CJW_GC_DEFAULT_AUTO_THRESHOLD;
static size_t g_sinceGc = 0;
static CjwGcStats g_stats;
static CjwGcHook g_hook = NULL;
static void* g_hookArg = NULL;

CjwRootFrame* __cj_root_top = NULL;

typedef struct { void** const* entries; size_t count; } CjwRootTable;
static CjwRootTable* g_tables = NULL;
static size_t g_ntables = 0, g_capTables = 0;
static void*** g_internal = NULL;
static size_t g_ninternal = 0, g_capInternal = 0;

static int g_verify = 0;
static int g_verifyVerbose = 0;
static CjwHeapVerifyStats g_vstats;

static void** g_markStack = NULL;
static size_t g_markSp = 0, g_markCap = 0;

// ---------------------------------------------------------------------------
// Failure reporting
// Diagnostics also run on allocation failure: write directly to fd 2 without allocating.
static void DiagWrite(const char* s, size_t len)
{
    while (len != 0) {
        ssize_t n = write(STDERR_FILENO, s, len);
        if (n < 0 && errno == EINTR) { continue; }
        if (n <= 0) { return; }
        s += n;
        len -= (size_t)n;
    }
}

static void DiagNumber(uint64_t value, unsigned base, unsigned width, int prefix)
{
    char buf[24]; // 20 decimal digits (or 16 hex digits) and an optional prefix
    char* end = buf + sizeof(buf);
    char* p = end;
    do {
        *--p = "0123456789abcdef"[value % base];
        value /= base;
    } while (value != 0 || (unsigned)(end - p) < width);
    if (prefix) { *--p = 'x'; *--p = '0'; }
    DiagWrite(p, (size_t)(end - p));
}

// Only the conversions used in the two Wasm runtime translation units.
static void DiagMessage(const char* fmt, va_list args)
{
    while (*fmt) {
        const char* start = fmt;
        while (*fmt && *fmt != '%') { ++fmt; }
        DiagWrite(start, (size_t)(fmt - start));
        if (!*fmt) { break; }
        ++fmt;
        if (*fmt == 's') {
            const char* s = va_arg(args, const char*);
            if (s == NULL) { s = "(null)"; }
            DiagWrite(s, strlen(s));
        } else if (*fmt == 'u') {
            DiagNumber(va_arg(args, unsigned), 10, 0, 0);
        } else if (strncmp(fmt, "zu", 2) == 0) {
            DiagNumber(va_arg(args, size_t), 10, 0, 0);
            ++fmt;
        } else if (*fmt == 'd' || strncmp(fmt, "lld", 3) == 0) {
            int64_t n;
            if (*fmt == 'd') { n = va_arg(args, int); }
            else { n = va_arg(args, long long); fmt += 2; }
            if (n < 0) { DiagWrite("-", 1); }
            DiagNumber(n < 0 ? 0 - (uint64_t)n : (uint64_t)n, 10, 0, 0);
        } else if (*fmt == 'p') {
            uintptr_t p = (uintptr_t)va_arg(args, void*);
            DiagNumber(p, 16, 0, p != 0);
        } else if (strncmp(fmt, "#lx", 3) == 0) {
            unsigned long n = va_arg(args, unsigned long);
            DiagNumber(n, 16, 0, n != 0);
            fmt += 2;
        } else if (strncmp(fmt, "#x", 2) == 0 || strncmp(fmt, "02x", 3) == 0) {
            unsigned n = va_arg(args, unsigned);
            int padded = *fmt == '0';
            DiagNumber(n, 16, padded ? 2 : 0, !padded && n != 0);
            fmt += padded ? 2 : 1;
        } else {
            // Leave an unrecognized conversion visible in the diagnostic.
            DiagWrite("%", 1);
            DiagWrite(fmt, strlen(fmt));
            break;
        }
        ++fmt;
    }
}

__attribute__((format(printf, 1, 2), visibility("hidden")))
void cjw_diag(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    DiagMessage(fmt, ap);
    va_end(ap);
}

__attribute__((format(printf, 1, 2)))
static _Noreturn void GcIntegrity(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    cjw_diag("cangjie wasm32 runtime: GC integrity failure: ");
    DiagMessage(fmt, ap);
    DiagWrite("\n", 1);
    va_end(ap);
    const char* trap = getenv("CJ_WASM_TRAP_ON_UNSUPPORTED");
    if (trap != NULL && trap[0] == '1') { __builtin_trap(); }
    exit(CJW_RC_GC_INTEGRITY);
}

static _Noreturn void OutOfMemory(size_t size)
{
    cjw_diag("cangjie wasm32 runtime: out of memory allocating %zu bytes (arena %zu bytes)\n", size,
            g_stats.heapBytes);
    exit(CJW_RC_UNSUPPORTED);
}

static void* XRealloc(void* p, size_t n)
{
    void* q = realloc(p, n);
    if (q == NULL) { OutOfMemory(n); }
    return q;
}

// ---------------------------------------------------------------------------
// Chunks and bitmaps
static inline int TestBit(const uint32_t* bits, size_t i) { return (int)((bits[i >> 5] >> (i & 31u)) & 1u); }
static inline void SetBit(uint32_t* bits, size_t i) { bits[i >> 5] |= 1u << (i & 31u); }
static inline void ClearBit(uint32_t* bits, size_t i) { bits[i >> 5] &= ~(1u << (i & 31u)); }
static inline size_t ChunkBytes(const CjwChunk* c) { return (size_t)c->nblocks << CJW_BLOCK_SHIFT; }
static inline size_t BitWords(const CjwChunk* c) { return ((size_t)c->nblocks * CJW_GRANULES_PER_BLOCK) / 32u; }
static inline size_t GranuleOf(const CjwChunk* c, uintptr_t a) { return (a - c->base) >> CJW_GRANULE_SHIFT; }
static inline uint32_t BlockOf(const CjwChunk* c, uintptr_t a) { return (uint32_t)((a - c->base) >> CJW_BLOCK_SHIFT); }
static inline uintptr_t BlockAddr(const CjwChunk* c, uint32_t b) { return c->base + ((uintptr_t)b << CJW_BLOCK_SHIFT); }

// Chunks are appended in increasing address order (linear memory only grows).
static CjwChunk* FindChunk(uintptr_t a)
{
    uint32_t lo = 0, hi = g_nchunks;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        CjwChunk* c = &g_chunks[mid];
        if (a < c->base) {
            hi = mid;
        } else if (a - c->base >= ChunkBytes(c)) {
            lo = mid + 1;
        } else {
            return c;
        }
    }
    return NULL;
}

// Grows linear memory by a new chunk of at least minBlocks blocks; NULL if memory.grow fails.
static CjwChunk* NewChunk(uint32_t minBlocks)
{
    size_t bytes = CJW_CHUNK_BYTES;
    size_t need = (size_t)minBlocks << CJW_BLOCK_SHIFT;
    if (need > bytes) { bytes = (need + CJW_WASM_PAGE - 1) & ~(size_t)(CJW_WASM_PAGE - 1); }
    if (g_nchunks == g_capChunks) {
        g_capChunks = g_capChunks ? g_capChunks * 2 : 8;
        g_chunks = (CjwChunk*)XRealloc(g_chunks, g_capChunks * sizeof(CjwChunk));
    }
    uint32_t nblocks = (uint32_t)(bytes >> CJW_BLOCK_SHIFT);
    CjwBlock* blocks = (CjwBlock*)calloc(nblocks, sizeof(CjwBlock));
    size_t words = ((size_t)nblocks * CJW_GRANULES_PER_BLOCK) / 32u;
    uint32_t* allocBits = (uint32_t*)calloc(words, sizeof(uint32_t));
    uint32_t* markBits = (uint32_t*)calloc(words, sizeof(uint32_t));
    if (blocks == NULL || allocBits == NULL || markBits == NULL) { OutOfMemory(bytes); }
    size_t old = __builtin_wasm_memory_grow(0, bytes / CJW_WASM_PAGE);
    if (old == (size_t)-1) {
        free(blocks); free(allocBits); free(markBits);
        return NULL;
    }
    uintptr_t base = (uintptr_t)old * CJW_WASM_PAGE;
    if (g_nchunks > 0) {
        const CjwChunk* last = &g_chunks[g_nchunks - 1];
        if (base < last->base + ChunkBytes(last)) {
            GcIntegrity("memory.grow returned %#lx below the previous heap chunk %#lx", (unsigned long)base,
                        (unsigned long)last->base);
        }
    }
    CjwChunk* c = &g_chunks[g_nchunks++];
    c->base = base;
    c->nblocks = nblocks;
    c->freeBlocks = nblocks;
    c->blocks = blocks;
    c->allocBits = allocBits;
    c->markBits = markBits;
    g_stats.heapBytes += bytes;
    g_stats.chunks = g_nchunks;
    return c;
}

// ---------------------------------------------------------------------------
// Allocation
static uint32_t SizeClass(size_t size)
{
    for (uint32_t i = 0; i < CJW_NCLASSES; ++i) {
        if (size <= kClassBytes[i]) { return i; }
    }
    return CJW_NCLASSES; // large
}

// Turns one free block into cells of class cls and pushes them (lowest address first).
static int CarveBlock(uint32_t cls)
{
    for (uint32_t ci = 0; ci < g_nchunks; ++ci) {
        CjwChunk* c = &g_chunks[ci];
        if (c->freeBlocks == 0) { continue; }
        for (uint32_t b = 0; b < c->nblocks; ++b) {
            if (c->blocks[b].kind != BK_FREE) { continue; }
            c->blocks[b].kind = BK_SMALL;
            c->blocks[b].cls = (uint8_t)cls;
            c->blocks[b].used = 0;
            c->blocks[b].run = 0;
            c->freeBlocks--;
            uint32_t cell = kClassBytes[cls];
            uint32_t ncells = CJW_BLOCK_BYTES / cell;
            uintptr_t start = BlockAddr(c, b);
            for (uint32_t k = ncells; k > 0; --k) {
                void** p = (void**)(start + (uintptr_t)(k - 1) * cell);
                *p = g_freeList[cls];
                g_freeList[cls] = p;
            }
            return 1;
        }
    }
    return 0;
}

static void* TryAllocSmall(uint32_t cls)
{
    if (g_freeList[cls] == NULL && !CarveBlock(cls)) { return NULL; }
    void** p = (void**)g_freeList[cls];
    g_freeList[cls] = *p;
    uintptr_t a = (uintptr_t)p;
    CjwChunk* c = FindChunk(a);
    size_t g = GranuleOf(c, a);
    if (TestBit(c->allocBits, g)) { GcIntegrity("free-list cell %p is already allocated", (void*)p); }
    SetBit(c->allocBits, g);
    c->blocks[BlockOf(c, a)].used++;
    memset(p, 0, kClassBytes[cls]);
    g_stats.liveBytes += kClassBytes[cls];
    return p;
}

static void* TryAllocLarge(uint32_t n)
{
    for (uint32_t ci = 0; ci < g_nchunks; ++ci) {
        CjwChunk* c = &g_chunks[ci];
        if (c->freeBlocks < n) { continue; }
        uint32_t runStart = 0, runLen = 0;
        for (uint32_t b = 0; b < c->nblocks; ++b) {
            if (c->blocks[b].kind != BK_FREE) { runLen = 0; continue; }
            if (runLen == 0) { runStart = b; }
            if (++runLen < n) { continue; }
            c->blocks[runStart].kind = BK_LARGE_HEAD;
            c->blocks[runStart].run = n;
            for (uint32_t k = 1; k < n; ++k) {
                c->blocks[runStart + k].kind = BK_LARGE_CONT;
                c->blocks[runStart + k].run = 0;
            }
            c->freeBlocks -= n;
            uintptr_t a = BlockAddr(c, runStart);
            SetBit(c->allocBits, GranuleOf(c, a));
            memset((void*)a, 0, (size_t)n << CJW_BLOCK_SHIFT);
            g_stats.liveBytes += (size_t)n << CJW_BLOCK_SHIFT;
            return (void*)a;
        }
    }
    return NULL;
}

static void* TryAlloc(size_t size)
{
    uint32_t cls = SizeClass(size);
    if (cls < CJW_NCLASSES) { return TryAllocSmall(cls); }
    return TryAllocLarge((uint32_t)((size + CJW_BLOCK_BYTES - 1) >> CJW_BLOCK_SHIFT));
}

void* cjw_heap_alloc(size_t size)
{
    if (size == 0) { size = CJW_GRANULE; }
    if (size > ((size_t)1 << 31)) { OutOfMemory(size); }
    if (g_inGc) { GcIntegrity("managed allocation of %zu bytes during a collection", size); }
    // I-SLACK: reserve at least one byte more than requested, so a one-past
    // address of the object still lies inside its own allocation unit.
    size_t unit = size + 1;
    if (g_collecting && (g_gcEvery || (g_autoThreshold != 0 && g_sinceGc + size > g_autoThreshold))) {
        (void)cjw_gc_collect();
    }
    void* p = TryAlloc(unit);
    if (p == NULL && g_collecting && g_sinceGc != 0 && !g_growFirst) {
        (void)cjw_gc_collect();
        p = TryAlloc(unit);
    }
    if (p == NULL) {
        uint32_t cls = SizeClass(unit);
        uint32_t blocks = cls < CJW_NCLASSES ? 1u : (uint32_t)((unit + CJW_BLOCK_BYTES - 1) >> CJW_BLOCK_SHIFT);
        if (NewChunk(blocks) == NULL) { OutOfMemory(size); }
        g_growFirst = g_stats.liveBytes * 2 > g_stats.heapBytes;
        p = TryAlloc(unit);
        if (p == NULL) { GcIntegrity("fresh heap chunk cannot satisfy %zu bytes", size); }
    }
    size_t got = cjw_heap_object_bytes(p);
    g_sinceGc += got;
    g_stats.allocatedBytesTotal += got;
    return p;
}

static size_t ObjectBytesIn(const CjwChunk* c, uintptr_t a)
{
    const CjwBlock* blk = &c->blocks[BlockOf(c, a)];
    if (blk->kind == BK_SMALL) { return kClassBytes[blk->cls]; }
    if (blk->kind == BK_LARGE_HEAD) { return (size_t)blk->run << CJW_BLOCK_SHIFT; }
    return 0;
}

int cjw_heap_is_object(const void* p)
{
    uintptr_t a = (uintptr_t)p;
    CjwChunk* c = FindChunk(a);
    return c != NULL && (a & (CJW_GRANULE - 1)) == 0 && TestBit(c->allocBits, GranuleOf(c, a));
}

int cjw_heap_contains(const void* p) { return FindChunk((uintptr_t)p) != NULL; }

size_t cjw_heap_object_bytes(const void* p)
{
    if (!cjw_heap_is_object(p)) { return 0; }
    uintptr_t a = (uintptr_t)p;
    return ObjectBytesIn(FindChunk(a), a);
}

// ---------------------------------------------------------------------------
// Roots
void cjw_static_root_table_register(void** const* entries, size_t count)
{
    if (count == 0) { return; }
    if (entries == NULL) { GcIntegrity("static root table of %zu entries is NULL", count); }
    for (size_t i = 0; i < count; ++i) {
        if (entries[i] == NULL || FindChunk((uintptr_t)entries[i]) != NULL) {
            GcIntegrity("static root table %p entry %zu (%p) is NULL or inside the managed heap", (const void*)entries, i,
                        (void*)entries[i]);
        }
    }
    if (g_ntables == g_capTables) {
        g_capTables = g_capTables ? g_capTables * 2 : 16;
        g_tables = (CjwRootTable*)XRealloc(g_tables, g_capTables * sizeof(CjwRootTable));
    }
    g_tables[g_ntables].entries = entries;
    g_tables[g_ntables].count = count;
    g_ntables++;
}

void cjw_add_root(void** slot)
{
    if (g_ninternal == g_capInternal) {
        g_capInternal = g_capInternal ? g_capInternal * 2 : 16;
        g_internal = (void***)XRealloc(g_internal, g_capInternal * sizeof(void**));
    }
    g_internal[g_ninternal++] = slot;
}

void cjw_remove_root(void** slot)
{
    for (size_t i = g_ninternal; i > 0; --i) {
        if (g_internal[i - 1] == slot) {
            g_internal[i - 1] = g_internal[--g_ninternal];
            return;
        }
    }
    GcIntegrity("removing internal root slot %p that was never added", (void*)slot);
}

// ---------------------------------------------------------------------------
// Mark
static void MarkRef(void* ref, const char* what, const void* where)
{
    if (ref == NULL) { return; }
    uintptr_t a = (uintptr_t)ref;
    CjwChunk* c = FindChunk(a);
    if (c == NULL) {
        if (g_verify) { g_vstats.nonHeapRefs++; } else { g_stats.nonHeapRefsSeen++; }
        return;
    }
    size_t g = GranuleOf(c, a);
    if ((a & (CJW_GRANULE - 1)) != 0 || !TestBit(c->allocBits, g)) {
        GcIntegrity("%s at %p holds %p, which points into the managed heap but not at the start of a live object",
                    what, where, ref);
    }
    if (g_verify) {
        g_vstats.heapRefs++;
        return;
    }
    if (TestBit(c->markBits, g)) { return; }
    SetBit(c->markBits, g);
    g_stats.lastMarkedObjects++;
    g_stats.lastMarkedBytes += ObjectBytesIn(c, a);
    if (g_markSp == g_markCap) {
        g_markCap = g_markCap ? g_markCap * 2 : 1024;
        g_markStack = (void**)XRealloc(g_markStack, g_markCap * sizeof(void*));
    }
    g_markStack[g_markSp++] = ref;
}

// A reference-carrying object the M1 collector cannot trace: fatal when collecting,
// counted by the verifier.
static void Untraceable(const char* what, const uint8_t* ti)
{
    if (g_verify) {
        g_vstats.untraceable++;
        if (g_verifyVerbose) {
            const char* name = ti != NULL ? *(const char* const*)ti : NULL;
            cjw_diag("cangjie wasm32 runtime: gc verify: untraceable %s: %s\n", name ? name : "?", what);
        }
        return;
    }
    cjw_unsupported(what, CJW_RC_UNSUPPORTED);
}

// ---------------------------------------------------------------------------
// GC tibs for TypeInfos instantiated at run time
#define CJW_MAX_DERIVED_SLOTS 65536u
static int SetDerivedSlot(uint8_t* bits, uint32_t slot, uint32_t size)
{
    if (slot >= CJW_MAX_DERIVED_SLOTS || ((size_t)slot + 1u) * CJW_GCTIB_SLOT_BYTES > size) { return 0; }
    bits[slot >> 3] |= (uint8_t)(1u << (slot & 7u));
    return 1;
}

static const CjwGCTib* EnsureGCTib(const uint8_t* ti, int depth)
{
    const CjwGCTib* have = cjw_ti_gctib(ti);
    if (have != NULL) { return have; }
    if (depth > 32 || (cjw_ti_flag(ti) & CJW_FLAG_HAS_REF_FIELD_BIT) == 0) { return NULL; }
    int8_t kind = cjw_ti_kind(ti);
    if (kind == CJW_KIND_RAWARRAY || kind == CJW_KIND_VARRAY || kind == CJW_KIND_GENERIC_TI ||
        kind == CJW_KIND_GENERIC_CUSTOM) {
        return NULL;
    }
    uint16_t n = *(const uint16_t*)(ti + CJW_TI_FIELDNUM_OFFSET);
    const uint32_t* offs = *(const uint32_t* const*)(ti + CJW_TI_OFFSETS_OFFSET);
    const void* source = *(const void* const*)(ti + CJW_TI_SOURCE_GENERIC_OFFSET);
    const uint8_t* const* fields = *(const uint8_t* const* const*)(ti + CJW_TI_FIELDS_OFFSET);
    uint32_t size = cjw_ti_size(ti);
    if (source == NULL || offs == NULL || fields == NULL || n == 0 || size == 0) { return NULL; }
    uint32_t nBytes = (size / CJW_GCTIB_SLOT_BYTES + 7u) / 8u;
    CjwGCTib* tib = (CjwGCTib*)calloc(1, sizeof(CjwGCTib) + nBytes);
    if (tib == NULL) { OutOfMemory(sizeof(CjwGCTib) + nBytes); }
    tib->nBytes = nBytes;
    for (uint16_t f = 0; f < n; ++f) {
        const uint8_t* fti = fields[f];
        if (fti == NULL) { free(tib); return NULL; }
        int8_t fk = cjw_ti_kind(fti);
        if (fk == CJW_KIND_NOTHING || fk == CJW_KIND_UNIT) { continue; }
        uint32_t off = offs[f];
        if (cjw_kind_is_ref(fk)) {
            if (off % CJW_GCTIB_SLOT_BYTES != 0 || !SetDerivedSlot(tib->bits, off / CJW_GCTIB_SLOT_BYTES, size)) {
                free(tib); return NULL;
            }
            continue;
        }
        if ((cjw_ti_flag(fti) & CJW_FLAG_HAS_REF_FIELD_BIT) == 0) { continue; }
        const CjwGCTib* sub = cjw_kind_is_struct(fk) ? EnsureGCTib(fti, depth + 1) : NULL;
        if (sub == NULL || off % CJW_GCTIB_SLOT_BYTES != 0) { free(tib); return NULL; }   // VArray or untraceable value field
        for (uint32_t i = 0; i < sub->nBytes; ++i) {
            for (uint32_t j = 0; j < 8u; ++j) {
                if (((sub->bits[i] >> j) & 1u) != 0 &&
                    !SetDerivedSlot(tib->bits, off / CJW_GCTIB_SLOT_BYTES + i * 8u + j, size)) {
                    free(tib); return NULL;
                }
            }
        }
    }
    *(const CjwGCTib**)(uintptr_t)(ti + CJW_TI_GCTIB_OFFSET) = tib;   // cache, like TypeInfo::SetGCTib
    g_stats.derivedGCTibs++;
    if (g_verifyVerbose) {
        const char* name = *(const char* const*)ti;
        cjw_diag("cangjie wasm32 runtime: gc verify: derived tib for %s: size=%u bits[0]=%#x\n", name ? name : "?",
                size, nBytes ? tib->bits[0] : 0u);
    }
    return tib;
}

const void* cjw_ti_ensure_gctib(const void* typeInfo) { return EnsureGCTib((const uint8_t*)typeInfo, 0); }

static int TibMarks(const CjwGCTib* gctib, size_t slot)
{
    return gctib != NULL && slot / 8u < gctib->nBytes && ((gctib->bits[slot / 8u] >> (slot % 8u)) & 1u) != 0;
}

// Verifier only: unmarked words of [base, base+limit) that hold a live object start.
static void ProbeUnmarked(const CjwGCTib* gctib, const uint8_t* base, size_t limit)
{
    for (size_t slot = 0; (slot + 1u) * CJW_GCTIB_SLOT_BYTES <= limit; ++slot) {
        if (TibMarks(gctib, slot)) { continue; }
        const void* v = *(const void* const*)(base + slot * CJW_GCTIB_SLOT_BYTES);
        if (v != NULL && cjw_heap_is_object(v)) { g_vstats.unmarkedHeapWords++; }
    }
}

// Verifier level 3: print each distinct traced TypeInfo with its GC tib bytes once.
static const uint8_t* g_dumped[256];
static uint32_t g_ndumped = 0;
static void DumpTib(const uint8_t* ti, const CjwGCTib* gctib)
{
    for (uint32_t i = 0; i < g_ndumped; ++i) { if (g_dumped[i] == ti) { return; } }
    if (g_ndumped < 256u) { g_dumped[g_ndumped++] = ti; }
    const char* name = *(const char* const*)ti;
    cjw_diag("cangjie wasm32 runtime: gc verify: tib %s kind=%d size=%u nBytes=%u bits=", name ? name : "?",
            cjw_ti_kind(ti), cjw_ti_size(ti), gctib->nBytes);
    for (uint32_t i = 0; i < gctib->nBytes && i < 16u; ++i) { cjw_diag("%02x", gctib->bits[i]); }
    DiagWrite("\n", 1);
}

// Visits the reference slots that `gctib` marks in [base, base+limit).
static void VisitGCTib(const CjwGCTib* gctib, uint8_t* base, size_t limit, const void* owner)
{
    for (uint32_t i = 0; i < gctib->nBytes; ++i) {
        uint32_t bits = gctib->bits[i];
        while (bits != 0) {
            uint32_t j = (uint32_t)__builtin_ctz(bits);
            bits &= bits - 1;
            size_t off = ((size_t)i * 8u + j) * CJW_GCTIB_SLOT_BYTES;
            if (off + CJW_GCTIB_SLOT_BYTES > limit) {
                GcIntegrity("GC tib of object %p marks a reference at +%zu beyond its %zu-byte traced region", owner,
                            off, limit);
            }
            MarkRef(*(void**)(base + off), "reference field", base + off);
        }
    }
}

static void TraceArray(uint8_t* obj, const uint8_t* ti, size_t objBytes)
{
    int64_t len = *(const int64_t*)(obj + CJW_ARRAY_LENGTH_OFFSET);
    const uint8_t* comp = cjw_ti_super(ti);
    if (comp == NULL) { GcIntegrity("RawArray %p has a TypeInfo without a component TypeInfo", (void*)obj); }
    if (len < 0) { GcIntegrity("RawArray %p has negative length %lld", (void*)obj, (long long)len); }
    int8_t ck = cjw_ti_kind(comp);
    uint8_t* content = obj + CJW_ARRAY_HEADER_SIZE;
    size_t room = objBytes - CJW_ARRAY_HEADER_SIZE;
    if (cjw_kind_is_ref(ck)) {
        if (ck == CJW_KIND_GENERIC_TI || ck == CJW_KIND_GENERIC_CUSTOM) {
            Untraceable("tracing a RawArray whose component is an unresolved generic TypeInfo", ti);
            return;
        }
        if ((uint64_t)len > room / sizeof(void*)) {
            GcIntegrity("RawArray %p of %lld references exceeds its %zu-byte cell", (void*)obj, (long long)len, objBytes);
        }
        void** slots = (void**)content;
        for (int64_t i = 0; i < len; ++i) { MarkRef(slots[i], "array element", &slots[i]); }
        return;
    }
    if (cjw_kind_is_struct(ck)) {
        if ((cjw_ti_flag(comp) & CJW_FLAG_HAS_REF_FIELD_BIT) == 0) {
            // Verifier: a flagless struct/tuple/enum component claims to hold no references; probe that claim.
            uint32_t w = cjw_ti_size(comp);
            if (g_verify && w != 0 && (uint64_t)len <= room / w) {
                for (int64_t i = 0; i < len; ++i) { ProbeUnmarked(NULL, content + (size_t)i * w, w); }
            }
            return;
        }
        const CjwGCTib* gctib = EnsureGCTib(comp, 0);
        if (gctib == NULL) {
            Untraceable("tracing a struct array whose component TypeInfo has references but no GC tib", ti);
            return;
        }
        // Element width = the component TypeInfo's size, as MArray::GetElementSize and the allocation side
        // (CjWasmRuntime.c ArrayElemBytes); runtime-instantiated RawArray TypeInfos carry the pointer size.
        uint32_t elem = cjw_ti_size(comp);
        if (elem == 0) { GcIntegrity("struct array %p: component TypeInfo has size 0", (void*)obj); }
        if ((uint64_t)len > room / elem) {
            GcIntegrity("struct array %p of %lld x %u bytes exceeds its %zu-byte cell", (void*)obj, (long long)len,
                        elem, objBytes);
        }
        for (int64_t i = 0; i < len; ++i) {
            VisitGCTib(gctib, content + (size_t)i * elem, elem, obj);
            if (g_verify) { ProbeUnmarked(gctib, content + (size_t)i * elem, elem); }
        }
        return;
    }
    if (ck == CJW_KIND_VARRAY || (cjw_ti_flag(comp) & CJW_FLAG_HAS_REF_FIELD_BIT) != 0) {
        Untraceable("tracing a RawArray of VArray or other reference-carrying value components", ti);
    }
    // primitive components: nothing to trace
}

static void TraceObject(void* ref)
{
    uint8_t* obj = (uint8_t*)ref;
    uintptr_t a = (uintptr_t)obj;
    size_t objBytes = ObjectBytesIn(FindChunk(a), a);
    const uint8_t* ti = cjw_obj_ti(obj);
    if (ti == NULL) { GcIntegrity("managed object %p has no TypeInfo", ref); }
    uintptr_t memEnd = (uintptr_t)__builtin_wasm_memory_size(0) * CJW_WASM_PAGE;
    if ((uintptr_t)ti >= memEnd || memEnd - (uintptr_t)ti < CJW_TI_SUPER_OFFSET + sizeof(void*)) {
        GcIntegrity("managed object %p has a TypeInfo pointer %p outside linear memory", ref, (const void*)ti);
    }
    if (FindChunk((uintptr_t)ti) != NULL) { GcIntegrity("managed object %p has a TypeInfo %p inside the managed heap", ref, (const void*)ti); }
    int8_t kind = cjw_ti_kind(ti);
    if (kind == CJW_KIND_RAWARRAY) {
        TraceArray(obj, ti, objBytes);
        return;
    }
    if (kind == CJW_KIND_GENERIC_TI || kind == CJW_KIND_GENERIC_CUSTOM) {
        GcIntegrity("managed object %p has a non-instantiable TypeInfo kind %d", ref, kind);
    }
    if (kind == CJW_KIND_VARRAY) {   // a boxed VArray value: component layout not traced in M1
        if ((cjw_ti_flag(ti) & CJW_FLAG_HAS_REF_FIELD_BIT) != 0) { Untraceable("tracing a boxed VArray with references", ti); }
        return;
    }
    if ((cjw_ti_flag(ti) & CJW_FLAG_HAS_REF_FIELD_BIT) == 0) {
        if (g_verify) { ProbeUnmarked(NULL, obj + CJW_OBJECT_HEADER_SIZE, objBytes - CJW_OBJECT_HEADER_SIZE); }
        return;
    }
    const CjwGCTib* gctib = EnsureGCTib(ti, 0);
    if (gctib == NULL) {
        Untraceable("tracing an object whose TypeInfo has reference fields but no GC tib (and no derivable runtime "
                    "field layout)", ti);
        return;
    }
    VisitGCTib(gctib, obj + CJW_OBJECT_HEADER_SIZE, objBytes - CJW_OBJECT_HEADER_SIZE, ref);
    if (g_verify) {
        ProbeUnmarked(gctib, obj + CJW_OBJECT_HEADER_SIZE, objBytes - CJW_OBJECT_HEADER_SIZE);
        if (g_verifyVerbose >= 2) { DumpTib(ti, gctib); }
    }
}

// A shadow-frame word (slot or range byte window): NULL, a non-heap address, or
// any address inside the heap. Interpreted conservatively by address: marks the
// live allocation unit containing it (I-SLACK makes a one-past address belong to
// its own unit); free cells, free blocks and a small block's unused tail hold no
// object and are ignored (stale stack words).
static void MarkFrameWord(void* v, const void* where)
{
    uintptr_t a = (uintptr_t)v;
    CjwChunk* c = FindChunk(a);
    if (c == NULL) { return; }
    uint32_t b = BlockOf(c, a);
    while (b > 0 && c->blocks[b].kind == BK_LARGE_CONT) { --b; }
    const CjwBlock* blk = &c->blocks[b];
    uintptr_t start = BlockAddr(c, b);
    if (blk->kind == BK_SMALL) {
        uint32_t cell = kClassBytes[blk->cls];
        uint32_t k = (uint32_t)((a - start) / cell);
        if (k >= CJW_BLOCK_BYTES / cell) { return; }
        start += (uintptr_t)k * cell;
    } else if (blk->kind != BK_LARGE_HEAD) {
        return;
    }
    if (!TestBit(c->allocBits, GranuleOf(c, start))) { return; }
    MarkRef((void*)start, "shadow frame word", where);
}

void* cjw_inflight_exception(void); // CjWasmRuntime.c: the exception between throw and handler
static void* g_inFlightWhere = NULL;

static void MarkRoots(void)
{
    size_t frames = 0;
    // The in-flight exception is reachable only from the runtime while a cleanup or handler runs.
    g_inFlightWhere = cjw_inflight_exception();
    if (g_inFlightWhere != NULL) { MarkFrameWord(g_inFlightWhere, &g_inFlightWhere); }
    // MarkRoots order follows the ZGC root task split (zMark.cpp root tasks):
    // stack frames, then static tables, then runtime-internal roots.
    for (CjwRootFrame* f = __cj_root_top; f != NULL; f = f->prev) {
        // I-FRAME diagnostics: frames live on the wasm stack (not the heap), the
        // chain ascends strictly in stack address and every length is bounded.
        if (FindChunk((uintptr_t)f) != NULL) { GcIntegrity("root frame %p lies inside the managed heap", (void*)f); }
        if (++frames > (1u << 24)) { GcIntegrity("shadow root stack longer than 2^24 frames (cycle?)"); }
        if (f->prev != NULL && (uintptr_t)f->prev <= (uintptr_t)f) {
            GcIntegrity("root frame %p is not below its caller frame %p", (void*)f, (void*)f->prev);
        }
        if (f->nslots > (1u << 20)) { GcIntegrity("root frame %p claims %u slots", (void*)f, f->nslots); }
        for (uint32_t i = 0; i < f->nslots; ++i) { MarkFrameWord(f->slots[i], &f->slots[i]); }
        const CjwFrameRanges* r = f->ranges;
        if (r == NULL) { continue; }
        if (FindChunk((uintptr_t)r) != NULL) { GcIntegrity("frame range table %p lies inside the managed heap", (const void*)r); }
        if (r->nranges > (1u << 16)) { GcIntegrity("root frame %p claims %u stack ranges", (void*)f, r->nranges); }
        for (uint32_t k = 0; k < r->nranges; ++k) {
            const uint8_t* start = r->ranges[k].start;
            if (FindChunk((uintptr_t)start) != NULL) { GcIntegrity("frame range %p lies inside the managed heap", (const void*)start); }
            if (r->ranges[k].bytes > (1u << 26)) { GcIntegrity("frame range %p claims %u bytes", (const void*)start, r->ranges[k].bytes); }
            // Stack-backed aggregates may be packed or written through a
            // borrowed address. Read full pointer representations, including
            // unaligned ones, without crossing the allocation boundary.
            for (size_t end = sizeof(void*); end <= r->ranges[k].bytes; ++end) {
                void* word;
                const void* slot = start + end - sizeof(word);
                memcpy(&word, slot, sizeof(word));
                MarkFrameWord(word, slot);
            }
        }
    }
    for (size_t t = 0; t < g_ntables; ++t) {
        for (size_t i = 0; i < g_tables[t].count; ++i) {
            void** slot = g_tables[t].entries[i];
            MarkRef(*slot, "static root table slot", slot);
        }
    }
    for (size_t r = 0; r < g_ninternal; ++r) { MarkRef(*g_internal[r], "internal root slot", g_internal[r]); }
}

// ---------------------------------------------------------------------------
// Sweep: rebuilds the free lists, releases empty blocks and dead large runs.
static size_t Sweep(void)
{
    size_t freed = 0;
    for (uint32_t k = 0; k < CJW_NCLASSES; ++k) { g_freeList[k] = NULL; }
    for (uint32_t ci = g_nchunks; ci > 0; --ci) {
        CjwChunk* c = &g_chunks[ci - 1];
        for (uint32_t b = c->nblocks; b > 0; --b) {
            CjwBlock* blk = &c->blocks[b - 1];
            uintptr_t start = BlockAddr(c, b - 1);
            if (blk->kind == BK_SMALL) {
                uint32_t cell = kClassBytes[blk->cls];
                uint32_t ncells = CJW_BLOCK_BYTES / cell;
                uint32_t used = 0;
                for (uint32_t i = 0; i < ncells; ++i) {
                    uintptr_t a = start + (uintptr_t)i * cell;
                    size_t g = GranuleOf(c, a);
                    if (!TestBit(c->allocBits, g)) { continue; }
                    if (TestBit(c->markBits, g)) { used++; continue; }
                    ClearBit(c->allocBits, g);
                    memset((void*)a, CJW_POISON, cell);
                    freed += cell;
                }
                blk->used = (uint16_t)used;
                if (used == 0) {
                    blk->kind = BK_FREE;
                    c->freeBlocks++;
                    continue;
                }
                for (uint32_t i = ncells; i > 0; --i) {
                    uintptr_t a = start + (uintptr_t)(i - 1) * cell;
                    if (TestBit(c->allocBits, GranuleOf(c, a))) { continue; }
                    *(void**)a = g_freeList[blk->cls];
                    g_freeList[blk->cls] = (void*)a;
                }
            } else if (blk->kind == BK_LARGE_HEAD) {
                size_t g = GranuleOf(c, start);
                if (!TestBit(c->allocBits, g)) { GcIntegrity("large run at %p has no allocation bit", (void*)start); }
                if (TestBit(c->markBits, g)) { continue; }
                uint32_t n = blk->run;
                ClearBit(c->allocBits, g);
                memset((void*)start, CJW_POISON, (size_t)n << CJW_BLOCK_SHIFT);
                for (uint32_t i = 0; i < n; ++i) {
                    c->blocks[b - 1 + i].kind = BK_FREE;
                    c->blocks[b - 1 + i].run = 0;
                }
                c->freeBlocks += n;
                freed += (size_t)n << CJW_BLOCK_SHIFT;
            }
        }
    }
    return freed;
}

// ---------------------------------------------------------------------------
// Collection
void cjw_gc_enable(void) { g_collecting = 1; }
int cjw_gc_enabled(void) { return g_collecting; }
void cjw_gc_set_auto_threshold(size_t bytes) { g_autoThreshold = bytes; }
void cjw_gc_set_hook(CjwGcHook hook, void* arg)
{
    g_hook = hook;
    g_hookArg = arg;
}

static void RedzonesAfterCollect(void);

size_t cjw_gc_collect(void)
{
    if (!g_collecting) { return 0; }   // before CJ_MRT_CjRuntimeStart: nothing collects (I-BOOT)
    if (g_inGc) { GcIntegrity("re-entrant collection"); }
    g_inGc = 1;
    if (g_hook != NULL) { g_hook(CJW_GC_PHASE_BEGIN, &g_stats, g_hookArg); }
    g_stats.lastMarkedObjects = 0;
    g_stats.lastMarkedBytes = 0;
    for (uint32_t ci = 0; ci < g_nchunks; ++ci) { memset(g_chunks[ci].markBits, 0, BitWords(&g_chunks[ci]) * 4u); }
    g_markSp = 0;
    MarkRoots();
    while (g_markSp > 0) { TraceObject(g_markStack[--g_markSp]); }
    size_t freed = Sweep();
    g_stats.liveBytes -= freed;
    g_stats.freedBytesTotal += freed;
    g_stats.lastFreedBytes = freed;
    g_stats.collections++;
    g_sinceGc = 0;
    g_growFirst = g_stats.liveBytes * 2 > g_stats.heapBytes;
    RedzonesAfterCollect();   // drop records of reclaimed objects before their cells can be reused
    if (g_hook != NULL) { g_hook(CJW_GC_PHASE_END, &g_stats, g_hookArg); }
    g_inGc = 0;
    return freed;
}

void cjw_gc_get_stats(CjwGcStats* out) { *out = g_stats; }

void cjw_gc_verify_heap(CjwHeapVerifyStats* out)
{
    if (g_inGc) { GcIntegrity("heap verification during a collection"); }
    g_inGc = 1;
    g_verify = 1;
    memset(&g_vstats, 0, sizeof(g_vstats));
    for (uint32_t ci = 0; ci < g_nchunks; ++ci) {
        CjwChunk* c = &g_chunks[ci];
        for (uint32_t b = 0; b < c->nblocks; ++b) {
            CjwBlock* blk = &c->blocks[b];
            uintptr_t start = BlockAddr(c, b);
            if (blk->kind == BK_SMALL) {
                uint32_t cell = kClassBytes[blk->cls];
                for (uint32_t i = 0; i < CJW_BLOCK_BYTES / cell; ++i) {
                    uintptr_t a = start + (uintptr_t)i * cell;
                    if (!TestBit(c->allocBits, GranuleOf(c, a))) { continue; }
                    g_vstats.objects++;
                    TraceObject((void*)a);
                }
            } else if (blk->kind == BK_LARGE_HEAD && TestBit(c->allocBits, GranuleOf(c, start))) {
                g_vstats.objects++;
                TraceObject((void*)start);
            }
        }
    }
    g_verify = 0;
    g_inGc = 0;
    *out = g_vstats;
}

// ---------------------------------------------------------------------------
// M0 collector boundary (CjWasmRuntime.h): every managed allocation of the
// runtime entry points goes through cjw_alloc.
static int g_statsAtExit = 0;
static void PrintStatsAtExit(void)
{
    cjw_diag("cangjie wasm32 runtime: gc stats: heap=%zu live=%zu allocated=%zu freed=%zu collections=%u chunks=%u "
            "collecting=%d\n",
            g_stats.heapBytes, g_stats.liveBytes, g_stats.allocatedBytesTotal, g_stats.freedBytesTotal,
            g_stats.collections, g_stats.chunks, g_collecting);
}

static void VerifyAtExit(void)
{
    CjwHeapVerifyStats v;
    cjw_gc_verify_heap(&v);
    cjw_diag("cangjie wasm32 runtime: gc verify: objects=%zu heap_refs=%zu nonheap_refs=%zu untraceable=%zu "
            "derived_tibs=%u unmarked_heap_words=%zu internal_roots=%zu static_tables=%zu\n",
            v.objects, v.heapRefs, v.nonHeapRefs, v.untraceable, g_stats.derivedGCTibs, v.unmarkedHeapWords,
            g_ninternal, g_ntables);
}

// Diagnostic redzones (CJ_WASM_HEAP_REDZONE=1): every cjw_alloc request gets 16 extra bytes and the
// unused tail of its cell is filled with a pattern; before each allocation and at exit every live
// object's tail is checked, so a write past the requested size names the object instead of silently
// corrupting a neighbour (or a free-list link). Debug only: O(objects) per allocation.
#define CJW_RZ_BYTES 16u
#define CJW_RZ_FILL 0xC5
typedef struct { uint8_t* p; uint32_t size; uint32_t cell; } CjwRedzone;
static int g_redzone = 0;
static CjwRedzone* g_rz = NULL;
static size_t g_nrz = 0, g_caprz = 0;

static void CheckRedzones(void)
{
    size_t keep = 0;
    for (size_t i = 0; i < g_nrz; ++i) {
        CjwRedzone* r = &g_rz[i];
        if (!cjw_heap_is_object(r->p)) { continue; }   // reclaimed: drop the record
        for (uint32_t k = r->size; k < r->cell; ++k) {
            if (r->p[k] != CJW_RZ_FILL) {
                const uint8_t* ti = cjw_obj_ti(r->p);
                uintptr_t memEnd = (uintptr_t)__builtin_wasm_memory_size(0) * CJW_WASM_PAGE;
                const char* name = (ti != NULL && (uintptr_t)ti < memEnd) ? *(const char* const*)ti : NULL;
                GcIntegrity("redzone of object %p (TypeInfo %p %s, requested %u bytes) overwritten at +%u "
                            "(byte %#x)", (void*)r->p, (const void*)ti, name ? name : "?", r->size, k, r->p[k]);
            }
        }
        g_rz[keep++] = *r;
    }
    g_nrz = keep;
}

static void RedzonesAtExit(void) { CheckRedzones(); }
static void RedzonesAfterCollect(void)
{
    if (g_redzone) { CheckRedzones(); }   // reclaimed objects fail cjw_heap_is_object and are dropped here
}

void* cjw_alloc(size_t size, size_t align)
{
    if (align > CJW_GRANULE) { cjw_unsupported("managed allocation aligned above 8 bytes", CJW_RC_UNSUPPORTED); }
    if (!g_statsAtExit) {
        g_statsAtExit = 1;
        const char* s = getenv("CJ_WASM_GC_STATS");
        if (s != NULL && s[0] == '1') { atexit(PrintStatsAtExit); }
        const char* every = getenv("CJ_WASM_GC_EVERY");
        if (every != NULL && every[0] == '1') { g_gcEvery = 1; }
        const char* rz = getenv("CJ_WASM_HEAP_REDZONE");
        if (rz != NULL && rz[0] == '1') {
            g_redzone = 1;
            atexit(RedzonesAtExit);
        }
        const char* v = getenv("CJ_WASM_GC_VERIFY");
        if (v != NULL && v[0] >= '1' && v[0] <= '3') {
            g_verifyVerbose = v[0] - '1';   // 1: counts, 2: +names of derived/untraceable, 3: +every tib
            atexit(VerifyAtExit);
        }
    }
    if (!g_redzone) { return cjw_heap_alloc(size); }
    CheckRedzones();
    uint8_t* p = (uint8_t*)cjw_heap_alloc(size + CJW_RZ_BYTES);
    uint32_t cell = (uint32_t)cjw_heap_object_bytes(p);
    memset(p + size, CJW_RZ_FILL, cell - size);
    if (g_nrz == g_caprz) {
        g_caprz = g_caprz ? g_caprz * 2 : 256;
        g_rz = (CjwRedzone*)XRealloc(g_rz, g_caprz * sizeof(CjwRedzone));
    }
    g_rz[g_nrz++] = (CjwRedzone){ p, (uint32_t)size, cell };
    return p;
}

void cjw_collect(void) { (void)cjw_gc_collect(); }

size_t cjw_arena_bytes(void) { return g_stats.liveBytes; }
