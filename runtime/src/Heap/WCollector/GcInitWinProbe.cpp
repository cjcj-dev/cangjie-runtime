// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "GcInitWinProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>

namespace MapleRuntime {
namespace GcInitWin {
namespace {

constexpr size_t kWatchN = 12;
constexpr size_t kBadWatchN = 6;
constexpr size_t kWriteCap = 96;
constexpr size_t kLifeCap = 48;
constexpr size_t kInitEventCap = 32;
constexpr size_t kNameCap = 96;
constexpr size_t kSnapshotCap = 512;
constexpr size_t kDeltaCap = 128;
constexpr size_t kWgRingCap = 256;
constexpr size_t kWgPhaseN = 16;
constexpr size_t kOldIdCap = 32;
constexpr size_t kRawWordN = 16; // 64B before + 64B at target = 16 words

enum SnapshotMoment : uint8_t {
    SNAP_A = 0,
    SNAP_B_PRE,
    SNAP_B_POST,
    SNAP_C,
    SNAP_COUNT
};

enum WritePath : uint8_t {
    PATH_MCC_STATIC_REF = 0,
    PATH_IDLE_STATIC_REF,
    PATH_ENUM_STATIC_REF,
    PATH_TRACE_STATIC_REF,
    PATH_POST_STATIC_REF,
    PATH_BARRIER_STATIC_REF,
    PATH_MCC_STATIC_STRUCT,
    PATH_IDLE_STATIC_STRUCT,
    PATH_ENUM_STATIC_STRUCT,
    PATH_TRACE_STATIC_STRUCT,
    PATH_POST_STATIC_STRUCT,
    PATH_BARRIER_STATIC_STRUCT,
    PATH_WRITE_GENERIC,
    PATH_OTHER,
    PATH_COUNT
};

struct WatchSlot {
    const void* addr;
    char name[kNameCap];
    char packageHint[32];
    std::atomic<uint64_t> writeCount;
    std::atomic<uintptr_t> lastValue;
    std::atomic<uint32_t> lastPhase; // 0=pre_any 1=in_global_init 2=post_all_known 3=between
    std::atomic<uint8_t> lastValueClass; // 0=null 1=low 2=noncanon 3=unmapped_like 4=plausible
    std::atomic<uint8_t> seenNonNull;
    std::atomic<uint8_t> seenPlausible;
    std::atomic<uint8_t> resolved;
    uint64_t snapshotRound;
    uint8_t snapshotMask;
    uintptr_t snapshotValue[SNAP_COUNT];
    MinorTargetFate aFate;
    std::atomic<uint64_t> snapshotCount[SNAP_COUNT];
    std::atomic<uint64_t> aToBPreCompared;
    std::atomic<uint64_t> aToBPreChanged;
    std::atomic<uint64_t> aToBPostCompared;
    std::atomic<uint64_t> aToBPostChanged;
    std::atomic<uint64_t> bPostToCCompared;
    std::atomic<uint64_t> bPostToCChanged;
};

struct WriteRec {
    const void* field;
    uintptr_t value;
    uint32_t seq;
    uint32_t phase;
    uint8_t valueClass;
    uint8_t watchIdx;
    char site[24];
    char slotName[kNameCap];
    char initFile[48];
};

struct LifeRec {
    const void* slot;
    uintptr_t value;
    uint32_t seq;
    uint32_t phase;
    uint8_t tiClass;
    uint8_t valueClass;
    uint8_t lifecycle; // 0=never_written 1=partial_mid 2=init_done_value_bad 3=init_done_value_good 4=unknown
    char kind[8];
    char point[20];
    char slotName[kNameCap];
    char initFile[48];
    uint64_t writesSeen;
};

struct InitEvent {
    char file[48];
    uint32_t seq;
    uint8_t begin; // 1 begin 0 end
    uint8_t ok;
    uint32_t depthAfter;
    uint64_t completedAfter;
};

std::atomic<int> g_on{-1};
std::atomic<uint32_t> g_initDepth{0};
std::atomic<uint64_t> g_initCompleted{0};
std::atomic<uint64_t> g_initBeginTotal{0};
std::atomic<uint32_t> g_writeSeq{0};
std::atomic<uint32_t> g_lifeSeq{0};
std::atomic<uint32_t> g_initEventSeq{0};
std::atomic<uint64_t> g_writeEmitted{0};
std::atomic<uint64_t> g_lifeEmitted{0};
std::atomic<uint64_t> g_initEventEmitted{0};
std::atomic<uint64_t> g_writeTotal{0};
std::atomic<uint64_t> g_writeWatched{0};
std::atomic<uint64_t> g_writeGarbageVal{0};
std::atomic<uint64_t> g_structWriteTotal{0};
std::atomic<uint64_t> g_minorRound{0};
std::atomic<uint64_t> g_snapshotEmitted{0};
std::atomic<uint64_t> g_deltaEmitted{0};
std::atomic<uint64_t> g_pathTotal[PATH_COUNT];
std::atomic<uint64_t> g_pathWatched[PATH_COUNT];
std::atomic<bool> g_summaryDumped{false};
std::atomic<bool> g_resolvedOnce{false};

// WriteGeneric phase histogram + recent-write ring for reverse lookup at bad-TI.
std::atomic<uint64_t> g_wgPhaseTotal[kWgPhaseN];
std::atomic<uint64_t> g_wgTotal{0};
struct WgRec {
    const void* obj;
    const void* field;
    size_t size;
    uint8_t phase;
    uint32_t seq;
};
WgRec g_wgRing[kWgRingCap];
std::atomic<uint64_t> g_wgRingN{0};
std::atomic<uint64_t> g_oldIdEmitted{0};
std::atomic<uint64_t> g_badTiEvents{0};
std::atomic<uint64_t> g_faultyRoundSlotDeltas{0};

// FixMinorEvacuatedSlot write observation (gcfwdwrite).
constexpr size_t kFixWriteSampleCap = 96;
constexpr size_t kFixWriteRingCap = 512;
std::atomic<uint64_t> g_fixWriteTotal{0};
std::atomic<uint64_t> g_fixWriteValid{0};
std::atomic<uint64_t> g_fixWriteInvalid{0};
std::atomic<uint64_t> g_fixWriteEmitted{0};
std::atomic<uint64_t> g_fixWriteDidForward{0};
std::atomic<uint64_t> g_crashEqualsFixWriteYes{0};
std::atomic<uint64_t> g_crashEqualsFixWriteNo{0};
std::atomic<uint64_t> g_crashEqualsFixWriteChecked{0};

struct FixWriteRec {
    const void* slot;
    const void* oldValue;
    const void* target;
    const void* current;
    uintptr_t currentTi;
    uint8_t tiValid; // 1=looks like real TypeInfo, 0=garbage/low/noncanon/unmapped
    uint8_t didForward;
    uint8_t ghostFrom;
    uint8_t unmovable;
    uint8_t young;
    uint8_t validRegion;
    uint8_t freeRegion;
    uint8_t garbageRegion;
    uint8_t inAlloc;
    uint8_t regionType;
    uint8_t watchIdx; // 0xff if not watched
    uint32_t seq;
    uint64_t round;
    char slotName[kNameCap];
};
FixWriteRec g_fixWriteSamples[kFixWriteSampleCap];
// Ring of current addresses written by FixMinorEvacuatedSlot (for crash-addr match).
const void* g_fixWriteRing[kFixWriteRingCap];
std::atomic<uint64_t> g_fixWriteRingN{0};

// Active init file name (best-effort single mutator during package init).
char g_activeInitFile[48] = {};
std::atomic<bool> g_activeInitSet{false};

WatchSlot g_watch[kWatchN];
std::atomic<size_t> g_watchCount{0};
std::atomic<uintptr_t> g_watchMin{0};
std::atomic<uintptr_t> g_watchMax{0};
WriteRec g_writes[kWriteCap];
LifeRec g_lives[kLifeCap];
InitEvent g_inits[kInitEventCap];

uint8_t ClassifyValue(uintptr_t v)
{
    if (v == 0) {
        return 0;
    }
    if (v < 0x1000ULL) {
        return 1;
    }
    // non-canonical user VA on x86_64: top bits set outside 48-bit
    if (v >= (1ULL << 48) && v < (~0ULL - (1ULL << 48))) {
        return 2;
    }
    // small non-null integers often seen as garbage TI
    if (v <= 0x10000ULL) {
        return 1;
    }
    // heuristic: heap-like or image-like
    return 4;
}

const char* ValueClassName(uint8_t c)
{
    switch (c) {
        case 0: return "null";
        case 1: return "garbage_low";
        case 2: return "noncanon";
        case 3: return "unmapped_like";
        case 4: return "plausible";
        default: return "?";
    }
}

const char* PhaseName(uint32_t p)
{
    switch (p) {
        case 0: return "pre_any_global_init";
        case 1: return "in_global_init";
        case 2: return "post_all_global_init";
        case 3: return "between_global_inits";
        default: return "?";
    }
}

const char* LifecycleName(uint8_t l)
{
    switch (l) {
        case 0: return "never_written_by_static_ref";
        case 1: return "partial_or_mid_init";
        case 2: return "init_done_value_still_bad";
        case 3: return "init_done_value_good";
        case 4: return "unknown";
        default: return "?";
    }
}

const char* SnapshotMomentName(uint8_t moment)
{
    static const char* names[SNAP_COUNT] = {"a-root", "b-pre-fix", "b-post-fix", "c-enqueue"};
    return moment < SNAP_COUNT ? names[moment] : "?";
}

int SnapshotMomentId(const char* moment)
{
    if (moment == nullptr) {
        return -1;
    }
    for (uint8_t i = 0; i < SNAP_COUNT; ++i) {
        if (std::strcmp(moment, SnapshotMomentName(i)) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const char* WritePathName(uint8_t path)
{
    static const char* names[PATH_COUNT] = {
        "MCC_WriteStaticRef", "IdleBarrier::WriteStaticRef", "EnumBarrier::WriteStaticRef",
        "TraceBarrier::WriteStaticRef", "PostTraceBarrier::WriteStaticRef", "Barrier::WriteStaticRef",
        "MCC_WriteStaticStruct", "IdleBarrier::WriteStaticStruct", "EnumBarrier::WriteStaticStruct",
        "TraceBarrier::WriteStaticStruct", "PostTraceBarrier::WriteStaticStruct", "Barrier::WriteStaticStruct",
        "Barrier::WriteGeneric", "other"
    };
    return path < PATH_COUNT ? names[path] : "?";
}

const char* GcPhaseNameLocal(uint8_t phase)
{
    // Mirrors Collector::GetGCPhaseName numeric slots used in this tree.
    switch (phase) {
        case 0: return "undef";
        case 1: return "idle";
        case 2: return "finish";
        case 3: return "reclaim_satb";
        case 8: return "init";
        case 9: return "enum";
        case 10: return "trace";
        case 11: return "clear_satb";
        case 12: return "post_trace";
        case 13: return "preforward";
        case 14: return "forward";
        default: return "other";
    }
}

uint8_t WritePathId(const char* site)
{
    if (site == nullptr) {
        return PATH_OTHER;
    }
    if (std::strstr(site, "MCC_WriteStaticRef") != nullptr) return PATH_MCC_STATIC_REF;
    if (std::strstr(site, "IdleBarrier::WriteStaticRef") != nullptr) return PATH_IDLE_STATIC_REF;
    if (std::strstr(site, "EnumBarrier::WriteStaticRef") != nullptr) return PATH_ENUM_STATIC_REF;
    if (std::strstr(site, "PostTraceBarrier::WriteStaticRef") != nullptr) return PATH_POST_STATIC_REF;
    if (std::strstr(site, "TraceBarrier::WriteStaticRef") != nullptr) return PATH_TRACE_STATIC_REF;
    if (std::strstr(site, "Barrier::WriteStaticRef") != nullptr) return PATH_BARRIER_STATIC_REF;
    if (std::strstr(site, "MCC_WriteStaticStruct") != nullptr) return PATH_MCC_STATIC_STRUCT;
    if (std::strstr(site, "IdleBarrier::WriteStaticStruct") != nullptr) return PATH_IDLE_STATIC_STRUCT;
    if (std::strstr(site, "EnumBarrier::WriteStaticStruct") != nullptr) return PATH_ENUM_STATIC_STRUCT;
    if (std::strstr(site, "PostTraceBarrier::WriteStaticStruct") != nullptr) return PATH_POST_STATIC_STRUCT;
    if (std::strstr(site, "TraceBarrier::WriteStaticStruct") != nullptr) return PATH_TRACE_STATIC_STRUCT;
    if (std::strstr(site, "Barrier::WriteStaticStruct") != nullptr) return PATH_BARRIER_STATIC_STRUCT;
    if (std::strstr(site, "WriteGeneric") != nullptr) return PATH_WRITE_GENERIC;
    return PATH_OTHER;
}

void RecordWritePath(const char* site, size_t watched)
{
    uint8_t path = WritePathId(site);
    g_pathTotal[path].fetch_add(1, std::memory_order_relaxed);
    if (watched != 0) {
        g_pathWatched[path].fetch_add(watched, std::memory_order_relaxed);
    }
}

MinorTargetFate EmptyFate()
{
    MinorTargetFate fate{};
    fate.regionType = 0xff;
    fate.marked = 2;
    fate.state = 0xff;
    return fate;
}

void ResetSnapshot(WatchSlot& watch, uint64_t round)
{
    watch.snapshotRound = round;
    watch.snapshotMask = 0;
    for (size_t i = 0; i < SNAP_COUNT; ++i) {
        watch.snapshotValue[i] = 0;
    }
    watch.aFate = EmptyFate();
}

uint32_t CurrentPhase()
{
    uint32_t depth = g_initDepth.load(std::memory_order_acquire);
    if (depth > 0) {
        return 1;
    }
    uint64_t done = g_initCompleted.load(std::memory_order_acquire);
    if (done == 0) {
        return 0;
    }
    // completed some package inits; not currently inside one
    return 2;
}

void CopyStr(char* dst, size_t n, const char* src)
{
    if (dst == nullptr || n == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, n, "%s", src);
}

int FindWatch(const void* field)
{
    size_t n = g_watchCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n && i < kWatchN; ++i) {
        if (g_watch[i].addr == field) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Parse /proc/self/maps for cjcj-stage1 / main image, then nm-like scan via known file_off anchors.
// Primary path: read ELF .dynsym/.symtab is heavy; use file_off table from previous bars + dl_iterate.
// For this probe we resolve by scanning maps-backed ELF symbol table lightly via popen-free method:
// walk known offsets relative to image base once maps are available.

struct ImageBase {
    uintptr_t start;
    uintptr_t end;
    uintptr_t fileOffBase; // maps offset for start
    char path[256];
    bool ok;
};

bool FindMainImage(ImageBase& out)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (f == nullptr) {
        return false;
    }
    char line[512];
    out.ok = false;
    while (fgets(line, sizeof(line), f) != nullptr) {
        uintptr_t start = 0;
        uintptr_t end = 0;
        uintptr_t offset = 0;
        char perms[8] = {};
        char path[256] = {};
        int n = std::sscanf(line, "%lx-%lx %7s %lx %*s %*s %255[^\n]", &start, &end, perms, &offset, path);
        if (n < 5) {
            continue;
        }
        // trim path
        const char* p = path;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (std::strstr(p, "cjcj-stage1") == nullptr && std::strstr(p, "/cjc") == nullptr) {
            // still accept first r-xp of current executable
            continue;
        }
        if (perms[0] != 'r' || perms[1] != 'w') {
            // prefer data mapping for .bss; keep scanning for rw
            if (!(perms[0] == 'r' && perms[1] == '-')) {
                // also allow r-x to get base
            }
        }
        // Prefer rw-p mapping of stage1 (holds .bss)
        if (perms[0] == 'r' && perms[1] == 'w' && std::strstr(p, "cjcj-stage1") != nullptr) {
            out.start = start;
            out.end = end;
            out.fileOffBase = offset;
            std::snprintf(out.path, sizeof(out.path), "%s", p);
            out.ok = true;
            // keep first rw stage1
            break;
        }
    }
    fclose(f);
    return out.ok;
}

// From gcroottbl: VMA = file_off + 0x1000 for that image; bad file_offs:
// 0x259a5d0, 0x259aa20, 0x259aa28, 0x259aa30, 0x259aaa0, 0x259ac08
// Also sample good cluster: 0x259a170.. for contrast.
struct KnownOff {
    uintptr_t fileOff;
    const char* name;
    const char* pkg;
};

const KnownOff kKnown[] = {
    {0x259a5d0ULL, "globalState", "chir"},
    {0x259aa20ULL, "theQuestTy", "sema"},
    {0x259aa28ULL, "theCStringTy", "sema"},
    {0x259aa30ULL, "primitiveTys", "sema"},
    {0x259aaa0ULL, "MacroProcMsger.instance", "macro"},
    {0x259ac08ULL, "OP_KIND_MAP", "modules"},
    // Exact good controls from gcaddrdelta (all are precise GC_ROOT_TABLE entries).
    {0x259a170ULL, "X86_64_TARGET_CPUS", "good"},
    {0x259a188ULL, "AARCH64_TARGET_CPUS", "good"},
    {0x259a1a0ULL, "OPTIMIZATION_LEVEL_TO_BACKEND_OPTION", "good"},
    {0x259a1e0ULL, "g_cjdAstCache", "good"},
    {0x259a258ULL, "INTEGER_CONVERT_MAP", "good"},
    {0x259a318ULL, "G_FLOAT2INT_BOUND_MAP", "good"},
};
constexpr size_t kKnownN = sizeof(kKnown) / sizeof(kKnown[0]);

void ResolveFromKnownOffsets()
{
    if (g_resolvedOnce.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    ImageBase img{};
    if (!FindMainImage(img)) {
        std::fprintf(stderr, "[GCINITWIN] RESOLVE_FAIL reason=no_stage1_rw_map\n");
        return;
    }
    // maps offset for start corresponds to file_off of that page; slot_va = start + (fileOff - maps_offset)
    size_t added = 0;
    uintptr_t watchMin = ~static_cast<uintptr_t>(0);
    uintptr_t watchMax = 0;
    for (size_t i = 0; i < kKnownN && added < kWatchN; ++i) {
        if (kKnown[i].fileOff < img.fileOffBase) {
            continue;
        }
        uintptr_t va = img.start + (kKnown[i].fileOff - img.fileOffBase);
        if (va < img.start || va >= img.end) {
            continue;
        }
        WatchSlot& w = g_watch[added];
        w.addr = reinterpret_cast<const void*>(va);
        watchMin = va < watchMin ? va : watchMin;
        watchMax = va > watchMax ? va : watchMax;
        CopyStr(w.name, sizeof(w.name), kKnown[i].name);
        CopyStr(w.packageHint, sizeof(w.packageHint), kKnown[i].pkg);
        w.writeCount.store(0, std::memory_order_relaxed);
        w.lastValue.store(0, std::memory_order_relaxed);
        w.lastPhase.store(0, std::memory_order_relaxed);
        w.lastValueClass.store(0, std::memory_order_relaxed);
        w.seenNonNull.store(0, std::memory_order_relaxed);
        w.seenPlausible.store(0, std::memory_order_relaxed);
        w.resolved.store(1, std::memory_order_relaxed);
        ResetSnapshot(w, 0);
        for (size_t moment = 0; moment < SNAP_COUNT; ++moment) {
            w.snapshotCount[moment].store(0, std::memory_order_relaxed);
        }
        w.aToBPreCompared.store(0, std::memory_order_relaxed);
        w.aToBPreChanged.store(0, std::memory_order_relaxed);
        w.aToBPostCompared.store(0, std::memory_order_relaxed);
        w.aToBPostChanged.store(0, std::memory_order_relaxed);
        w.bPostToCCompared.store(0, std::memory_order_relaxed);
        w.bPostToCChanged.store(0, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[GCINITWIN] WATCH_SLOT idx=%zu name=%s pkg=%s va=%p file_off=0x%llx maps=%s "
                     "map_start=0x%llx map_off=0x%llx\n",
                     added, w.name, w.packageHint, w.addr,
                     static_cast<unsigned long long>(kKnown[i].fileOff), img.path,
                     static_cast<unsigned long long>(img.start),
                     static_cast<unsigned long long>(img.fileOffBase));
        // snapshot current value
        uintptr_t cur = *reinterpret_cast<const uintptr_t*>(w.addr);
        w.lastValue.store(cur, std::memory_order_relaxed);
        w.lastValueClass.store(ClassifyValue(cur), std::memory_order_relaxed);
        std::fprintf(stderr, "[GCINITWIN] WATCH_SNAPSHOT name=%s va=%p value=0x%llx vclass=%s phase=%s\n",
                     w.name, w.addr, static_cast<unsigned long long>(cur),
                     ValueClassName(ClassifyValue(cur)), PhaseName(CurrentPhase()));
        ++added;
    }
    g_watchMin.store(added == 0 ? 0 : watchMin, std::memory_order_release);
    g_watchMax.store(watchMax, std::memory_order_release);
    g_watchCount.store(added, std::memory_order_release);
    std::fprintf(stderr, "[GCINITWIN] RESOLVE_DONE count=%zu image=%s\n", added, img.path);
}

void EmitWrite(const void* field, uintptr_t value, const char* site, int watchIdx)
{
    uint64_t n = g_writeEmitted.fetch_add(1, std::memory_order_relaxed);
    if (n >= kWriteCap) {
        return;
    }
    WriteRec& r = g_writes[n];
    r.field = field;
    r.value = value;
    r.seq = g_writeSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    r.phase = CurrentPhase();
    r.valueClass = ClassifyValue(value);
    r.watchIdx = watchIdx >= 0 ? static_cast<uint8_t>(watchIdx) : 0xff;
    CopyStr(r.site, sizeof(r.site), site);
    if (watchIdx >= 0) {
        CopyStr(r.slotName, sizeof(r.slotName), g_watch[watchIdx].name);
    } else {
        CopyStr(r.slotName, sizeof(r.slotName), "(unwatched)");
    }
    if (g_activeInitSet.load(std::memory_order_acquire)) {
        CopyStr(r.initFile, sizeof(r.initFile), g_activeInitFile);
    } else {
        r.initFile[0] = '\0';
    }
    std::fprintf(stderr,
                 "[GCINITWIN] SLOT_WRITE seq=%u site=%s slot=%p name=%s value=0x%llx vclass=%s "
                 "phase=%s init_file=%s depth=%u completed=%llu\n",
                 r.seq, r.site, field, r.slotName, static_cast<unsigned long long>(value),
                 ValueClassName(r.valueClass), PhaseName(r.phase),
                 r.initFile[0] ? r.initFile : "-",
                 g_initDepth.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(g_initCompleted.load(std::memory_order_relaxed)));
}

void EmitLife(const void* slot, uintptr_t value, uint8_t tiClass, const char* point, const char* kind, int watchIdx)
{
    uint64_t n = g_lifeEmitted.fetch_add(1, std::memory_order_relaxed);
    if (n >= kLifeCap) {
        return;
    }
    LifeRec& r = g_lives[n];
    r.slot = slot;
    r.value = value;
    r.seq = g_lifeSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    r.phase = CurrentPhase();
    r.tiClass = tiClass;
    r.valueClass = ClassifyValue(value);
    CopyStr(r.kind, sizeof(r.kind), kind);
    CopyStr(r.point, sizeof(r.point), point);
    uint64_t writes = 0;
    if (watchIdx >= 0) {
        CopyStr(r.slotName, sizeof(r.slotName), g_watch[watchIdx].name);
        writes = g_watch[watchIdx].writeCount.load(std::memory_order_relaxed);
    } else {
        CopyStr(r.slotName, sizeof(r.slotName), "(unwatched)");
    }
    r.writesSeen = writes;
    if (g_activeInitSet.load(std::memory_order_acquire)) {
        CopyStr(r.initFile, sizeof(r.initFile), g_activeInitFile);
    } else {
        r.initFile[0] = '\0';
    }
    // lifecycle classification
    uint8_t life = 4;
    if (writes == 0 && r.valueClass == 0) {
        life = 0;
    } else if (writes == 0 && r.valueClass != 0 && r.valueClass != 4) {
        // non-zero garbage without observed WriteStaticRef ⇒ not simple zero-bss; writer missed or non-barrier store
        life = 0; // never via static-ref path
        // refine tag in message
    } else if (r.phase == 1) {
        life = 1;
    } else if (r.phase == 2 || r.phase == 3) {
        life = (r.valueClass == 4) ? 3 : 2;
    } else if (r.phase == 0) {
        life = (writes > 0) ? 1 : 0;
    }
    r.lifecycle = life;
    std::fprintf(stderr,
                 "[GCINITWIN] SLOT_LIFE seq=%u kind=%s point=%s slot=%p name=%s value=0x%llx "
                 "vclass=%s ti_class=%u phase=%s lifecycle=%s writes=%llu init_file=%s depth=%u completed=%llu\n",
                 r.seq, r.kind, r.point, slot, r.slotName, static_cast<unsigned long long>(value),
                 ValueClassName(r.valueClass), static_cast<unsigned>(tiClass), PhaseName(r.phase),
                 LifecycleName(life), static_cast<unsigned long long>(writes),
                 r.initFile[0] ? r.initFile : "-",
                 g_initDepth.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(g_initCompleted.load(std::memory_order_relaxed)));
}

} // namespace

bool ProbeOn()
{
    int c = g_on.load(std::memory_order_relaxed);
    if (c >= 0) {
        return c == 1;
    }
    const char* e = std::getenv("MRT_GCINITWIN");
    const char* d = std::getenv("MRT_GCDISPEL");
    int on = 0;
    if (e != nullptr && std::strcmp(e, "1") == 0) {
        on = 1;
    } else if (d != nullptr && std::strcmp(d, "1") == 0) {
        // co-enable with existing diag load for this lane
        on = 1;
    }
    if (on == 1) {
        std::atexit([]() { DumpSummary("atexit"); });
    }
    g_on.store(on, std::memory_order_relaxed);
    return on == 1;
}

void NoteGlobalInitBegin(const char* fileBaseName)
{
    if (!ProbeOn()) {
        return;
    }
    TryResolveWatchSlots();
    uint32_t depth = g_initDepth.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_initBeginTotal.fetch_add(1, std::memory_order_relaxed);
    CopyStr(g_activeInitFile, sizeof(g_activeInitFile), fileBaseName != nullptr ? fileBaseName : "?");
    g_activeInitSet.store(true, std::memory_order_release);
    uint64_t n = g_initEventEmitted.fetch_add(1, std::memory_order_relaxed);
    if (n < kInitEventCap) {
        InitEvent& ev = g_inits[n];
        CopyStr(ev.file, sizeof(ev.file), g_activeInitFile);
        ev.seq = g_initEventSeq.fetch_add(1, std::memory_order_relaxed) + 1;
        ev.begin = 1;
        ev.ok = 1;
        ev.depthAfter = depth;
        ev.completedAfter = g_initCompleted.load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[GCINITWIN] GLOBAL_INIT begin=1 file=%s depth=%u completed=%llu seq=%u\n",
                     ev.file, depth,
                     static_cast<unsigned long long>(ev.completedAfter), ev.seq);
    }
    // snapshot watched slots at begin
    size_t wn = g_watchCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < wn && i < kWatchN; ++i) {
        if (g_watch[i].addr == nullptr) {
            continue;
        }
        uintptr_t cur = *reinterpret_cast<const uintptr_t*>(g_watch[i].addr);
        std::fprintf(stderr,
                     "[GCINITWIN] INIT_SNAP when=begin file=%s name=%s va=%p value=0x%llx vclass=%s\n",
                     g_activeInitFile, g_watch[i].name, g_watch[i].addr,
                     static_cast<unsigned long long>(cur), ValueClassName(ClassifyValue(cur)));
    }
}

void NoteGlobalInitEnd(const char* fileBaseName, bool ok)
{
    if (!ProbeOn()) {
        return;
    }
    uint32_t depth = g_initDepth.load(std::memory_order_acquire);
    if (depth > 0) {
        depth = g_initDepth.fetch_sub(1, std::memory_order_acq_rel) - 1;
    }
    uint64_t completed = g_initCompleted.fetch_add(1, std::memory_order_acq_rel) + 1;
    // snapshot watched
    size_t wn = g_watchCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < wn && i < kWatchN; ++i) {
        if (g_watch[i].addr == nullptr) {
            continue;
        }
        uintptr_t cur = *reinterpret_cast<const uintptr_t*>(g_watch[i].addr);
        g_watch[i].lastValue.store(cur, std::memory_order_relaxed);
        g_watch[i].lastValueClass.store(ClassifyValue(cur), std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[GCINITWIN] INIT_SNAP when=end file=%s ok=%u name=%s va=%p value=0x%llx vclass=%s\n",
                     fileBaseName != nullptr ? fileBaseName : "?", static_cast<unsigned>(ok),
                     g_watch[i].name, g_watch[i].addr, static_cast<unsigned long long>(cur),
                     ValueClassName(ClassifyValue(cur)));
    }
    uint64_t n = g_initEventEmitted.fetch_add(1, std::memory_order_relaxed);
    if (n < kInitEventCap) {
        InitEvent& ev = g_inits[n];
        CopyStr(ev.file, sizeof(ev.file), fileBaseName != nullptr ? fileBaseName : "?");
        ev.seq = g_initEventSeq.fetch_add(1, std::memory_order_relaxed) + 1;
        ev.begin = 0;
        ev.ok = ok ? 1 : 0;
        ev.depthAfter = depth;
        ev.completedAfter = completed;
        std::fprintf(stderr,
                     "[GCINITWIN] GLOBAL_INIT begin=0 file=%s ok=%u depth=%u completed=%llu seq=%u\n",
                     ev.file, static_cast<unsigned>(ev.ok), depth,
                     static_cast<unsigned long long>(completed), ev.seq);
    }
    if (depth == 0) {
        g_activeInitSet.store(false, std::memory_order_release);
    }
}

void NoteStaticRefWrite(const void* field, const void* ref, const char* site)
{
    if (!ProbeOn() || field == nullptr) {
        return;
    }
    TryResolveWatchSlots();
    g_writeTotal.fetch_add(1, std::memory_order_relaxed);
    uintptr_t value = reinterpret_cast<uintptr_t>(ref);
    uint8_t vc = ClassifyValue(value);
    if (vc != 0 && vc != 4) {
        g_writeGarbageVal.fetch_add(1, std::memory_order_relaxed);
    }
    int wi = FindWatch(field);
    RecordWritePath(site, wi >= 0 ? 1 : 0);
    if (wi >= 0) {
        g_writeWatched.fetch_add(1, std::memory_order_relaxed);
        g_watch[wi].writeCount.fetch_add(1, std::memory_order_relaxed);
        g_watch[wi].lastValue.store(value, std::memory_order_relaxed);
        g_watch[wi].lastPhase.store(CurrentPhase(), std::memory_order_relaxed);
        g_watch[wi].lastValueClass.store(vc, std::memory_order_relaxed);
        if (value != 0) {
            g_watch[wi].seenNonNull.store(1, std::memory_order_relaxed);
        }
        if (vc == 4) {
            g_watch[wi].seenPlausible.store(1, std::memory_order_relaxed);
        }
        EmitWrite(field, value, site, wi);
        return;
    }
    // sample unwatched garbage / early writes
    static std::atomic<uint64_t> unwatchedSamples{0};
    bool take = false;
    if (vc != 0 && vc != 4) {
        take = unwatchedSamples.fetch_add(1, std::memory_order_relaxed) < 16;
    } else if (g_writeEmitted.load(std::memory_order_relaxed) < 8) {
        take = unwatchedSamples.fetch_add(1, std::memory_order_relaxed) < 8;
    }
    if (take) {
        EmitWrite(field, value, site, -1);
    }
}

void NoteWriteGeneric(const void* obj, const void* field, size_t size, uint8_t gcPhase)
{
    if (!ProbeOn()) {
        return;
    }
    g_wgTotal.fetch_add(1, std::memory_order_relaxed);
    if (gcPhase < kWgPhaseN) {
        g_wgPhaseTotal[gcPhase].fetch_add(1, std::memory_order_relaxed);
    }
    // Sample every write into ring (overwrite old). Cost: one atomic index + store.
    uint64_t n = g_wgRingN.fetch_add(1, std::memory_order_relaxed);
    WgRec& r = g_wgRing[n % kWgRingCap];
    r.obj = obj;
    r.field = field;
    r.size = size;
    r.phase = gcPhase;
    r.seq = static_cast<uint32_t>(n + 1);
}

// Classify TypeInfo pointer without calling real IsVaildType / HasRefField (observe only).
// 1=plausible mapped TI, 0=null/low/noncanon/unmapped-like.
uint8_t ClassifyTiPtrObserve(uintptr_t typeAddr)
{
    if (typeAddr == 0) {
        return 0;
    }
    if (typeAddr < 0x1000ULL) {
        return 0;
    }
    if (typeAddr >= (1ULL << 48)) {
        return 0;
    }
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        pageSize = 4096;
    }
    unsigned char vec = 0;
    uintptr_t page = typeAddr & ~static_cast<uintptr_t>(pageSize - 1);
    if (mincore(reinterpret_cast<void*>(page), static_cast<size_t>(pageSize), &vec) != 0) {
        return 0;
    }
    if ((vec & 1) == 0) {
        return 0;
    }
    return 1;
}

bool PageReadableAt(const void* p)
{
    if (p == nullptr) {
        return false;
    }
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        pageSize = 4096;
    }
    unsigned char vec = 0;
    uintptr_t page = reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(pageSize - 1);
    if (mincore(reinterpret_cast<void*>(page), static_cast<size_t>(pageSize), &vec) != 0) {
        return false;
    }
    return (vec & 1) != 0;
}

void NoteFixWrite(const void* slot, const void* oldValue, const void* target, const void* current,
                  uint8_t didForward, uint8_t ghostFrom, uint8_t unmovable,
                  const MinorTargetFate& currentFate)
{
    if (!ProbeOn() || slot == nullptr) {
        return;
    }
    TryResolveWatchSlots();
    uint64_t total = g_fixWriteTotal.fetch_add(1, std::memory_order_relaxed) + 1;
    if (didForward != 0) {
        g_fixWriteDidForward.fetch_add(1, std::memory_order_relaxed);
    }

    uintptr_t curTi = 0;
    uint8_t tiValid = 0;
    if (current != nullptr && currentFate.heap != 0 && currentFate.inAllocRange != 0 &&
        currentFate.freeRegion == 0 && currentFate.garbageRegion == 0 && PageReadableAt(current)) {
        // Read StateWord type-info bits the same way BaseObject::GetTypeInfo would (observe only).
        // Layout: typeInfoLow32 | typeInfoHigh16 | objectState (StateWord.h:173-175).
        uintptr_t head = *reinterpret_cast<const uintptr_t*>(current);
        curTi = head & ((1ULL << 48) - 1ULL);
        tiValid = ClassifyTiPtrObserve(curTi);
    }
    if (tiValid != 0) {
        g_fixWriteValid.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_fixWriteInvalid.fetch_add(1, std::memory_order_relaxed);
    }

    // Always record current into ring for later crash-addr equality.
    uint64_t rn = g_fixWriteRingN.fetch_add(1, std::memory_order_relaxed);
    g_fixWriteRing[rn % kFixWriteRingCap] = current;

    int wi = FindWatch(slot);
    uint64_t emitted = g_fixWriteEmitted.fetch_add(1, std::memory_order_relaxed);
    // Prefer: watched slots always; invalid TI always (bounded); first N overall.
    bool emit = (emitted < kFixWriteSampleCap) || (wi >= 0 && emitted < kFixWriteSampleCap * 2) ||
                (tiValid == 0 && emitted < kFixWriteSampleCap * 3);
    if (!emit) {
        return;
    }
    size_t idx = emitted < kFixWriteSampleCap ? static_cast<size_t>(emitted) : (emitted % kFixWriteSampleCap);
    FixWriteRec& r = g_fixWriteSamples[idx];
    r.slot = slot;
    r.oldValue = oldValue;
    r.target = target;
    r.current = current;
    r.currentTi = curTi;
    r.tiValid = tiValid;
    r.didForward = didForward;
    r.ghostFrom = ghostFrom;
    r.unmovable = unmovable;
    r.young = currentFate.young;
    r.validRegion = currentFate.validRegion;
    r.freeRegion = currentFate.freeRegion;
    r.garbageRegion = currentFate.garbageRegion;
    r.inAlloc = currentFate.inAllocRange;
    r.regionType = currentFate.regionType;
    r.watchIdx = wi >= 0 ? static_cast<uint8_t>(wi) : 0xff;
    r.seq = static_cast<uint32_t>(total);
    r.round = g_minorRound.load(std::memory_order_acquire);
    r.slotName[0] = '\0';
    if (wi >= 0) {
        std::strncpy(r.slotName, g_watch[wi].name, kNameCap - 1);
        r.slotName[kNameCap - 1] = '\0';
    }

    std::fprintf(stderr,
                 "[GCFWDWRITE] FIXWRITE seq=%u round=%llu slot=%p name=%s old=%p target=%p current=%p "
                 "ti=0x%llx ti_valid=%u did_fwd=%u ghost=%u unmov=%u young=%u valid=%u free=%u garbage=%u "
                 "in_alloc=%u rtype=%u\n",
                 r.seq, static_cast<unsigned long long>(r.round), slot,
                 r.slotName[0] != '\0' ? r.slotName : "?",
                 oldValue, target, current, static_cast<unsigned long long>(curTi),
                 static_cast<unsigned>(tiValid), static_cast<unsigned>(didForward),
                 static_cast<unsigned>(ghostFrom), static_cast<unsigned>(unmovable),
                 static_cast<unsigned>(currentFate.young), static_cast<unsigned>(currentFate.validRegion),
                 static_cast<unsigned>(currentFate.freeRegion),
                 static_cast<unsigned>(currentFate.garbageRegion),
                 static_cast<unsigned>(currentFate.inAllocRange),
                 static_cast<unsigned>(currentFate.regionType));
}

void NoteOldTargetAtBadTi(const void* target, uintptr_t typeAddr, const char* point,
                          const MinorTargetFate& fate)
{
    if (!ProbeOn() || target == nullptr) {
        return;
    }
    g_badTiEvents.fetch_add(1, std::memory_order_relaxed);
    uint64_t emitN = g_oldIdEmitted.fetch_add(1, std::memory_order_relaxed);
    if (emitN >= kOldIdCap) {
        return;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(target);
    uintptr_t regionStart = fate.regionStart;
    uintptr_t regionAlloc = fate.regionAlloc;
    ptrdiff_t offFromStart = (regionStart != 0) ? static_cast<ptrdiff_t>(addr - regionStart) : -1;
    ptrdiff_t offFromAlloc = (regionAlloc != 0) ? static_cast<ptrdiff_t>(addr - regionAlloc) : -1;

    // 64B before + 64B at target (16 words). Bound: only when page-readable.
    uint64_t raw[kRawWordN] = {};
    bool rawOk = false;
    {
        long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0) {
            pageSize = 4096;
        }
        uintptr_t base = addr >= 64 ? addr - 64 : addr;
        uintptr_t page = base & ~static_cast<uintptr_t>(pageSize - 1);
        unsigned char vec = 0;
        if (mincore(reinterpret_cast<void*>(page), static_cast<size_t>(pageSize), &vec) == 0) {
            const uint64_t* p = reinterpret_cast<const uint64_t*>(base);
            for (size_t i = 0; i < kRawWordN; ++i) {
                raw[i] = p[i];
            }
            rawOk = true;
        }
    }

    // Classify memory identity from region flags + raw head shape.
    const char* identity = "undetermined";
    if (fate.heap == 0) {
        identity = "not_heap";
    } else if (fate.region == nullptr) {
        identity = "no_region";
    } else if (fate.freeRegion != 0) {
        identity = "free_region";
    } else if (fate.garbageRegion != 0) {
        identity = "garbage_region";
    } else if (fate.validRegion == 0) {
        identity = "invalid_region";
    } else if (fate.inAllocRange == 0) {
        identity = "outside_alloc_range";
    } else if (fate.young != 0) {
        identity = "young_in_alloc";
    } else {
        // old + valid + in_alloc — refine by head shape
        uint64_t head = rawOk ? raw[8] : typeAddr; // word at target when base=addr-64
        if (!rawOk) {
            head = typeAddr;
        }
        if (head == 0) {
            identity = "old_zero_head_in_alloc";
        } else if (head < 0x1000ULL || head == 0xffffffffffffULL || head == ~0ULL) {
            // free-list-like: next at +8 often a heap pointer
            uint64_t next = rawOk ? raw[9] : 0;
            if (next >= 0x1000ULL && next < (1ULL << 48) && (next & 0x7ULL) == 0) {
                identity = "old_slotlist_like_in_alloc";
            } else {
                identity = "old_garbage_ti_in_alloc";
            }
        } else if ((addr & 0x7ULL) != 0) {
            identity = "old_misaligned_interior";
        } else {
            identity = "old_linked_alloc_payload";
        }
    }

    // Reverse-scan WriteGeneric ring for last write whose [field, field+size) covers target
    // or whose obj == target (struct write into this object).
    const char* lastWgPhase = "none";
    uint8_t lastWgPhaseId = 0xff;
    uint32_t lastWgSeq = 0;
    const void* lastWgField = nullptr;
    size_t lastWgSize = 0;
    uint64_t ringN = g_wgRingN.load(std::memory_order_acquire);
    size_t scan = ringN < kWgRingCap ? static_cast<size_t>(ringN) : kWgRingCap;
    for (size_t i = 0; i < scan; ++i) {
        size_t idx = (ringN - 1 - i) % kWgRingCap;
        const WgRec& r = g_wgRing[idx];
        if (r.obj == nullptr && r.field == nullptr) {
            continue;
        }
        bool hit = false;
        if (r.obj == target) {
            hit = true;
        } else if (r.field != nullptr) {
            uintptr_t f0 = reinterpret_cast<uintptr_t>(r.field);
            uintptr_t f1 = f0 + r.size;
            if (addr >= f0 && addr < f1) {
                hit = true;
            }
        }
        if (hit) {
            lastWgPhaseId = r.phase;
            lastWgPhase = GcPhaseNameLocal(r.phase);
            lastWgSeq = r.seq;
            lastWgField = r.field;
            lastWgSize = r.size;
            break;
        }
    }

    // Q2: is this crash target equal to some current written by FixMinorEvacuatedSlot?
    uint8_t crashEq = 0;
    uint64_t fwRingN = g_fixWriteRingN.load(std::memory_order_acquire);
    size_t scanFw = fwRingN < kFixWriteRingCap ? static_cast<size_t>(fwRingN) : kFixWriteRingCap;
    for (size_t i = 0; i < scanFw; ++i) {
        if (g_fixWriteRing[i % kFixWriteRingCap] == target) {
            crashEq = 1;
            break;
        }
    }
    g_crashEqualsFixWriteChecked.fetch_add(1, std::memory_order_relaxed);
    if (crashEq != 0) {
        g_crashEqualsFixWriteYes.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_crashEqualsFixWriteNo.fetch_add(1, std::memory_order_relaxed);
    }

    std::fprintf(stderr,
                 "[GCOLDREG] OLD_TARGET_MEMORY identity=%s point=%s target=%p ti=0x%llx "
                 "region=%p rtype=%u young=%u valid=%u free=%u garbage=%u heap=%u "
                 "in_alloc=%u marked=%u state=%u start=0x%llx alloc=0x%llx "
                 "off_start=%td off_alloc=%td last_wg_phase=%s last_wg_phase_id=%u "
                 "last_wg_seq=%u last_wg_field=%p last_wg_size=%zu raw_ok=%u "
                 "crash_eq_fixwrite=%u\n",
                 identity, point != nullptr ? point : "?", target,
                 static_cast<unsigned long long>(typeAddr), fate.region,
                 static_cast<unsigned>(fate.regionType), static_cast<unsigned>(fate.young),
                 static_cast<unsigned>(fate.validRegion), static_cast<unsigned>(fate.freeRegion),
                 static_cast<unsigned>(fate.garbageRegion), static_cast<unsigned>(fate.heap),
                 static_cast<unsigned>(fate.inAllocRange), static_cast<unsigned>(fate.marked),
                 static_cast<unsigned>(fate.state),
                 static_cast<unsigned long long>(regionStart),
                 static_cast<unsigned long long>(regionAlloc), offFromStart, offFromAlloc,
                 lastWgPhase, static_cast<unsigned>(lastWgPhaseId), lastWgSeq, lastWgField,
                 lastWgSize, rawOk ? 1u : 0u, static_cast<unsigned>(crashEq));
    std::fprintf(stderr,
                 "[GCFWDWRITE] CRASH_EQ_FIXWRITE target=%p eq=%u checked=%llu yes=%llu no=%llu "
                 "fixwrite_ring_n=%llu\n",
                 target, static_cast<unsigned>(crashEq),
                 static_cast<unsigned long long>(
                     g_crashEqualsFixWriteChecked.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_crashEqualsFixWriteYes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_crashEqualsFixWriteNo.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(fwRingN));

    if (rawOk) {
        std::fprintf(stderr,
                     "[GCOLDREG] OLD_TARGET_RAW target=%p "
                     "m64=0x%llx m56=0x%llx m48=0x%llx m40=0x%llx m32=0x%llx m24=0x%llx m16=0x%llx m8=0x%llx "
                     "p0=0x%llx p8=0x%llx p16=0x%llx p24=0x%llx p32=0x%llx p40=0x%llx p48=0x%llx p56=0x%llx\n",
                     target,
                     static_cast<unsigned long long>(raw[0]), static_cast<unsigned long long>(raw[1]),
                     static_cast<unsigned long long>(raw[2]), static_cast<unsigned long long>(raw[3]),
                     static_cast<unsigned long long>(raw[4]), static_cast<unsigned long long>(raw[5]),
                     static_cast<unsigned long long>(raw[6]), static_cast<unsigned long long>(raw[7]),
                     static_cast<unsigned long long>(raw[8]), static_cast<unsigned long long>(raw[9]),
                     static_cast<unsigned long long>(raw[10]), static_cast<unsigned long long>(raw[11]),
                     static_cast<unsigned long long>(raw[12]), static_cast<unsigned long long>(raw[13]),
                     static_cast<unsigned long long>(raw[14]), static_cast<unsigned long long>(raw[15]));
    }

    // On bad-TI also dump current watched-slot a/b/c for this round (faulty-round Q3).
    size_t wn = g_watchCount.load(std::memory_order_acquire);
    uint64_t round = g_minorRound.load(std::memory_order_acquire);
    for (size_t i = 0; i < wn && i < kWatchN; ++i) {
        WatchSlot& w = g_watch[i];
        if (w.addr == nullptr || w.snapshotRound != round) {
            continue;
        }
        if ((w.snapshotMask & 0xfU) == 0) {
            continue;
        }
        if (g_faultyRoundSlotDeltas.fetch_add(1, std::memory_order_relaxed) >= 64) {
            break;
        }
        uintptr_t cur = *reinterpret_cast<const uintptr_t*>(w.addr);
        std::fprintf(stderr,
                     "[GCOLDREG] SLOT_DELTA_FAULTY round=%llu class=%s name=%s mask=0x%x "
                     "a=0x%llx b_pre=0x%llx b_post=0x%llx c=0x%llx cur=0x%llx "
                     "a_to_b=%u b_to_c=%u a_target=%p a_young=%u a_rtype=%u\n",
                     static_cast<unsigned long long>(round),
                     i < kBadWatchN ? "bad" : "good", w.name,
                     static_cast<unsigned>(w.snapshotMask),
                     static_cast<unsigned long long>(w.snapshotValue[SNAP_A]),
                     static_cast<unsigned long long>(w.snapshotValue[SNAP_B_PRE]),
                     static_cast<unsigned long long>(w.snapshotValue[SNAP_B_POST]),
                     static_cast<unsigned long long>(w.snapshotValue[SNAP_C]),
                     static_cast<unsigned long long>(cur),
                     static_cast<unsigned>((w.snapshotMask & 0x3U) == 0x3U &&
                                           w.snapshotValue[SNAP_A] != w.snapshotValue[SNAP_B_PRE]),
                     static_cast<unsigned>((w.snapshotMask & 0xcU) == 0xcU &&
                                           w.snapshotValue[SNAP_B_POST] != w.snapshotValue[SNAP_C]),
                     w.aFate.target, static_cast<unsigned>(w.aFate.young),
                     static_cast<unsigned>(w.aFate.regionType));
    }
}

void NoteStaticStructWrite(const void* dst, size_t dstLen, const void* src, const char* site)
{
    if (!ProbeOn() || dst == nullptr) {
        return;
    }
    TryResolveWatchSlots();
    g_structWriteTotal.fetch_add(1, std::memory_order_relaxed);
    // if any watched slot overlaps [dst, dst+dstLen), log
    uintptr_t d0 = reinterpret_cast<uintptr_t>(dst);
    uintptr_t d1 = d0 + dstLen;
    uint8_t path = WritePathId(site);
    g_pathTotal[path].fetch_add(1, std::memory_order_relaxed);
    uintptr_t watchMin = g_watchMin.load(std::memory_order_acquire);
    uintptr_t watchMax = g_watchMax.load(std::memory_order_acquire);
    if (watchMin == 0 || d1 <= watchMin || d0 > watchMax) {
        return;
    }
    size_t wn = g_watchCount.load(std::memory_order_acquire);
    size_t watched = 0;
    for (size_t i = 0; i < wn && i < kWatchN; ++i) {
        uintptr_t a = reinterpret_cast<uintptr_t>(g_watch[i].addr);
        if (a >= d0 && a < d1) {
            ++watched;
            uintptr_t value = 0;
            if (src != nullptr) {
                value = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<const char*>(src) + (a - d0));
            }
            g_watch[i].writeCount.fetch_add(1, std::memory_order_relaxed);
            g_writeWatched.fetch_add(1, std::memory_order_relaxed);
            g_watch[i].lastValue.store(value, std::memory_order_relaxed);
            g_watch[i].lastPhase.store(CurrentPhase(), std::memory_order_relaxed);
            g_watch[i].lastValueClass.store(ClassifyValue(value), std::memory_order_relaxed);
            char site2[32];
            std::snprintf(site2, sizeof(site2), "%s+struct", site != nullptr ? site : "struct");
            EmitWrite(g_watch[i].addr, value, site2, static_cast<int>(i));
        }
    }
    if (watched != 0) {
        g_pathWatched[path].fetch_add(watched, std::memory_order_relaxed);
    }
}

void NoteMinorCycleStart(uint64_t round)
{
    if (!ProbeOn()) {
        return;
    }
    TryResolveWatchSlots();
    g_minorRound.store(round, std::memory_order_release);
    size_t wn = g_watchCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < wn && i < kWatchN; ++i) {
        ResetSnapshot(g_watch[i], round);
    }
    std::fprintf(stderr, "[GCSLOTDELTA] MINOR_CYCLE_START round=%llu watch_n=%zu\n",
                 static_cast<unsigned long long>(round), wn);
}

uint64_t CurrentMinorRound()
{
    return g_minorRound.load(std::memory_order_acquire);
}

const void* InitialMinorTarget(const void* slot, uint64_t round)
{
    if (!ProbeOn() || slot == nullptr) {
        return nullptr;
    }
    TryResolveWatchSlots();
    int wi = FindWatch(slot);
    if (wi < 0 || g_watch[wi].snapshotRound != round || (g_watch[wi].snapshotMask & (1U << SNAP_A)) == 0) {
        return nullptr;
    }
    return g_watch[wi].aFate.target;
}

void NoteMinorSlotSnapshot(const void* slot, uint64_t round, const char* moment,
                           const MinorTargetFate& currentFate, const MinorTargetFate& initialFate)
{
    if (!ProbeOn() || slot == nullptr || round == 0) {
        return;
    }
    TryResolveWatchSlots();
    int wi = FindWatch(slot);
    int momentId = SnapshotMomentId(moment);
    if (wi < 0 || momentId < 0) {
        return;
    }
    WatchSlot& watch = g_watch[wi];
    if (watch.snapshotRound != round) {
        ResetSnapshot(watch, round);
    }
    uint8_t bit = static_cast<uint8_t>(1U << momentId);
    if ((watch.snapshotMask & bit) != 0) {
        return;
    }
    uintptr_t value = *reinterpret_cast<const uintptr_t*>(slot);
    watch.snapshotMask |= bit;
    watch.snapshotValue[momentId] = value;
    watch.snapshotCount[momentId].fetch_add(1, std::memory_order_relaxed);
    if (momentId == SNAP_A) {
        watch.aFate = currentFate;
    } else if ((watch.snapshotMask & (1U << SNAP_A)) != 0) {
        if (momentId == SNAP_B_PRE) {
            watch.aToBPreCompared.fetch_add(1, std::memory_order_relaxed);
            if (watch.snapshotValue[SNAP_A] != value) {
                watch.aToBPreChanged.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (momentId == SNAP_B_POST) {
            watch.aToBPostCompared.fetch_add(1, std::memory_order_relaxed);
            if (watch.snapshotValue[SNAP_A] != value) {
                watch.aToBPostChanged.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (momentId == SNAP_C) {
            int b = (watch.snapshotMask & (1U << SNAP_B_POST)) != 0 ? SNAP_B_POST : SNAP_B_PRE;
            if ((watch.snapshotMask & (1U << b)) != 0) {
                watch.bPostToCCompared.fetch_add(1, std::memory_order_relaxed);
                if (watch.snapshotValue[b] != value) {
                    watch.bPostToCChanged.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    uint64_t emitted = g_snapshotEmitted.fetch_add(1, std::memory_order_relaxed);
    if (emitted < kSnapshotCap) {
        std::fprintf(stderr,
                     "[GCSLOTDELTA] SLOT_SNAPSHOT round=%llu moment=%s class=%s name=%s slot=%p "
                     "value=0x%llx target=%p region=%p rtype=%u heap=%u valid=%u young=%u marked=%u "
                     "free=%u garbage=%u in_alloc=%u state=%u initial=%p initial_region=%p "
                     "initial_rtype=%u initial_young=%u initial_marked=%u initial_free=%u "
                     "initial_garbage=%u initial_state=%u\n",
                     static_cast<unsigned long long>(round), SnapshotMomentName(static_cast<uint8_t>(momentId)),
                     static_cast<size_t>(wi) < kBadWatchN ? "bad" : "good", watch.name, slot,
                     static_cast<unsigned long long>(value), currentFate.target, currentFate.region,
                     static_cast<unsigned>(currentFate.regionType), static_cast<unsigned>(currentFate.heap),
                     static_cast<unsigned>(currentFate.validRegion), static_cast<unsigned>(currentFate.young),
                     static_cast<unsigned>(currentFate.marked), static_cast<unsigned>(currentFate.freeRegion),
                     static_cast<unsigned>(currentFate.garbageRegion), static_cast<unsigned>(currentFate.inAllocRange),
                     static_cast<unsigned>(currentFate.state), initialFate.target, initialFate.region,
                     static_cast<unsigned>(initialFate.regionType), static_cast<unsigned>(initialFate.young),
                     static_cast<unsigned>(initialFate.marked), static_cast<unsigned>(initialFate.freeRegion),
                     static_cast<unsigned>(initialFate.garbageRegion), static_cast<unsigned>(initialFate.state));
    }

    if (momentId == SNAP_C && g_deltaEmitted.fetch_add(1, std::memory_order_relaxed) < kDeltaCap) {
        bool haveA = (watch.snapshotMask & (1U << SNAP_A)) != 0;
        bool haveBPre = (watch.snapshotMask & (1U << SNAP_B_PRE)) != 0;
        bool haveBPost = (watch.snapshotMask & (1U << SNAP_B_POST)) != 0;
        uintptr_t bValue = haveBPost ? watch.snapshotValue[SNAP_B_POST] : watch.snapshotValue[SNAP_B_PRE];
        bool recycled = haveA && (watch.aFate.region != initialFate.region ||
                        watch.aFate.regionType != initialFate.regionType || initialFate.freeRegion != 0 ||
                        initialFate.garbageRegion != 0);
        std::fprintf(stderr,
                     "[GCSLOTDELTA] SLOT_DELTA round=%llu class=%s name=%s slot=%p mask=0x%x "
                     "a=0x%llx b_pre=0x%llx b_post=0x%llx c=0x%llx a_to_b=%u b_to_c=%u "
                     "a_target=%p a_young=%u a_marked=%u a_region=%p a_rtype=%u "
                     "a_target_now_marked=%u a_target_now_state=%u a_region_recycled=%u\n",
                     static_cast<unsigned long long>(round), static_cast<size_t>(wi) < kBadWatchN ? "bad" : "good",
                     watch.name, slot, static_cast<unsigned>(watch.snapshotMask),
                     static_cast<unsigned long long>(watch.snapshotValue[SNAP_A]),
                     static_cast<unsigned long long>(watch.snapshotValue[SNAP_B_PRE]),
                     static_cast<unsigned long long>(watch.snapshotValue[SNAP_B_POST]),
                     static_cast<unsigned long long>(watch.snapshotValue[SNAP_C]),
                     static_cast<unsigned>(haveA && (haveBPre || haveBPost) &&
                                           watch.snapshotValue[SNAP_A] != bValue),
                     static_cast<unsigned>((haveBPre || haveBPost) && bValue != watch.snapshotValue[SNAP_C]),
                     watch.aFate.target, static_cast<unsigned>(watch.aFate.young),
                     static_cast<unsigned>(watch.aFate.marked), watch.aFate.region,
                     static_cast<unsigned>(watch.aFate.regionType), static_cast<unsigned>(initialFate.marked),
                     static_cast<unsigned>(initialFate.state), static_cast<unsigned>(recycled));
    }
}

void NoteStaticEnqueueLifecycle(const void* slot, const void* target, uint8_t tiClass, const char* point,
                                const char* kind, const MinorTargetFate& currentFate,
                                const MinorTargetFate& initialFate)
{
    if (!ProbeOn() || slot == nullptr) {
        return;
    }
    TryResolveWatchSlots();
    if (point != nullptr && std::strcmp(point, "after-dispel") == 0) {
        NoteMinorSlotSnapshot(slot, g_minorRound.load(std::memory_order_acquire), "c-enqueue",
                              currentFate, initialFate);
    }
    int wi = FindWatch(slot);
    uintptr_t value = reinterpret_cast<uintptr_t>(target);
    // also re-read slot
    uintptr_t slotVal = *reinterpret_cast<const uintptr_t*>(slot);
    if (value == 0) {
        value = slotVal;
    }
    // prefer watched + bad
    static std::atomic<uint64_t> lifeSamples{0};
    bool take = false;
    if (wi >= 0) {
        take = lifeSamples.fetch_add(1, std::memory_order_relaxed) < kLifeCap;
    } else if (tiClass != 0) {
        take = lifeSamples.fetch_add(1, std::memory_order_relaxed) < 16;
    }
    if (take) {
        EmitLife(slot, value, tiClass, point != nullptr ? point : "?", kind != nullptr ? kind : "?", wi);
    }
}

void TryResolveWatchSlots()
{
    if (!ProbeOn()) {
        return;
    }
    if (!g_resolvedOnce.load(std::memory_order_acquire)) {
        ResolveFromKnownOffsets();
    }
}

void DumpSummary(const char* reason)
{
    if (!ProbeOn()) {
        return;
    }
    if (g_summaryDumped.exchange(true, std::memory_order_relaxed)) {
        // allow multiple? still print once fully; second as short
    }
    std::fprintf(stderr,
                 "[GCINITWIN] SUMMARY reason=%s write_total=%llu write_watched=%llu write_garbage_val=%llu "
                 "struct_write=%llu life_emitted=%llu init_begin=%llu init_completed=%llu depth=%u watch_n=%zu\n",
                 reason != nullptr ? reason : "?",
                 static_cast<unsigned long long>(g_writeTotal.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_writeWatched.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_writeGarbageVal.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_structWriteTotal.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_lifeEmitted.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_initBeginTotal.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_initCompleted.load(std::memory_order_relaxed)),
                 g_initDepth.load(std::memory_order_relaxed),
                 g_watchCount.load(std::memory_order_relaxed));
    size_t controlComplete = 0;
    size_t wn = g_watchCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < wn && i < kWatchN; ++i) {
        uintptr_t cur = 0;
        if (g_watch[i].addr != nullptr) {
            cur = *reinterpret_cast<const uintptr_t*>(g_watch[i].addr);
        }
        std::fprintf(stderr,
                     "[GCINITWIN] WATCH_SUMMARY name=%s va=%p writes=%llu last=0x%llx last_vclass=%s "
                     "last_phase=%s seen_nonnull=%u seen_plausible=%u final=0x%llx final_vclass=%s\n",
                     g_watch[i].name, g_watch[i].addr,
                     static_cast<unsigned long long>(g_watch[i].writeCount.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_watch[i].lastValue.load(std::memory_order_relaxed)),
                     ValueClassName(g_watch[i].lastValueClass.load(std::memory_order_relaxed)),
                     PhaseName(g_watch[i].lastPhase.load(std::memory_order_relaxed)),
                     static_cast<unsigned>(g_watch[i].seenNonNull.load(std::memory_order_relaxed)),
                     static_cast<unsigned>(g_watch[i].seenPlausible.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(cur), ValueClassName(ClassifyValue(cur)));
        if (i >= kBadWatchN && (g_watch[i].snapshotMask & 0xfU) == 0xfU) {
            ++controlComplete;
        }
        std::fprintf(stderr,
                     "[GCSLOTDELTA] SLOT_DELTA_SUMMARY class=%s name=%s a=%llu b_pre=%llu b_post=%llu c=%llu "
                     "a_b_pre_changed=%llu/%llu a_b_post_changed=%llu/%llu b_post_c_changed=%llu/%llu "
                     "write_hits=%llu\n",
                     i < kBadWatchN ? "bad" : "good", g_watch[i].name,
                     static_cast<unsigned long long>(g_watch[i].snapshotCount[SNAP_A].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(
                         g_watch[i].snapshotCount[SNAP_B_PRE].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(
                         g_watch[i].snapshotCount[SNAP_B_POST].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_watch[i].snapshotCount[SNAP_C].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_watch[i].aToBPreChanged.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_watch[i].aToBPreCompared.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_watch[i].aToBPostChanged.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_watch[i].aToBPostCompared.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_watch[i].bPostToCChanged.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_watch[i].bPostToCCompared.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_watch[i].writeCount.load(std::memory_order_relaxed)));
    }
    std::fprintf(stderr, "[GCSLOTDELTA] CONTROL_SUMMARY complete=%zu total=%zu snapshot_emitted=%llu cap=%zu "
                         "delta_emitted=%llu delta_cap=%zu\n",
                 controlComplete, wn > kBadWatchN ? wn - kBadWatchN : 0,
                 static_cast<unsigned long long>(g_snapshotEmitted.load(std::memory_order_relaxed)), kSnapshotCap,
                 static_cast<unsigned long long>(g_deltaEmitted.load(std::memory_order_relaxed)), kDeltaCap);
    for (size_t i = 0; i < PATH_COUNT; ++i) {
        uint64_t total = g_pathTotal[i].load(std::memory_order_relaxed);
        uint64_t watched = g_pathWatched[i].load(std::memory_order_relaxed);
        if (total != 0 || watched != 0) {
            std::fprintf(stderr, "[GCSLOTDELTA] WRITE_PATH site=%s total=%llu watched=%llu\n",
                         WritePathName(static_cast<uint8_t>(i)), static_cast<unsigned long long>(total),
                         static_cast<unsigned long long>(watched));
        }
    }
    std::fprintf(stderr,
                 "[GCOLDREG] WRITEGENERIC_PHASE_HISTOGRAM total=%llu bad_ti_events=%llu old_id_emitted=%llu "
                 "faulty_slot_deltas=%llu",
                 static_cast<unsigned long long>(g_wgTotal.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_badTiEvents.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_oldIdEmitted.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_faultyRoundSlotDeltas.load(std::memory_order_relaxed)));
    for (size_t i = 0; i < kWgPhaseN; ++i) {
        uint64_t c = g_wgPhaseTotal[i].load(std::memory_order_relaxed);
        if (c != 0) {
            std::fprintf(stderr, " %s=%llu", GcPhaseNameLocal(static_cast<uint8_t>(i)),
                         static_cast<unsigned long long>(c));
        }
    }
    std::fprintf(stderr, "\n");
    std::fprintf(stderr,
                 "[GCFWDWRITE] FIXWRITE_SUMMARY total=%llu valid=%llu invalid=%llu did_forward=%llu "
                 "emitted=%llu crash_eq_yes=%llu crash_eq_no=%llu crash_eq_checked=%llu ring_n=%llu\n",
                 static_cast<unsigned long long>(g_fixWriteTotal.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_fixWriteValid.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_fixWriteInvalid.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_fixWriteDidForward.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_fixWriteEmitted.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_crashEqualsFixWriteYes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_crashEqualsFixWriteNo.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_crashEqualsFixWriteChecked.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_fixWriteRingN.load(std::memory_order_relaxed)));
}

uint32_t GlobalInitDepth()
{
    return g_initDepth.load(std::memory_order_relaxed);
}

uint64_t GlobalInitCompletedCount()
{
    return g_initCompleted.load(std::memory_order_relaxed);
}

bool AnyGlobalInitActive()
{
    return g_initDepth.load(std::memory_order_relaxed) > 0;
}

} // namespace GcInitWin
} // namespace MapleRuntime
