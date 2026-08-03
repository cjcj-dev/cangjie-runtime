// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "WCollector.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include <unistd.h>
#include <sys/mman.h>

#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/Flags.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "GcInitWinProbe.h"

namespace MapleRuntime {
namespace {
// GCDISPEL instrumentation (diag only). Gated by MRT_GCDISPEL=1.
// Path IDs for HasRefField fail/classify (Q2). Counters prove fire.
enum GcdispelHasPath : size_t {
    HR_CALL = 0,
    HR_TI_NULL,
    HR_TI_NONCANON,
    HR_TI_UNMAPPED,
    HR_TYPE_READ,
    HR_ARRAY_BRANCH,
    HR_COMP_NULL,
    HR_COMP_UNMAPPED,
    HR_COMP_IS_REF_TRUE,
    HR_COMP_RECURSE,
    HR_NONARRAY_FLAG_TRUE,
    HR_NONARRAY_FLAG_FALSE,
    HR_RETURN_TRUE,
    HR_RETURN_FALSE,
    HR_PATH_COUNT
};

std::atomic<uint64_t> g_hrPath[HR_PATH_COUNT];
std::atomic<uint64_t> g_hrSampleEmitted{0};
std::atomic<uint64_t> g_holderSnapEmitted{0};
std::atomic<uint64_t> g_holderDeltaEmitted{0};
std::atomic<uint64_t> g_positiveControl{0};
std::atomic<bool> g_summaryDumped{false};
void DumpHrSummary(const char* reason);
void DumpEnqSummary(const char* reason);

constexpr size_t kSnapCap = 64;
struct HolderSnap {
    BaseObject* holder;
    RegionInfo* region;
    TypeInfo* typeInfo;
    uintptr_t typeAddr;
    uint32_t live;
    uint32_t liveRaw;
    uint8_t regionType;
    uint8_t young;
    uint8_t authority;
    uint8_t knownEmpty;
    uint8_t validObj;
    uint8_t stateCode;
    uint8_t noncanon;
    uint8_t tiMapped;
    I8 tiType;
    U8 tiFlag;
    TypeInfo* component;
    uint8_t filled;
    const char* point;
};
HolderSnap g_beforeSnaps[kSnapCap];
std::atomic<size_t> g_beforeSnapN{0};

bool GcdispelOn()
{
    static int cached = -1;
    if (cached < 0) {
        const char* e = std::getenv("MRT_GCDISPEL");
        cached = (e != nullptr && std::strcmp(e, "1") == 0) ? 1 : 0;
        if (cached == 1) {
            std::atexit([]() {
                DumpHrSummary("atexit");
                DumpEnqSummary("atexit");
            });
        }
    }
    return cached == 1;
}

GcInitWin::MinorTargetFate CaptureMinorTargetFate(const void* value)
{
    GcInitWin::MinorTargetFate fate{};
    fate.target = value;
    fate.regionType = 0xff;
    fate.marked = 2;
    fate.state = 0xff;
    BaseObject* target = reinterpret_cast<BaseObject*>(const_cast<void*>(value));
    fate.heap = Heap::IsHeapAddress(target) ? 1 : 0;
    if (fate.heap == 0) {
        return fate;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    fate.region = region;
    if (region == nullptr) {
        return fate;
    }
    fate.regionStart = region->GetRegionStart();
    fate.regionAlloc = region->GetRegionAllocPtr();
    fate.regionType = static_cast<uint8_t>(region->GetRegionType());
    fate.validRegion = region->IsValidRegion() ? 1 : 0;
    fate.young = region->IsYoungRegion() ? 1 : 0;
    fate.freeRegion = region->IsFreeRegion() ? 1 : 0;
    fate.garbageRegion = region->IsGarbageRegion() ? 1 : 0;
    uintptr_t address = reinterpret_cast<uintptr_t>(target);
    fate.inAllocRange = address >= fate.regionStart && address < fate.regionAlloc ? 1 : 0;
    if (fate.validRegion != 0 && fate.freeRegion == 0 && fate.garbageRegion == 0 && fate.inAllocRange != 0) {
        fate.marked = region->IsMarkedObject(target) ? 1 : 0;
        fate.state = static_cast<uint8_t>(target->GetStateWord().GetStateCode());
    }
    return fate;
}

bool PageMapped(const void* p)
{
    if (p == nullptr) {
        return false;
    }
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        pageSize = 4096;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    uintptr_t page = addr & ~static_cast<uintptr_t>(pageSize - 1);
    unsigned char vec = 0;
    return mincore(reinterpret_cast<void*>(page), 1, &vec) == 0;
}

void DumpHrSummary(const char* reason)
{
    bool expected = false;
    if (!g_summaryDumped.compare_exchange_strong(expected, true)) {
        return;
    }
    static const char* names[HR_PATH_COUNT] = {
        "call", "ti_null", "ti_noncanon", "ti_unmapped", "type_read", "array_branch",
        "comp_null", "comp_unmapped", "comp_is_ref_true", "comp_recurse",
        "nonarray_flag_true", "nonarray_flag_false", "return_true", "return_false"
    };
    std::fprintf(stderr, "[GCDISPEL] HASREFFIELD_SUMMARY reason=%s", reason);
    for (size_t i = 0; i < HR_PATH_COUNT; ++i) {
        std::fprintf(stderr, " %s=%llu", names[i],
                     static_cast<unsigned long long>(g_hrPath[i].load(std::memory_order_relaxed)));
    }
    std::fprintf(stderr,
                 " snap_emitted=%llu delta_emitted=%llu pos=%llu\n",
                 static_cast<unsigned long long>(g_holderSnapEmitted.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_holderDeltaEmitted.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_positiveControl.load(std::memory_order_relaxed)));
}

void FillSnap(HolderSnap& s, BaseObject* holder, const char* point)
{
    s.holder = holder;
    s.point = point;
    s.filled = 1;
    s.region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    s.typeInfo = holder != nullptr ? holder->GetTypeInfo() : nullptr;
    s.typeAddr = reinterpret_cast<uintptr_t>(s.typeInfo);
    s.noncanon = s.typeAddr >= (1ULL << StateWord::ADDRESS_BIT_COUNT) ? 1 : 0;
    s.validObj = (holder != nullptr && holder->IsValidObject()) ? 1 : 0;
    s.stateCode = holder != nullptr ? static_cast<uint8_t>(holder->GetStateWord().GetStateCode()) : 0xff;
    s.tiMapped = PageMapped(s.typeInfo) ? 1 : 0;
    s.tiType = 0;
    s.tiFlag = 0;
    s.component = nullptr;
    if (s.region != nullptr) {
        s.regionType = static_cast<uint8_t>(s.region->GetRegionType());
        s.young = s.region->IsYoungRegion() ? 1 : 0;
        s.live = s.region->GetLiveByteCount();
        s.authority = s.region->IsLiveCountAuthoritative() ? 1 : 0;
        s.knownEmpty = s.region->IsKnownEmpty() ? 1 : 0;
        s.liveRaw = s.live | (s.authority ? (1u << 31) : 0);
    } else {
        s.regionType = 0xff;
        s.young = 0;
        s.live = 0;
        s.liveRaw = 0;
        s.authority = 0;
        s.knownEmpty = 0;
    }
    if (s.typeInfo != nullptr && s.tiMapped && !s.noncanon) {
        s.tiType = s.typeInfo->GetType();
        s.tiFlag = static_cast<U8>(s.typeInfo->GetFlags());
        if (s.typeInfo->IsArrayType()) {
            s.component = s.typeInfo->GetComponentTypeInfo();
        }
    }
}

void EmitSnapLine(const HolderSnap& s, const char* tag)
{
    std::fprintf(stderr,
                 "[GCDISPEL] %s point=%s holder=%p region=%p region_type=%u young=%u live=%u authority=%u "
                 "known_empty=%u valid=%u state=%u type_info=%p type_noncanon=%u ti_mapped=%u ti_type=%d "
                 "ti_flag=0x%x component=%p\n",
                 tag, s.point != nullptr ? s.point : "?", s.holder, s.region,
                 static_cast<unsigned>(s.regionType), static_cast<unsigned>(s.young), s.live,
                 static_cast<unsigned>(s.authority), static_cast<unsigned>(s.knownEmpty),
                 static_cast<unsigned>(s.validObj), static_cast<unsigned>(s.stateCode), s.typeInfo,
                 static_cast<unsigned>(s.noncanon), static_cast<unsigned>(s.tiMapped),
                 static_cast<int>(s.tiType), static_cast<unsigned>(s.tiFlag), s.component);
}

void RecordBeforeSnap(BaseObject* holder)
{
    size_t n = g_beforeSnapN.load(std::memory_order_relaxed);
    if (n >= kSnapCap) {
        return;
    }
    size_t slot = g_beforeSnapN.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kSnapCap) {
        return;
    }
    FillSnap(g_beforeSnaps[slot], holder, "before-return");
    if (g_holderSnapEmitted.fetch_add(1, std::memory_order_relaxed) < 16) {
        EmitSnapLine(g_beforeSnaps[slot], "HOLDER_SNAP");
    }
}

void EmitDeltaIfAny(BaseObject* holder, const HolderSnap& after)
{
    size_t n = g_beforeSnapN.load(std::memory_order_acquire);
    if (n > kSnapCap) {
        n = kSnapCap;
    }
    for (size_t i = 0; i < n; ++i) {
        if (g_beforeSnaps[i].holder != holder) {
            continue;
        }
        const HolderSnap& b = g_beforeSnaps[i];
        bool any = false;
        auto check = [&](const char* field, unsigned long long bv, unsigned long long av) {
            if (bv != av) {
                any = true;
                std::fprintf(stderr, "[GCDISPEL] HOLDER_DELTA holder=%p field=%s before=%llu after=%llu\n", holder,
                             field, bv, av);
            }
        };
        check("region", reinterpret_cast<uintptr_t>(b.region), reinterpret_cast<uintptr_t>(after.region));
        check("region_type", b.regionType, after.regionType);
        check("young", b.young, after.young);
        check("live", b.live, after.live);
        check("authority", b.authority, after.authority);
        check("known_empty", b.knownEmpty, after.knownEmpty);
        check("valid", b.validObj, after.validObj);
        check("state", b.stateCode, after.stateCode);
        check("type_info", b.typeAddr, after.typeAddr);
        check("type_noncanon", b.noncanon, after.noncanon);
        check("ti_mapped", b.tiMapped, after.tiMapped);
        check("ti_type", static_cast<unsigned long long>(static_cast<int>(b.tiType)),
              static_cast<unsigned long long>(static_cast<int>(after.tiType)));
        check("ti_flag", b.tiFlag, after.tiFlag);
        check("component", reinterpret_cast<uintptr_t>(b.component),
              reinterpret_cast<uintptr_t>(after.component));
        if (!any) {
            if (g_holderDeltaEmitted.fetch_add(1, std::memory_order_relaxed) < 8) {
                std::fprintf(stderr,
                             "[GCDISPEL] HOLDER_DELTA holder=%p none_dispel_is_not_the_mutator\n", holder);
            }
        } else {
            g_holderDeltaEmitted.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
}

// gcenqueue Q1/Q2: who enqueues into after-dispel closure, and TI state at enqueue.
enum EnqTiClass : uint8_t {
    ENQ_TI_GOOD = 0,
    ENQ_TI_NULL = 1,
    ENQ_TI_GARBAGE_LOW = 2,
    ENQ_TI_NONCANON = 3,
    ENQ_TI_UNMAPPED = 4,
    ENQ_TI_CLASS_COUNT = 5
};
constexpr size_t kEnqCatCount = 12;
constexpr size_t kEnqRingCap = 8192;
constexpr size_t kEnqSampleCap = 48;
constexpr size_t kEnqHitCap = 64;
constexpr size_t kStaticLocCap = 64;
struct EnqRec {
    BaseObject* target;
    BaseObject* slotHolder;
    const void* slot;
    uintptr_t tiAtEnq;
    uintptr_t holderTiAtEnq;
    uint8_t category;
    uint8_t tiClass;
    uint8_t holderTiClass;
    uint8_t pointId; // 1=after-dispel 2=round2-start 3=other
    uint32_t seq;
};
EnqRec g_enqRing[kEnqRingCap];
std::atomic<uint32_t> g_enqSeq{0};
std::atomic<size_t> g_enqRingN{0};
std::atomic<uint64_t> g_enqSiteCount[kEnqCatCount];
std::atomic<uint64_t> g_enqTiClassCount[ENQ_TI_CLASS_COUNT];
std::atomic<uint64_t> g_enqSampleEmitted{0};
std::atomic<uint64_t> g_enqHitEmitted{0};
std::atomic<uint64_t> g_enqHitGoodThenBad{0};
std::atomic<uint64_t> g_enqHitBadAlready{0};
std::atomic<uint64_t> g_enqHitMiss{0};
std::atomic<bool> g_enqSummaryDumped{false};
// gcstatic: locate static-root slot addresses in maps / ELF section.
std::atomic<uint64_t> g_staticLocEmitted{0};
std::atomic<uint64_t> g_staticGoodSample{0};
std::atomic<uint64_t> g_staticBadSample{0};
std::atomic<uint64_t> g_staticSectionCount[8]; // 0=unknown 1=.data 2=.bss 3=.rodata 4=.cjmetadata 5=anon 6=heap_like 7=other

struct MapsRange {
    uintptr_t start;
    uintptr_t end;
    uintptr_t offset;
    char perms[8];
    char path[256];
};

bool LookupMaps(uintptr_t addr, MapsRange& out)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (f == nullptr) {
        return false;
    }
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f) != nullptr) {
        uintptr_t start = 0;
        uintptr_t end = 0;
        uintptr_t offset = 0;
        char perms[8] = {};
        char path[256] = {};
        // 55aa...-55aa... r-xp 00000000 08:01 123 /path
        int n = std::sscanf(line, "%lx-%lx %7s %lx %*s %*s %255[^\n]", &start, &end, perms, &offset, path);
        if (n < 4) {
            continue;
        }
        if (addr >= start && addr < end) {
            out.start = start;
            out.end = end;
            out.offset = offset + (addr - start);
            std::snprintf(out.perms, sizeof(out.perms), "%s", perms);
            if (n >= 5) {
                // trim leading spaces in path
                const char* p = path;
                while (*p == ' ' || *p == '\t') {
                    ++p;
                }
                std::snprintf(out.path, sizeof(out.path), "%s", p);
            } else {
                out.path[0] = '\0';
            }
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

// Best-effort ELF section name for file-backed mapping offset.
const char* ElfSectionAt(const char* path, uintptr_t fileOff)
{
    if (path == nullptr || path[0] == '\0' || path[0] == '[') {
        return "anon_or_special";
    }
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return "unreadable";
    }
    unsigned char eident[16];
    if (fread(eident, 1, 16, f) != 16 || eident[0] != 0x7f || eident[1] != 'E' || eident[2] != 'L' ||
        eident[3] != 'F') {
        fclose(f);
        return "not_elf";
    }
    bool is64 = eident[4] == 2;
    uint16_t shnum = 0;
    uint16_t shstrndx = 0;
    uint64_t shoff = 0;
    uint16_t shentsize = 0;
    if (is64) {
        if (fseek(f, 40, SEEK_SET) != 0) {
            fclose(f);
            return "elf_hdr_fail";
        }
        if (fread(&shoff, 8, 1, f) != 1) {
            fclose(f);
            return "elf_hdr_fail";
        }
        if (fseek(f, 58, SEEK_SET) != 0) {
            fclose(f);
            return "elf_hdr_fail";
        }
        if (fread(&shentsize, 2, 1, f) != 1 || fread(&shnum, 2, 1, f) != 1 || fread(&shstrndx, 2, 1, f) != 1) {
            fclose(f);
            return "elf_hdr_fail";
        }
    } else {
        fclose(f);
        return "elf32_skip";
    }
    if (shnum == 0 || shentsize < 64 || shstrndx >= shnum) {
        fclose(f);
        return "elf_no_shdr";
    }
    // section header: name(4) type(4) flags(8) addr(8) offset(8) size(8) ...
    auto readShdr = [&](uint16_t idx, uint32_t& nameOff, uint64_t& off, uint64_t& size) -> bool {
        if (fseek(f, static_cast<long>(shoff + static_cast<uint64_t>(idx) * shentsize), SEEK_SET) != 0) {
            return false;
        }
        unsigned char buf[64];
        if (fread(buf, 1, 64, f) != 64) {
            return false;
        }
        std::memcpy(&nameOff, buf + 0, 4);
        std::memcpy(&off, buf + 24, 8);
        std::memcpy(&size, buf + 32, 8);
        return true;
    };
    uint32_t strName = 0;
    uint64_t strOff = 0;
    uint64_t strSize = 0;
    if (!readShdr(shstrndx, strName, strOff, strSize) || strSize == 0 || strSize > (1u << 20)) {
        fclose(f);
        return "elf_strtab_fail";
    }
    std::vector<char> strtab(static_cast<size_t>(strSize));
    if (fseek(f, static_cast<long>(strOff), SEEK_SET) != 0 ||
        fread(strtab.data(), 1, static_cast<size_t>(strSize), f) != strSize) {
        fclose(f);
        return "elf_strtab_read_fail";
    }
    static thread_local char secNameBuf[64];
    secNameBuf[0] = '?';
    secNameBuf[1] = '\0';
    for (uint16_t i = 0; i < shnum; ++i) {
        uint32_t nameOff = 0;
        uint64_t off = 0;
        uint64_t size = 0;
        if (!readShdr(i, nameOff, off, size)) {
            continue;
        }
        if (size == 0) {
            continue;
        }
        if (fileOff >= off && fileOff < off + size) {
            if (nameOff < strSize) {
                std::snprintf(secNameBuf, sizeof(secNameBuf), "%s", strtab.data() + nameOff);
            }
            fclose(f);
            return secNameBuf;
        }
    }
    fclose(f);
    return "no_section_match";
}

uint8_t SectionBucket(const char* sec)
{
    if (sec == nullptr) {
        return 0;
    }
    if (std::strcmp(sec, ".data") == 0 || std::strncmp(sec, ".data.", 6) == 0) {
        return 1;
    }
    if (std::strcmp(sec, ".bss") == 0 || std::strncmp(sec, ".bss.", 5) == 0) {
        return 2;
    }
    if (std::strcmp(sec, ".rodata") == 0 || std::strncmp(sec, ".rodata", 7) == 0) {
        return 3;
    }
    if (std::strstr(sec, "cjmeta") != nullptr || std::strstr(sec, "CJ") != nullptr ||
        std::strstr(sec, "gcroot") != nullptr) {
        return 4;
    }
    if (std::strcmp(sec, "anon_or_special") == 0) {
        return 5;
    }
    return 7;
}

const char* EnqTiNameFwd(uint8_t c); // defined below with other Enq helpers

void EmitStaticSlotLoc(const void* slot, uint8_t tiClass, uint32_t seq, const char* kind)
{
    if (slot == nullptr) {
        return;
    }
    uint64_t n = g_staticLocEmitted.fetch_add(1, std::memory_order_relaxed);
    if (n >= kStaticLocCap) {
        return;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
    MapsRange mr{};
    bool ok = LookupMaps(addr, mr);
    const char* sec = "maps_miss";
    const char* mapTag = "unknown";
    if (ok) {
        if (mr.path[0] == '\0') {
            mapTag = "anon";
            sec = "anon";
        } else if (mr.path[0] == '[') {
            mapTag = mr.path;
            sec = mr.path;
        } else {
            mapTag = mr.path;
            sec = ElfSectionAt(mr.path, mr.offset);
        }
    }
    uint8_t bucket = SectionBucket(sec);
    g_staticSectionCount[bucket].fetch_add(1, std::memory_order_relaxed);
    // Precise root table path: VisitStaticRoots only walks registered GC_ROOT_TABLE entries.
    // in_root_table=yes by construction for category=static.
    std::fprintf(stderr,
                 "[GCSTATIC] SLOT_LOC seq=%u kind=%s ticlass=%s slot=%p maps=%s perms=%s "
                 "map_start=0x%llx map_end=0x%llx file_off=0x%llx section=%s "
                 "in_root_table=yes_by_VisitStaticRoots\n",
                 seq, kind, EnqTiNameFwd(tiClass), slot, ok ? mapTag : "(none)", ok ? mr.perms : "?",
                 static_cast<unsigned long long>(ok ? mr.start : 0),
                 static_cast<unsigned long long>(ok ? mr.end : 0),
                 static_cast<unsigned long long>(ok ? mr.offset : 0), sec);
}

void DumpStaticLocSummary(const char* reason)
{
    static const char* names[8] = {
        "unknown", ".data", ".bss", ".rodata", ".cjmetadata", "anon", "heap_like", "other"
    };
    std::fprintf(stderr, "[GCSTATIC] SLOT_LOC_SUMMARY reason=%s emitted=%llu", reason,
                 static_cast<unsigned long long>(g_staticLocEmitted.load(std::memory_order_relaxed)));
    for (size_t i = 0; i < 8; ++i) {
        uint64_t c = g_staticSectionCount[i].load(std::memory_order_relaxed);
        if (c != 0) {
            std::fprintf(stderr, " %s=%llu", names[i], static_cast<unsigned long long>(c));
        }
    }
    std::fprintf(stderr, " good_samples=%llu bad_samples=%llu\n",
                 static_cast<unsigned long long>(g_staticGoodSample.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_staticBadSample.load(std::memory_order_relaxed)));
}

uint8_t PointIdOf(const char* point)
{
    if (point == nullptr) {
        return 3;
    }
    if (std::strcmp(point, "after-dispel") == 0) {
        return 1;
    }
    if (std::strcmp(point, "round2-start") == 0) {
        return 2;
    }
    return 3;
}

uint8_t ClassifyTiAddr(uintptr_t typeAddr, TypeInfo* ti)
{
    if (ti == nullptr || typeAddr == 0) {
        return ENQ_TI_NULL;
    }
    if (typeAddr < 0x1000ULL) {
        return ENQ_TI_GARBAGE_LOW;
    }
    if (typeAddr >= (1ULL << StateWord::ADDRESS_BIT_COUNT)) {
        return ENQ_TI_NONCANON;
    }
    if (!PageMapped(ti)) {
        return ENQ_TI_UNMAPPED;
    }
    return ENQ_TI_GOOD;
}

const char* EnqCatName(size_t cat)
{
    static const char* names[kEnqCatCount] = {
        "stack", "register", "derived", "static", "heap", "weak", "finalizer", "export",
        "concurrency", "external_resurrection", "exception", "raw_object"
    };
    return cat < kEnqCatCount ? names[cat] : "?";
}

const char* EnqTiName(uint8_t c)
{
    switch (c) {
        case ENQ_TI_GOOD: return "good";
        case ENQ_TI_NULL: return "null";
        case ENQ_TI_GARBAGE_LOW: return "garbage_low";
        case ENQ_TI_NONCANON: return "noncanon";
        case ENQ_TI_UNMAPPED: return "unmapped";
        default: return "?";
    }
}
const char* EnqTiNameFwd(uint8_t c) { return EnqTiName(c); }

void DumpEnqSummary(const char* reason)
{
    // Always print full site/ti/hit counters (bounded line; needed after each validator point).
    (void)g_enqSummaryDumped.exchange(true, std::memory_order_relaxed);
    GcInitWin::DumpSummary(reason);
    std::fprintf(stderr, "[GCENQUEUE] ENQUEUE_SUMMARY reason=%s total=%u", reason,
                 g_enqSeq.load(std::memory_order_relaxed));
    for (size_t i = 0; i < kEnqCatCount; ++i) {
        uint64_t c = g_enqSiteCount[i].load(std::memory_order_relaxed);
        if (c != 0) {
            std::fprintf(stderr, " %s=%llu", EnqCatName(i), static_cast<unsigned long long>(c));
        }
    }
    std::fprintf(stderr, " ti_good=%llu ti_null=%llu ti_garbage_low=%llu ti_noncanon=%llu ti_unmapped=%llu",
                 static_cast<unsigned long long>(g_enqTiClassCount[ENQ_TI_GOOD].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_enqTiClassCount[ENQ_TI_NULL].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_enqTiClassCount[ENQ_TI_GARBAGE_LOW].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_enqTiClassCount[ENQ_TI_NONCANON].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_enqTiClassCount[ENQ_TI_UNMAPPED].load(std::memory_order_relaxed)));
    std::fprintf(stderr,
                 " hit_good_then_bad=%llu hit_bad_already=%llu hit_miss=%llu hit_emitted=%llu\n",
                 static_cast<unsigned long long>(g_enqHitGoodThenBad.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_enqHitBadAlready.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_enqHitMiss.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_enqHitEmitted.load(std::memory_order_relaxed)));
    DumpStaticLocSummary(reason);
}

void RecordEnqueue(BaseObject* target, BaseObject* slotHolder, const void* slot, size_t category,
                   const char* point)
{
    if (!GcdispelOn() || target == nullptr || category >= kEnqCatCount) {
        return;
    }
    // Only track closure construction for after-dispel / round2-start (buildReachableClosure path).
    uint8_t pid = PointIdOf(point);
    if (pid == 3) {
        return;
    }
    TypeInfo* ti = target->GetTypeInfo();
    uintptr_t typeAddr = reinterpret_cast<uintptr_t>(ti);
    uint8_t tc = ClassifyTiAddr(typeAddr, ti);
    TypeInfo* hti = slotHolder != nullptr ? slotHolder->GetTypeInfo() : nullptr;
    uintptr_t hTypeAddr = reinterpret_cast<uintptr_t>(hti);
    uint8_t htc = slotHolder != nullptr ? ClassifyTiAddr(hTypeAddr, hti) : ENQ_TI_NULL;

    g_enqSiteCount[category].fetch_add(1, std::memory_order_relaxed);
    g_enqTiClassCount[tc].fetch_add(1, std::memory_order_relaxed);

    uint32_t seq = g_enqSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t idx = g_enqRingN.fetch_add(1, std::memory_order_relaxed);
    EnqRec& rec = g_enqRing[idx % kEnqRingCap];
    rec.target = target;
    rec.slotHolder = slotHolder;
    rec.slot = slot;
    rec.tiAtEnq = typeAddr;
    rec.holderTiAtEnq = hTypeAddr;
    rec.category = static_cast<uint8_t>(category);
    rec.tiClass = tc;
    rec.holderTiClass = htc;
    rec.pointId = pid;
    rec.seq = seq;

    // Sample: first N overall + all bad-TI enqueues up to cap (prefer bad).
    static std::atomic<uint64_t> g_enqBadSample{0};
    bool interesting = tc != ENQ_TI_GOOD;
    uint64_t sampleN = g_enqSampleEmitted.load(std::memory_order_relaxed);
    bool take = false;
    if (interesting) {
        take = g_enqBadSample.fetch_add(1, std::memory_order_relaxed) < kEnqSampleCap;
    } else {
        take = sampleN < 8;
    }
    if (take && g_enqSampleEmitted.fetch_add(1, std::memory_order_relaxed) < (kEnqSampleCap + 8)) {
        ptrdiff_t off = 0;
        if (slotHolder != nullptr && slot != nullptr) {
            off = reinterpret_cast<const char*>(slot) - reinterpret_cast<const char*>(slotHolder);
        }
        std::fprintf(stderr,
                     "[GCENQUEUE] ENQUEUE_SAMPLE seq=%u point=%s cat=%s target=%p ti=0x%llx ticlass=%s "
                     "slot=%p holder=%p holder_ti=0x%llx holder_ticlass=%s slot_off=%td\n",
                     seq, point, EnqCatName(category), target,
                     static_cast<unsigned long long>(typeAddr), EnqTiName(tc), slot, slotHolder,
                     static_cast<unsigned long long>(hTypeAddr), EnqTiName(htc), off);
    }
    // gcstatic: locate static-root slots (bad + limited good) in maps/section.
    if (category == 3 && slot != nullptr) {
        bool takeLoc = false;
        if (tc != ENQ_TI_GOOD) {
            takeLoc = g_staticBadSample.fetch_add(1, std::memory_order_relaxed) < 48;
        } else {
            takeLoc = g_staticGoodSample.fetch_add(1, std::memory_order_relaxed) < 16;
        }
        if (takeLoc) {
            EmitStaticSlotLoc(slot, tc, seq, tc != ENQ_TI_GOOD ? "bad" : "good");
        }
        // gcinitwin: lifecycle phase of static slot at enqueue.
        uint64_t round = GcInitWin::CurrentMinorRound();
        const void* initialTarget = GcInitWin::InitialMinorTarget(slot, round);
        GcInitWin::NoteStaticEnqueueLifecycle(slot, target, tc, point,
                                              tc != ENQ_TI_GOOD ? "bad" : "good",
                                              CaptureMinorTargetFate(target), CaptureMinorTargetFate(initialTarget));
    }
}

void EmitEnqueueHit(BaseObject* holder, const char* point, uintptr_t failTi)
{
    if (!GcdispelOn() || holder == nullptr) {
        return;
    }
    // Reverse lookup newest matching target in ring.
    size_t n = g_enqRingN.load(std::memory_order_acquire);
    size_t scan = n < kEnqRingCap ? n : kEnqRingCap;
    const EnqRec* hit = nullptr;
    for (size_t i = 0; i < scan; ++i) {
        size_t idx = (n - 1 - i) % kEnqRingCap;
        if (g_enqRing[idx].target == holder) {
            hit = &g_enqRing[idx];
            break;
        }
    }
    uint8_t failClass = ClassifyTiAddr(failTi, reinterpret_cast<TypeInfo*>(failTi));
    if (hit == nullptr) {
        g_enqHitMiss.fetch_add(1, std::memory_order_relaxed);
        if (g_enqHitEmitted.fetch_add(1, std::memory_order_relaxed) < kEnqHitCap) {
            std::fprintf(stderr,
                         "[GCENQUEUE] ENQUEUE_HIT miss=1 point=%s holder=%p fail_ti=0x%llx fail_class=%s "
                         "ring_n=%zu\n",
                         point, holder, static_cast<unsigned long long>(failTi), EnqTiName(failClass), n);
        }
        return;
    }
    bool badAtEnq = hit->tiClass != ENQ_TI_GOOD;
    if (badAtEnq) {
        g_enqHitBadAlready.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_enqHitGoodThenBad.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_enqHitEmitted.fetch_add(1, std::memory_order_relaxed) < kEnqHitCap) {
        ptrdiff_t off = 0;
        if (hit->slotHolder != nullptr && hit->slot != nullptr) {
            off = reinterpret_cast<const char*>(hit->slot) - reinterpret_cast<const char*>(hit->slotHolder);
        }
        std::fprintf(stderr,
                     "[GCENQUEUE] ENQUEUE_HIT miss=0 point=%s holder=%p fail_ti=0x%llx fail_class=%s "
                     "enq_seq=%u enq_cat=%s enq_ti=0x%llx enq_ticlass=%s "
                     "slot=%p slot_holder=%p slot_holder_ti=0x%llx slot_holder_ticlass=%s slot_off=%td "
                     "state=%s\n",
                     point, holder, static_cast<unsigned long long>(failTi), EnqTiName(failClass), hit->seq,
                     EnqCatName(hit->category), static_cast<unsigned long long>(hit->tiAtEnq),
                     EnqTiName(hit->tiClass), hit->slot, hit->slotHolder,
                     static_cast<unsigned long long>(hit->holderTiAtEnq), EnqTiName(hit->holderTiClass), off,
                     badAtEnq ? "bad_already_at_enqueue" : "good_then_corrupted");
    }
}

// gcgarbti Q2: is the holder address still an object, or already reused as free-list / metadata?
std::atomic<uint64_t> g_objectIdEmitted{0};
void EmitIsItEvenAnObject(BaseObject* holder, const char* point, uintptr_t typeAddr)
{
    if (g_objectIdEmitted.fetch_add(1, std::memory_order_relaxed) >= 24) {
        return;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(holder);
    bool aligned8 = (addr & 0x7ULL) == 0;
    bool pageMapped = PageMapped(holder);
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    uint8_t regionType = 0xff;
    uint8_t unitRole = 0xff;
    uint8_t young = 0;
    uint8_t freeR = 0;
    uint8_t garbageR = 0;
    uint8_t validR = 0;
    uint32_t live = 0;
    uint8_t authority = 0;
    uint8_t knownEmpty = 0;
    uintptr_t regionStart = 0;
    uintptr_t regionEnd = 0;
    uintptr_t allocPtr = 0;
    uint8_t inAllocRange = 0;
    if (region != nullptr) {
        regionType = static_cast<uint8_t>(region->GetRegionType());
        unitRole = static_cast<uint8_t>(region->GetUnitRole());
        young = region->IsYoungRegion() ? 1 : 0;
        freeR = region->IsFreeRegion() ? 1 : 0;
        garbageR = region->IsGarbageRegion() ? 1 : 0;
        validR = region->IsValidRegion() ? 1 : 0;
        live = region->GetLiveByteCount();
        authority = region->IsLiveCountAuthoritative() ? 1 : 0;
        knownEmpty = region->IsKnownEmpty() ? 1 : 0;
        regionStart = region->GetRegionStart();
        regionEnd = region->GetRegionEnd();
        allocPtr = region->GetRegionAllocPtr();
        inAllocRange = (addr >= regionStart && addr < allocPtr) ? 1 : 0;
    }
    uint64_t raw0 = 0;
    uint64_t raw1 = 0;
    uint64_t raw2 = 0;
    uint64_t raw3 = 0;
    if (pageMapped) {
        const uint64_t* p = reinterpret_cast<const uint64_t*>(holder);
        raw0 = p[0];
        raw1 = p[1];
        raw2 = p[2];
        raw3 = p[3];
    }
    // ObjectSlot = { StateWord; ObjectSlot* next } — next at +8 if free-list node.
    uintptr_t nextCand = static_cast<uintptr_t>(raw1);
    bool nextLooksHeap = nextCand != 0 && nextCand >= 0x1000ULL &&
                         nextCand < (1ULL << StateWord::ADDRESS_BIT_COUNT) && (nextCand & 0x7ULL) == 0;
    bool nextMapped = nextLooksHeap && PageMapped(reinterpret_cast<const void*>(nextCand));
    bool allZeroHead = (raw0 == 0 && raw1 == 0);
    bool allOnesLow = (typeAddr == ~0ULL) || (typeAddr == 0xffffffffffffULL);
    bool smallIntTi = typeAddr > 0 && typeAddr < 0x1000ULL;
    // Classification (bounded; not product logic).
    const char* verdict = "undetermined";
    if (!pageMapped || region == nullptr) {
        verdict = "no_unmapped_or_no_region";
    } else if (freeR || garbageR || !validR) {
        verdict = "no_region_free_or_garbage";
    } else if (!inAllocRange) {
        verdict = "no_outside_alloc_range";
    } else if (allZeroHead) {
        verdict = "no_zero_fill_head";
    } else if (smallIntTi || allOnesLow) {
        // still in alloc range of a linked region, but head is not a TypeInfo address shape
        if (nextLooksHeap && nextMapped) {
            verdict = "no_reused_slotlist_like";
        } else if (!aligned8) {
            verdict = "no_misaligned_interior";
        } else {
            verdict = "no_garbage_ti_in_alloc_range";
        }
    } else if (aligned8 && inAllocRange && validR && pageMapped) {
        verdict = "yes_header_looks_object_slot";
    }
    std::fprintf(stderr,
                 "[GCGARBTI] IS_IT_EVEN_AN_OBJECT point=%s holder=%p ti=0x%llx aligned8=%u page=%u "
                 "region=%p rtype=%u urole=%u young=%u free=%u garbage=%u valid=%u live=%u auth=%u "
                 "known_empty=%u start=0x%llx end=0x%llx alloc=0x%llx in_alloc=%u "
                 "raw0=0x%llx raw1=0x%llx raw2=0x%llx raw3=0x%llx next_heap=%u next_map=%u "
                 "verdict=%s\n",
                 point, holder, static_cast<unsigned long long>(typeAddr), aligned8 ? 1u : 0u,
                 pageMapped ? 1u : 0u, region, static_cast<unsigned>(regionType),
                 static_cast<unsigned>(unitRole), static_cast<unsigned>(young),
                 static_cast<unsigned>(freeR), static_cast<unsigned>(garbageR),
                 static_cast<unsigned>(validR), live, static_cast<unsigned>(authority),
                 static_cast<unsigned>(knownEmpty), static_cast<unsigned long long>(regionStart),
                 static_cast<unsigned long long>(regionEnd), static_cast<unsigned long long>(allocPtr),
                 static_cast<unsigned>(inAllocRange), static_cast<unsigned long long>(raw0),
                 static_cast<unsigned long long>(raw1), static_cast<unsigned long long>(raw2),
                 static_cast<unsigned long long>(raw3), nextLooksHeap ? 1u : 0u, nextMapped ? 1u : 0u,
                 verdict);
}

// Classify HasRefField by mirroring TypeInfo::HasRefField. On fail paths never call the real
// inlined HasRefField (would hard-fault); counters + samples are the evidence.
bool DiagnoseHasRefField(BaseObject* holder, const char* point, bool /*doRealCall*/)
{
    g_hrPath[HR_CALL].fetch_add(1, std::memory_order_relaxed);
    if (g_positiveControl.load(std::memory_order_relaxed) == 0) {
        g_positiveControl.store(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[GCDISPEL] PROBE_POSITIVE_CONTROL yes=1 point=%s holder=%p\n", point, holder);
    }

    TypeInfo* ti = holder->GetTypeInfo();
    if (ti == nullptr) {
        g_hrPath[HR_TI_NULL].fetch_add(1, std::memory_order_relaxed);
        if (g_hrSampleEmitted.fetch_add(1, std::memory_order_relaxed) < 32) {
            std::fprintf(stderr, "[GCDISPEL] HASREFFIELD_PATH path=ti_null point=%s holder=%p\n", point, holder);
        }
        return false;
    }
    uintptr_t typeAddr = reinterpret_cast<uintptr_t>(ti);
    // Low garbage pointers (0x1 etc) pass IsValidObject (non-null) but are not real TypeInfo.
    if (typeAddr < 0x1000ULL || typeAddr >= (1ULL << StateWord::ADDRESS_BIT_COUNT)) {
        if (typeAddr < 0x1000ULL) {
            g_hrPath[HR_TI_UNMAPPED].fetch_add(1, std::memory_order_relaxed);
            if (g_hrSampleEmitted.fetch_add(1, std::memory_order_relaxed) < 64) {
                std::fprintf(stderr,
                             "[GCDISPEL] HASREFFIELD_PATH path=ti_garbage_low point=%s holder=%p ti=%p\n",
                             point, holder, ti);
            }
            EmitIsItEvenAnObject(holder, point, typeAddr);
            EmitEnqueueHit(holder, point, typeAddr);
        } else {
            g_hrPath[HR_TI_NONCANON].fetch_add(1, std::memory_order_relaxed);
            if (g_hrSampleEmitted.fetch_add(1, std::memory_order_relaxed) < 64) {
                std::fprintf(stderr, "[GCDISPEL] HASREFFIELD_PATH path=ti_noncanon point=%s holder=%p ti=%p\n",
                             point, holder, ti);
            }
            EmitIsItEvenAnObject(holder, point, typeAddr);
            EmitEnqueueHit(holder, point, typeAddr);
        }
        return false;
    }
    if (!PageMapped(ti)) {
        g_hrPath[HR_TI_UNMAPPED].fetch_add(1, std::memory_order_relaxed);
        if (g_hrSampleEmitted.fetch_add(1, std::memory_order_relaxed) < 64) {
            std::fprintf(stderr, "[GCDISPEL] HASREFFIELD_PATH path=ti_unmapped point=%s holder=%p ti=%p\n",
                         point, holder, ti);
        }
        EmitIsItEvenAnObject(holder, point, typeAddr);
        EmitEnqueueHit(holder, point, typeAddr);
        return false;
    }

    g_hrPath[HR_TYPE_READ].fetch_add(1, std::memory_order_relaxed);
    if (ti->IsArrayType()) {
        g_hrPath[HR_ARRAY_BRANCH].fetch_add(1, std::memory_order_relaxed);
        TypeInfo* componentTi = ti->GetComponentTypeInfo();
        if (componentTi == nullptr) {
            g_hrPath[HR_COMP_NULL].fetch_add(1, std::memory_order_relaxed);
            if (g_hrSampleEmitted.fetch_add(1, std::memory_order_relaxed) < 64) {
                std::fprintf(stderr,
                             "[GCDISPEL] HASREFFIELD_PATH path=comp_null point=%s holder=%p ti=%p\n",
                             point, holder, ti);
            }
            return false;
        }
        uintptr_t compAddr = reinterpret_cast<uintptr_t>(componentTi);
        if (compAddr < 0x1000ULL || !PageMapped(componentTi)) {
            g_hrPath[HR_COMP_UNMAPPED].fetch_add(1, std::memory_order_relaxed);
            if (g_hrSampleEmitted.fetch_add(1, std::memory_order_relaxed) < 64) {
                std::fprintf(stderr,
                             "[GCDISPEL] HASREFFIELD_PATH path=comp_unmapped point=%s holder=%p ti=%p comp=%p\n",
                             point, holder, ti, componentTi);
            }
            return false;
        }
        if (componentTi->IsRef()) {
            g_hrPath[HR_COMP_IS_REF_TRUE].fetch_add(1, std::memory_order_relaxed);
            g_hrPath[HR_RETURN_TRUE].fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        g_hrPath[HR_COMP_RECURSE].fetch_add(1, std::memory_order_relaxed);
        // Nested component HasRefField: only if component itself looks mapped.
        bool r = false;
        if (PageMapped(componentTi)) {
            r = componentTi->HasRefField();
        }
        g_hrPath[r ? HR_RETURN_TRUE : HR_RETURN_FALSE].fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    // Non-array: flag bit. Also check IsVaildType for signature-shift evidence.
    I8 ty = ti->GetType();
    if (ty >= TypeKind::TYPE_KIND_MAX) {
        // type field garbage → would pass IsValidObject, fail deeper
        if (g_hrSampleEmitted.fetch_add(1, std::memory_order_relaxed) < 64) {
            std::fprintf(stderr,
                         "[GCDISPEL] HASREFFIELD_PATH path=ti_type_invalid point=%s holder=%p ti=%p type=%d\n",
                         point, holder, ti, static_cast<int>(ty));
        }
    }
    bool flag = static_cast<bool>(ti->GetFlags() & FLAG_HAS_REF_FIELD);
    g_hrPath[flag ? HR_NONARRAY_FLAG_TRUE : HR_NONARRAY_FLAG_FALSE].fetch_add(1, std::memory_order_relaxed);
    g_hrPath[flag ? HR_RETURN_TRUE : HR_RETURN_FALSE].fetch_add(1, std::memory_order_relaxed);
    return flag;
}

void MaybeSnapAndDiagnose(BaseObject* holder, const char* point, bool liveZeroFocus)
{
    if (!GcdispelOn() || holder == nullptr) {
        return;
    }
    HolderSnap snap{};
    FillSnap(snap, holder, point);
    bool focus = liveZeroFocus || (snap.region != nullptr && snap.live == 0 && snap.young == 0);
    if (std::strcmp(point, "before-return") == 0 && focus) {
        RecordBeforeSnap(holder);
    }
    if (std::strcmp(point, "after-dispel") == 0 && focus) {
        if (g_holderSnapEmitted.fetch_add(1, std::memory_order_relaxed) < 48) {
            EmitSnapLine(snap, "HOLDER_SNAP");
        }
        EmitDeltaIfAny(holder, snap);
    }
}
} // namespace

bool WCollector::IsUnmovableFromObject(BaseObject* obj) const
{
    // filter const string object.
    if (!Heap::IsHeapAddress(obj)) {
        return false;
    }

    RegionInfo* regionInfo = nullptr;
    if (RegionInfo::InGhostFromRegion(obj)) {
        regionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<uintptr_t>(obj));
    } else {
        regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    }
    return regionInfo->IsUnmovableFromRegion();
}

bool WCollector::MarkObject(BaseObject* obj) const
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    size_t objectSize = obj->GetSize();
    bool marked = region->MarkObject(obj, objectSize);
    if (!marked) {
        region->AddLiveByteCount(objectSize);
        (void)region;
        DLOG(TRACE, "mark obj %p<%p>(%zu) in region %p(%u)@%#zx, live %u", obj, obj->GetTypeInfo(), objectSize,
             region, region->GetRegionType(), region->GetRegionStart(), region->GetLiveByteCount());
    }
    return marked;
}

bool WCollector::ResurrectObject(BaseObject* obj, size_t offset, RegionInfo* region)
{
    bool resurrected = region->ResurrectObject(obj, offset);
        if (!resurrected) {
            region->AddLiveByteCount(obj->GetSize());
            DLOG(TRACE, "resurrect region %p@%#zx obj %p<%p>(%zu), live bytes %u", region, region->GetRegionStart(),
                 obj, obj->GetTypeInfo(), obj->GetSize(), region->GetLiveByteCount());
        }
        return resurrected;
}

// this api updates current pointer as well as old pointer, caller should take care of this.
template<bool forward>
bool WCollector::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                       BaseObject*& toObj) const
{
    RefField<> oldRef(field);
    if (oldRef.IsTagged()) {
        fromObj = oldRef.GetTargetObject();
        if (forward) {
            toObj = const_cast<WCollector*>(this)->TryForwardObject(fromObj);
        } else {
            toObj = FindToVersion(fromObj);
        }
        if (toObj == nullptr) {
            return false;
        }
        RefField<> tmpField(toObj);
        if (field.CompareExchange(oldRef.GetFieldValue(), tmpField.GetFieldValue())) {
            if (obj != nullptr) {
                DLOG(TRACE, "update obj %p<%p>(%zu)+%zu ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(),
                     obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field, oldRef.GetFieldValue(),
                     tmpField.GetFieldValue());
            } else {
                DLOG(TRACE, "update ref@%p: 0x%zx -> %p", &field, oldRef.GetFieldValue(), toObj);
            }
            return true;
        } else {
            if (obj != nullptr) {
                DLOG(TRACE,
                     "update obj %p<%p>(%zu)+%zu but cas failed ref-field@%p: %#zx(%#zx) -> %#zx but cas failed ", obj,
                     obj->GetTypeInfo(), obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field,
                     oldRef.GetFieldValue(), field.GetFieldValue(), tmpField.GetFieldValue());
            } else {
                DLOG(TRACE, "update but cas failed ref@%p: 0x%zx(%zx) -> %p", &field, oldRef.GetFieldValue(),
                     field.GetFieldValue(), toObj);
            }
            return true;
        }
    }

    return false;
}
bool WCollector::TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<false>(obj, field, oldRef, newRef);
}

bool WCollector::TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<true>(obj, field, oldRef, newRef);
}
// this api untags current pointer as well as old pointer, caller should take care of this.
bool WCollector::TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const
{
    for (;;) {
        RefField<> oldRef(field);
        if (!oldRef.IsTagged()) {
            return false;
        }
        target = oldRef.GetTargetObject();
        RefField<> newRef(target);
        if (field.CompareExchange(oldRef.GetFieldValue(), newRef.GetFieldValue())) {
            if (obj != nullptr) {
                DLOG(FIX, "untag obj %p<%p>(%zu) ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(), obj->GetSize(),
                     &field, oldRef.GetFieldValue(), newRef.GetFieldValue());
            } else {
                DLOG(FIX, "untag ref@%p: %#zx -> %#zx", &field, oldRef.GetFieldValue(), newRef.GetFieldValue());
            }
            return true;
        }
    }

    return false;
}

// RefFieldRoot is root in tagged pointer format.
void WCollector::EnumRefFieldRoot(RefField<>& field, RootSet& rootSet) const
{
    RefField<> oldField(field);
    // if field is already tagged currently, it is also already enumerated.
    if (IsCurrentPointer(oldField)) {
        rootSet.push_back(oldField.GetTargetObject());
        return;
    }

    BaseObject* latest = nullptr;
    if (IsOldPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        latest = FindLatestVersion(targetObj);
    } else {
        latest = field.GetTargetObject();
    }

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Enum static root %p(%p) encounters invalid object", latest, &field);
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(), latest,
             latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(),
             newField.GetFieldValue(), latest, latest->GetTypeInfo(), latest->GetSize());
    } else {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(), latest,
             latest->GetTypeInfo(), latest->GetSize());
    }
    rootSet.push_back(latest);
}

void WCollector::EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const
{
    RefField<>& refField = reinterpret_cast<RefField<>&>(ref);
    RefField<> oldField(refField);
    CHECK_DETAIL(!IsOldPointer(oldField), "EnumAndTagRawRoot failed: Invalid root: %zx", oldField.GetFieldValue());
    if (IsCurrentPointer(oldField)) {
        rootSet.push_back(oldField.GetTargetObject());
        return;
    }
    BaseObject* root = oldField.GetTargetObject();
    if (Heap::IsHeapAddress(root)) {
        CHECK_DETAIL(root->IsValidObject(), "Enum and tag runtime root %p(%p) encounters invalid object", root, &ref);
        RefField<> newField = GetAndTryTagRefField(root);
        if (oldField.GetFieldValue() == newField.GetFieldValue()) {
            DLOG(ENUM, "enum raw root @%p: %p(%zu)", &ref, root, root->GetSize());
        } else if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
            DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &refField, oldField.GetFieldValue(),
                 newField.GetFieldValue(), root, root->GetTypeInfo(), root->GetSize());
        } else {
            DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &refField, oldField.GetFieldValue(), root,
                 root->GetTypeInfo(), root->GetSize());
        }
        rootSet.push_back(root);
    }
}

// note each ref-field will not be traced twice, so each old pointer the tracer meets must come from previous gc.
void WCollector::TraceRefField(BaseObject* obj, RefField<>& field, WorkStack& workStack) const
{
    RefField<> oldField(field);
    if (IsCurrentPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        if (!IsMarkedObject(targetObj)) {
            workStack.push_back(targetObj);
        }
        return;
    }

    BaseObject* latest = nullptr;
    if (IsOldPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        latest = FindLatestVersion(targetObj);
    } else {
        latest = field.GetTargetObject();
    }

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by object %p: %s and offset %zd", latest,
                 obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, oldField.GetFieldValue(),
             newField.GetFieldValue(), latest, latest->GetTypeInfo(), latest->GetSize());
    }

    if (!IsMarkedObject(latest)) {
        workStack.push_back(latest);
    }
}

void WCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack)
{
    auto visitor = [this, obj, &workStack](RefField<>& field) { TraceRefField(obj, field, workStack); };
    TypeInfo* typeInfo = obj->GetTypeInfo();
    if (!typeInfo->HasRefField()) {
        return;
    }

    if (UNLIKELY(typeInfo->IsRawArray())) {
        MArray* array = reinterpret_cast<MArray*>(obj);
        MIndex arrayLength = array->GetLength();
        TypeInfo* componentTypeInfo = array->GetComponentTypeInfo();
        if (componentTypeInfo->IsStructType()) {
            GCTib gcTib = componentTypeInfo->GetGCTib();
            MAddress contentAddr = reinterpret_cast<Uptr>(array) + MArray::GetContentOffset();
            size_t elementSize = array->GetElementSize();
            for (MIndex i = 0; i < arrayLength; ++i) {
                gcTib.ForEachBitmapWord(contentAddr, visitor);
                contentAddr += elementSize;
            }
        } else if (componentTypeInfo->IsObjectType() || componentTypeInfo->IsArrayType() ||
                   componentTypeInfo->IsInterface()) {
            RefField<>* arrayContent = reinterpret_cast<RefField<>*>(array->ConvertToCArray());
            for (MIndex i = 0; i < arrayLength; ++i) {
                visitor(arrayContent[i]);
            }
        } else {
            LOG(RTLOG_FATAL, "array object %p has wrong component type", array);
        }
        return;
    }

    MAddress contentAddr = reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    obj->GetGCTib().ForEachBitmapWord(contentAddr, visitor);
}

BaseObject* WCollector::GetAndTryTagObj(BaseObject* obj, RefField<>& field)
{
    RefField<> oldField(field);
    BaseObject* latest = nullptr;
    if (IsCurrentPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        return targetObj;
    }
    if (IsOldPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        latest = FindLatestVersion(targetObj);
    } else {
        latest = field.GetTargetObject();
    }
    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return nullptr;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by weak object %p: %s and offset %zd",
                 latest, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, oldField.GetFieldValue(),
            newField.GetFieldValue(), latest, latest->GetTypeInfo(), latest->GetSize());
    }
    return latest;
}

BaseObject* WCollector::ForwardUpdateRawRef(ObjectRef& root)
{
    auto& refField = reinterpret_cast<RefField<>&>(root);
    RefField<> oldField(refField);
    BaseObject* oldObj = oldField.GetTargetObject();
    DLOG(FIX, "visit raw-ref @%p: %p", &root, oldObj);
    CHECK_DETAIL(!IsOldPointer(oldField), "ForwardUpdateRawRef failed: Invalid object: %zx", oldField.GetFieldValue());
    if (IsCurrentPointer(oldField)) {
        if (IsGhostFromObject(oldObj)) {
            BaseObject* toVersion = TryForwardObject(oldObj);
            CHECK(toVersion != nullptr);
            RefField<> newField(toVersion);
            // CAS failure means some mutator or gc thread writes a new ref (must be a to-object), no need to retry.
            if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
                DLOG(FIX, "fix raw-ref @%p: %p -> %p", &root, oldObj, toVersion);
                return toVersion;
            }
            CHECK(!IsCurrentPointer(refField));
        } else {
            RefField<> newField(oldObj);
            // CAS failure means some mutator or gc thread writes a new ref (must be a to-object), no need to retry.
            if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
                DLOG(FIX, "fix raw-ref @%p: %p -> %p", &root, oldObj, oldObj);
                return oldObj;
            }
        }
    }

    return oldObj;
}
void WCollector::PreforwardAllExportFromRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    Heap::GetHeap().VisitAllExportRoots(visitor);
}
void WCollector::PreforwardFinalizerProcessorRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    collectorResources.GetFinalizerProcessor().VisitRawPointers(visitor);
}

void WCollector::PreforwardConcurrencyModelRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitor);
}

void WCollector::PreforwardDiscoveredExternObjects()
{
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CHECK(discoveredExternObjects.empty());
    auto it = cycleRefWorkStack.begin();
    std::unordered_map<BaseObject*, std::list<BaseObject*>> tmp;
    while (it != cycleRefWorkStack.end()) {
        BaseObject* exportObj = it->first;
        BaseObject* latest = exportObj;
        if (IsGhostFromObject(exportObj) && !IsUnmovableFromObject(exportObj)) {
            latest = ForwardObject(exportObj);
        }
        for (auto &externObj : it->second) {
            if (IsGhostFromObject(externObj) && !IsUnmovableFromObject(externObj)) {
                BaseObject* toObj = ForwardObject(externObj);
                externObj = toObj;
            }
        }
        if (latest != exportObj) {
            tmp[latest] = it->second;
            it = cycleRefWorkStack.erase(it);
        } else {
            it++;
        }
    }
    if (!tmp.empty()) {
        cycleRefWorkStack.insert(tmp.begin(), tmp.end());
    }
}

void WCollector::PreforwardAllResurrectExportFromObjects()
{
    std::unordered_set<BaseObject*> tmp;
    std::lock_guard<std::mutex> lg(resurrectExportMtx);
    auto it = resurrectedExportObjectes.begin();
    while (it != resurrectedExportObjectes.end()) {
        BaseObject* exportObj = *it;
        BaseObject* latest = exportObj;
        if (IsGhostFromObject(exportObj) && !IsUnmovableFromObject(exportObj)) {
            latest = ForwardObject(exportObj);
        }
        if (latest != exportObj) {
            tmp.insert(latest);
            it = resurrectedExportObjectes.erase(it);
        } else {
            it++;
        }
    }
    if (!tmp.empty()) {
        resurrectedExportObjectes.insert(tmp.begin(), tmp.end());
    }
}
void WCollector::TraceHeap()
{
    WorkStack workStack = NewWorkStack();
    WorkStack foreignStack = NewWorkStack();
    // assemble garbage candidates for tracing.
    reinterpret_cast<RegionSpace&>(theAllocator).AssembleGarbageCandidates();

    {
        MRT_PHASE_TIMER("enum roots & update old pointers within");
        TransitionToGCPhase(GCPhase::GC_PHASE_ENUM, true);
        DoEnumeration(workStack, foreignStack);
    }

    {
        MRT_PHASE_TIMER("trace live objects & update old pointers in ref-fields");
        markedObjectCount.store(0, std::memory_order_relaxed);
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        DoTracing(workStack, foreignStack);

        ProcessFinalizers();
    }
}

void WCollector::PostTrace()
{
    MRT_PHASE_TIMER("PostTrace");
    TransitionToGCPhase(GC_PHASE_POST_TRACE, true);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    space.GetRegionManager().HandleTraceRegions();
    // clear weakRef List, set the referent as null
    WeakRefBuffer::Instance().ClearWeakRefBuffer();
    // clear satb buffer when gc finish tracing.
    SatbBuffer::Instance().ClearBuffer();
    // reclaim large objects immediately after tracing is done.
    PrepareCycleRef();
    CollectLargeGarbage();
    CollectPinnedGarbage();
    RefineFromSpace();
    fwdTable.PrepareForwardTable();
}

void WCollector::Preforward()
{
    ScopedEntryTrace trace("CJRT_GC_PREFORWARD");
    MRT_PHASE_TIMER("Preforward");
    {
        ScopedLightSync scopedLightSync("Preforward", true, GCPhase::GC_PHASE_PREFORWARD);
    }

    GCThreadPool* threadPool = GetThreadPool();
    MRT_ASSERT(threadPool != nullptr, "thread pool is null");
    // forward and fix cj future objects
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardConcurrencyModelRoots(); }));

    // forward and fix finalizer roots.
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardFinalizerProcessorRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllExportFromRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardDiscoveredExternObjects(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllResurrectExportFromObjects(); }));
    threadPool->Start();
    threadPool->WaitFinish();
}


extern "C" void CJ_MRT_RolveCycleRef();
extern "C" void ResolveCycleRefStub(CrossRefHandler, BaseObject*, BaseObject*, void**);

class CJFunc : public BaseObject {
public:
    CrossRefHandler GetHandler()
    {
        return handler;
    }
private:
    CrossRefHandler handler = nullptr;
};

class CJInteropContext : public BaseObject {
public:
    CJFunc* GetCJFunc()
    {
        return static_cast<CJFunc*>(Heap::GetBarrier().ReadReference(this,
            *reinterpret_cast<RefField<false>*>(&cjFunc)));
    }
private:
    CJFunc* cjFunc = nullptr;
};

class CJForeignProxy : public BaseObject {
public:
    CJInteropContext* GetCJInteropContext()
    {
        return static_cast<CJInteropContext*>(Heap::GetBarrier().ReadReference(this,
            *reinterpret_cast<RefField<false>*>(&interopContext)));
    }
private:
    CJInteropContext* interopContext = nullptr;
};

CrossRefHandler WCollector::GetCrossRefHandler(BaseObject *foreignProxy)
{
    return static_cast<CJForeignProxy*>(foreignProxy)->GetCJInteropContext()->GetCJFunc()->GetHandler();
}

void WCollector::ResolveCycleRef()
{
#if defined (__OHOS__)
    size_t i = 0;
    if (!cycleWorkStackMtx.try_lock()) {
        CJ_MRT_RolveCycleRef();
        return;
    }
    for (auto it = cycleRefWorkStack.begin(); it != cycleRefWorkStack.end(); i++) {
        ScopedObjectAccess soa;
        auto phase = GetGCPhase();
        static constexpr size_t taskNum = 100;
        if (phase == GC_PHASE_PREFORWARD || i >= taskNum) {
            cycleWorkStackMtx.unlock();
            CJ_MRT_RolveCycleRef();
            return;
        }
        BaseObject* exportObj = it->first;
        auto& heap = Heap::GetHeap();
        auto id = static_cast<ExportObject*>(exportObj)->GetId();
        if (!heap.CheckExportObjState(id, exportObj)) {
            it = cycleRefWorkStack.erase(it);
            continue;
        }
        if (resurrectedExportObjectes.find(exportObj) != resurrectedExportObjectes.end() ||
            resurrectedExportObjectesForwardPhase.find(exportObj) != resurrectedExportObjectesForwardPhase.end()) {
            it = cycleRefWorkStack.erase(it);
            continue;
        }
        auto externObjs = it->second;
        void* returnUnit = nullptr;
        for (auto externObj : externObjs) {
            auto resolveHook = GetCrossRefHandler(externObj);
            ResolveCycleRefStub(resolveHook, exportObj, externObj, &returnUnit);
        }
        heap.SetExportObjActiveState(id, false);
        it++;
    }
    cycleWorkStackMtx.unlock();
    resurrectedExportObjectes.clear();
    resurrectedExportObjectesForwardPhase.clear();
#endif
}
void WCollector::PostResolveCycleTask()
{
#if defined (__OHOS__)
    if (cycleRefWorkStack.empty()) {
        return;
    }
    CJ_MRT_RolveCycleRef();
#endif
}

BaseObject* WCollector::ResolveMinorReference(RefField<>& field) const
{
    RefField<> value(field);
    BaseObject* object = value.GetTargetObject();
    if (IsOldPointer(value)) {
        BaseObject* latest = FindLatestVersion(object);
        if (latest != nullptr) {
            field.SetTargetObject(latest);
            return latest;
        }
    }
    return object;
}

void WCollector::VisitMinorRootSlots(RootVisitor& rawRootVisitor, const RefFieldVisitor& fieldVisitor)
{
    MutatorManager::Instance().VisitAllMutators(
        [&rawRootVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(rawRootVisitor); });
    Heap::GetHeap().VisitStaticRoots(fieldVisitor);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rawRootVisitor);
    collectorResources.GetFinalizerProcessor().VisitRawPointers(rawRootVisitor);
    Heap::GetHeap().VisitAllExportRoots(rawRootVisitor);
}

void WCollector::VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
        for (BaseObject* object : resurrectedExportObjectes) {
            visitor(object);
        }
        for (BaseObject* object : resurrectedExportObjectesForwardPhase) {
            visitor(object);
        }
    }
    std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
    for (const auto& entry : cycleRefWorkStack) {
        visitor(entry.first);
        for (BaseObject* object : entry.second) {
            visitor(object);
        }
    }
}

void WCollector::VisitMinorRoots(const std::function<void(BaseObject*)>& visitor)
{
    RootVisitor rawRootVisitor = [this, &visitor](ObjectRef& root) {
        RefField<>& field = reinterpret_cast<RefField<>&>(root);
        visitor(ResolveMinorReference(field));
    };
    RefFieldVisitor fieldVisitor = [this, &visitor](RefField<>& field) {
        BaseObject* target = ResolveMinorReference(field);
        GcInitWin::MinorTargetFate fate = CaptureMinorTargetFate(target);
        GcInitWin::NoteMinorSlotSnapshot(&field, minorTotalRuns + 1, "a-root", fate, fate);
        visitor(target);
    };
    VisitMinorRootSlots(rawRootVisitor, fieldVisitor);
    VisitMinorValueRoots(visitor);
}

void WCollector::PushYoungObject(BaseObject* object, WorkStack& workStack) const
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    CHECK_DETAIL(object->IsValidObject(), "minor root/reference %p is not a valid object", object);
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
    if (region->IsYoungRegion() && !region->IsMarkedObject(object)) {
        workStack.push_back(object);
    }
}

void WCollector::TraceYoungClosure(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                   MinorSlotSet& reachableSlots, MinorSlotSet& weakSlots)
{
    auto pushTarget = [this, fullYoungScan, &workStack](RefField<>& field) {
        BaseObject* target = ResolveMinorReference(field);
        if (fullYoungScan) {
            if (Heap::IsHeapAddress(target)) {
                workStack.push_back(target);
            }
        } else {
            PushYoungObject(target, workStack);
        }
    };
    while (!workStack.empty()) {
        BaseObject* object = workStack.back();
        workStack.pop_back();
        if (!Heap::IsHeapAddress(object) || !reachableObjects.insert(object).second) {
            continue;
        }
        CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region->IsYoungRegion()) {
            (void)MarkObject(object);
        } else if (!fullYoungScan) {
            continue;
        }
        if (!object->HasRefField()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            RefField<>* referentField = reinterpret_cast<RefField<>*>(
                reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            weakSlots.insert(reinterpret_cast<MAddress>(referentField));
            BaseObject* referent = ResolveMinorReference(*referentField);
            if (!Heap::IsHeapAddress(referent)) {
                continue;
            }
            RegionInfo* referentRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(referent));
            if (referentRegion->IsYoungRegion()) {
                WeakRefBuffer::Instance().Insert(object);
            }
            referent->ForEachRefField([&pushTarget](RefField<>& field) { pushTarget(field); });
            continue;
        }
        object->ForEachRefField([&reachableSlots, &pushTarget](RefField<>& field) {
            reachableSlots.insert(reinterpret_cast<MAddress>(&field));
            pushTarget(field);
        });
    }
}

void WCollector::RescanRememberedSet(WorkStack& workStack, const MinorSlotSet& rememberedSlots,
                                     const MinorSlotSet& reachableSlots, const MinorSlotSet& weakSlots,
                                     bool fullYoungScan)
{
    for (MAddress slot : rememberedSlots) {
        if (!Heap::IsHeapAddress(slot) || weakSlots.count(slot) != 0 ||
            (fullYoungScan && reachableSlots.count(slot) == 0)) {
            continue;
        }
        RefField<>* field = reinterpret_cast<RefField<>*>(slot);
        PushYoungObject(ResolveMinorReference(*field), workStack);
    }
}

bool WCollector::FixMinorEvacuatedSlot(RefField<>& field) const
{
    RefField<> oldField(field);
    BaseObject* target = ResolveMinorReference(field);
    BaseObject* current = target;
    if (Heap::IsHeapAddress(target) && IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
        current = const_cast<WCollector*>(this)->ForwardObject(target);
    }
    RefField<> newField(current);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        return false;
    }
    CHECK_DETAIL(field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue()),
                 "minor reference changed while the world was stopped field=%p from=%p to=%p",
                 &field, target, current);
    return true;
}

void WCollector::FixMinorRootSlots(const char* moment)
{
    RootVisitor rawRootVisitor = [this](ObjectRef& root) {
        RefField<>& field = reinterpret_cast<RefField<>&>(root);
        (void)FixMinorEvacuatedSlot(field);
    };
    RefFieldVisitor fieldVisitor = [this, moment](RefField<>& field) {
        (void)FixMinorEvacuatedSlot(field);
        RefField<> value(field);
        BaseObject* target = value.GetTargetObject();
        uint64_t round = minorTotalRuns + 1;
        const void* initialTarget = GcInitWin::InitialMinorTarget(&field, round);
        GcInitWin::NoteMinorSlotSnapshot(&field, round, moment, CaptureMinorTargetFate(target),
                                         CaptureMinorTargetFate(initialTarget));
    };
    Heap::GetHeap().VisitStaticRoots(fieldVisitor);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rawRootVisitor);
    collectorResources.GetFinalizerProcessor().VisitRawPointers(rawRootVisitor);
    Heap::GetHeap().VisitAllExportRoots(rawRootVisitor);
}

void WCollector::FixMinorObjectSlots(BaseObject* object)
{
    if (!object->HasRefField()) {
        return;
    }
    object->ForEachRefField([this](RefField<>& field) { (void)FixMinorEvacuatedSlot(field); });
}

void WCollector::EvacuateYoungRegions(const MinorObjectSet& reachableObjects, const MinorSlotSet& rememberedSlots)
{
    RegionManager& manager = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    minorEvacuatedObjects.store(0, std::memory_order_relaxed);
    minorEvacuatedBytes.store(0, std::memory_order_relaxed);
    auto currentObject = [this](BaseObject* object) {
        if (IsGhostFromObject(object) && !IsUnmovableFromObject(object)) {
            return ForwardObject(object);
        }
        return object;
    };

    auto fixForwardedReferences = [this, &reachableObjects, &rememberedSlots, &currentObject]() {
        FixMinorRootSlots("b-post-fix");
        PreforwardDiscoveredExternObjects();
        PreforwardAllResurrectExportFromObjects();
        for (BaseObject* object : reachableObjects) {
            FixMinorObjectSlots(currentObject(object));
        }
        for (MAddress slot : rememberedSlots) {
            if (Heap::IsHeapAddress(slot)) {
                (void)FixMinorEvacuatedSlot(*reinterpret_cast<RefField<>*>(slot));
            }
        }
    };

    // pre-evacuate: collection set already in fromRegionList via PrepareYoungGarbageCandidates
    TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);
    FixMinorRootSlots("b-pre-fix");
    PreforwardDiscoveredExternObjects();
    PreforwardAllResurrectExportFromObjects();

    TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
    fwdTable.PrepareForwardTable();
    TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);
    // evacuate + fix references (roots / holders / remembered slots)
    fixForwardedReferences();
    ValidateMinorReferences("before-return", &reachableObjects);

    ForwardFromSpace();

    for (RegionInfo* region : minorCandidateRegions) {
        if (region->IsYoungRegion()) {
            region->SetYoungRegionFlag(0);
            region->SetYoungAge(0);
        }
    }

    // post-evacuate: rebuild remembered set for survivors that remain young
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    size_t rebuiltRecords = 0;
    for (BaseObject* object : reachableObjects) {
        BaseObject* holder = currentObject(object);
        RegionInfo* holderRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(holder));
        if (holderRegion->IsYoungRegion()) {
            continue;
        }
        // GCDISPEL: probe HasRefField on remset-rebuild path (after young flag clear, before after-dispel).
        bool hasRef = false;
        if (GcdispelOn()) {
            bool live0 = holderRegion->GetLiveByteCount() == 0;
            MaybeSnapAndDiagnose(holder, "remset-rebuild", live0);
            hasRef = DiagnoseHasRefField(holder, "remset-rebuild", /*doRealCall=*/true);
        } else {
            hasRef = holder->HasRefField();
        }
        if (!hasRef) {
            continue;
        }
        holder->ForEachRefField([this, &rememberedSet, &rebuiltRecords](RefField<>& field) {
            BaseObject* target = ResolveMinorReference(field);
            if (!Heap::IsHeapAddress(target)) {
                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion->IsYoungRegion()) {
                rememberedSet.Record(reinterpret_cast<MAddress>(&field));
                ++rebuiltRecords;
            }
        });
    }
    size_t evacuatedObjects = minorEvacuatedObjects.load(std::memory_order_relaxed);
    size_t evacuatedBytes = minorEvacuatedBytes.load(std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2Minor] evacuation objects=%zu bytes=%zu remembered-set rebuilt=%zu",
         evacuatedObjects, evacuatedBytes, rebuiltRecords);

    // dispel ghost routes before reassembly (c51e156d order)
    fwdTable.PrepareForwardTable();
    ValidateMinorReferences("after-dispel", nullptr);
    manager.ReassembleFromSpace();
}

void WCollector::ValidateMinorReferences(const char* point, const MinorObjectSet* reachableObjects)
{
    const char* enabled = std::getenv("MRT_GCV2_STALE_REFERENCE_VALIDATOR");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
        return;
    }

    constexpr size_t categoryCount = 12;
    constexpr size_t sampleCount = 3;
    const std::array<const char*, categoryCount> categoryNames = {
        "stack", "register", "derived", "static", "heap", "weak", "finalizer", "export",
        "concurrency", "external_resurrection", "exception", "raw_object"
    };
    std::array<size_t, categoryCount> counts{};
    std::array<std::array<const void*, sampleCount>, categoryCount> slots{};
    std::array<std::array<BaseObject*, sampleCount>, categoryCount> holders{};
    std::array<std::array<BaseObject*, sampleCount>, categoryCount> targets{};
    std::array<std::array<uint8_t, sampleCount>, categoryCount> regionTypes{};
    std::array<std::array<uint8_t, sampleCount>, categoryCount> objectStates{};
    std::array<std::array<uint16_t, sampleCount>, categoryCount> tags{};
    WorkStack pending = NewWorkStack();
    MinorObjectSet visited;
    bool buildReachableClosure = reachableObjects == nullptr;

    auto record = [this, &counts, &slots, &holders, &targets, &regionTypes, &objectStates, &tags](
                      size_t category, const void* slot, BaseObject* holder, BaseObject* target, uint16_t tag) {
        if (!Heap::IsHeapAddress(target) || !IsGhostFromObject(target) || IsUnmovableFromObject(target)) {
            return false;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        bool regionReturned = region == nullptr || region->IsGarbageRegion() || region->IsFreeRegion();
        ObjectState::ObjectStateCode state = target->GetStateWord().GetStateCode();
        if (!regionReturned && state != ObjectState::FORWARDED) {
            return false;
        }
        size_t sample = counts[category]++;
        if (sample < sampleCount) {
            slots[category][sample] = slot;
            holders[category][sample] = holder;
            targets[category][sample] = target;
            regionTypes[category][sample] =
                region == nullptr ? std::numeric_limits<uint8_t>::max() : static_cast<uint8_t>(region->GetRegionType());
            objectStates[category][sample] = static_cast<uint8_t>(state);
            tags[category][sample] = tag;
        }
        return true;
    };
    auto inspectTarget = [&record, &pending, buildReachableClosure, point](
                             size_t category, const void* slot, BaseObject* holder, BaseObject* target, uint16_t tag) {
        if (record(category, slot, holder, target, tag)) {
            return;
        }
        if (!buildReachableClosure || !Heap::IsHeapAddress(target)) {
            return;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (region != nullptr && !region->IsGarbageRegion() && !region->IsFreeRegion() && target->IsValidObject()) {
            // gcenqueue: record who enqueued this addr into after-dispel closure.
            RecordEnqueue(target, holder, slot, category, point);
            pending.push_back(target);
        }
    };
    auto recordRawRoot = [&inspectTarget](size_t category) {
        return RootVisitor([category, &inspectTarget](ObjectRef& root) {
            RefField<> value = reinterpret_cast<RefField<>&>(root);
            uint16_t tag = value.IsTagged() ? value.GetTagID() : std::numeric_limits<uint16_t>::max();
            inspectTarget(category, &root, nullptr, value.GetTargetObject(), tag);
        });
    };
    auto recordField = [&inspectTarget](size_t category, BaseObject* holder, RefField<>& field) {
        RefField<> value(field);
        uint16_t tag = value.IsTagged() ? value.GetTagID() : std::numeric_limits<uint16_t>::max();
        inspectTarget(category, &field, holder, value.GetTargetObject(), tag);
    };

    RootVisitor stackVisitor = recordRawRoot(0);
    RootVisitor registerVisitor = recordRawRoot(1);
    DerivedPtrVisitor derivedVisitor = [&inspectTarget](BasePtrType basePtr, DerivedPtrType& derivedPtr) {
        inspectTarget(2, &derivedPtr, nullptr, reinterpret_cast<BaseObject*>(basePtr),
                      std::numeric_limits<uint16_t>::max());
    };
    RootVisitor exceptionVisitor = recordRawRoot(10);
    RootVisitor rawObjectVisitor = recordRawRoot(11);
    MutatorManager::Instance().VisitAllMutators(
        [&registerVisitor, &stackVisitor, &derivedVisitor, &exceptionVisitor, &rawObjectVisitor](Mutator& mutator) {
            mutator.VisitHeapReferences(
                registerVisitor, stackVisitor, derivedVisitor, exceptionVisitor, rawObjectVisitor);
        });

    Heap::GetHeap().VisitStaticRoots(
        [&recordField](RefField<>& field) { recordField(3, nullptr, field); });
    collectorResources.GetFinalizerProcessor().VisitRawPointers(recordRawRoot(6));
    Heap::GetHeap().VisitAllExportRoots(recordRawRoot(7));
    RootVisitor concurrencyVisitor = recordRawRoot(8);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&concurrencyVisitor);

    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
        for (BaseObject* const& object : resurrectedExportObjectes) {
            inspectTarget(9, &object, nullptr, object, std::numeric_limits<uint16_t>::max());
        }
        for (BaseObject* const& object : resurrectedExportObjectesForwardPhase) {
            inspectTarget(9, &object, nullptr, object, std::numeric_limits<uint16_t>::max());
        }
    }
    {
        std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
        for (const auto& entry : cycleRefWorkStack) {
            inspectTarget(9, &entry.first, nullptr, entry.first, std::numeric_limits<uint16_t>::max());
            for (BaseObject* const& object : entry.second) {
                inspectTarget(9, &object, nullptr, object, std::numeric_limits<uint16_t>::max());
            }
        }
    }

    auto visitObject = [this, &recordField, point](BaseObject* object) {
        BaseObject* holder = object;
        if (IsGhostFromObject(holder) && !IsUnmovableFromObject(holder) &&
            holder->GetStateWord().GetStateCode() == ObjectState::FORWARDED) {
            holder = FindLatestVersion(holder);
        }
        if (holder == nullptr || IsGhostFromObject(holder) || !holder->IsValidObject()) {
            return;
        }
        // GCDISPEL: classify HasRefField before the real call that may hard-fault.
        if (GcdispelOn()) {
            RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            bool live0 = hr != nullptr && hr->GetLiveByteCount() == 0 && !hr->IsYoungRegion();
            MaybeSnapAndDiagnose(holder, point, live0);
            bool hasRef = DiagnoseHasRefField(holder, point, /*doRealCall=*/false);
            if (!hasRef) {
                // On after-dispel fail: emit full snap for the failing holder (Q1).
                if (std::strcmp(point, "after-dispel") == 0) {
                    HolderSnap failSnap{};
                    FillSnap(failSnap, holder, "after-dispel-fail");
                    if (g_holderSnapEmitted.fetch_add(1, std::memory_order_relaxed) < 96) {
                        EmitSnapLine(failSnap, "HOLDER_SNAP_FAIL");
                    }
                    EmitDeltaIfAny(holder, failSnap);
                    static std::atomic<int> failDump{0};
                    if (failDump.fetch_add(1, std::memory_order_relaxed) == 0) {
                        DumpHrSummary("first-hasref-fail");
                    }
                }
                return;
            }
        } else if (!holder->HasRefField()) {
            return;
        }
        size_t category = holder->IsWeakRef() ? 5 : 4;
        holder->ForEachRefField(
            [category, holder, &recordField](RefField<>& field) { recordField(category, holder, field); });
    };
    if (reachableObjects != nullptr) {
        for (BaseObject* object : *reachableObjects) {
            visitObject(object);
        }
    } else {
        while (!pending.empty()) {
            BaseObject* object = pending.back();
            pending.pop_back();
            if (visited.insert(object).second) {
                visitObject(object);
            }
        }
    }

    size_t total = 0;
    for (size_t category = 0; category < categoryCount; ++category) {
        total += counts[category];
        VLOG(REPORT,
             "[GCV2Minor] STALE_SLOT_CATEGORY_%s point=%s count=%zu "
             "samples=[%p/%p/%p/type=%u/state=%u/tag=%u,%p/%p/%p/type=%u/state=%u/tag=%u,"
             "%p/%p/%p/type=%u/state=%u/tag=%u]",
             categoryNames[category], point, counts[category], slots[category][0], holders[category][0],
             targets[category][0], static_cast<unsigned>(regionTypes[category][0]),
             static_cast<unsigned>(objectStates[category][0]), static_cast<unsigned>(tags[category][0]),
             slots[category][1], holders[category][1], targets[category][1],
             static_cast<unsigned>(regionTypes[category][1]), static_cast<unsigned>(objectStates[category][1]),
             static_cast<unsigned>(tags[category][1]), slots[category][2], holders[category][2], targets[category][2],
             static_cast<unsigned>(regionTypes[category][2]), static_cast<unsigned>(objectStates[category][2]),
             static_cast<unsigned>(tags[category][2]));
    }
    VLOG(REPORT, "[GCV2Minor] VALIDATOR_GATED_BY_MRT_GCV2_STALE_REFERENCE_VALIDATOR point=%s total=%zu",
         point, total);
    if (std::strcmp(point, "round2-start") == 0) {
        VLOG(REPORT, "[GCV2Minor] STALE_SLOT_AT_ROUND2_START_%zu", total);
    }
    if (GcdispelOn()) {
        DumpHrSummary(point);
        DumpEnqSummary(point);
    }
}

void WCollector::ValidateYoungMarking(const MinorObjectSet& reachableObjects, const MinorObjectSet& allocationRoots)
{
    MinorObjectSet reachable;
    MinorObjectSet expectedYoung;
    WorkStack pending = NewWorkStack();
    VisitMinorRoots([&pending](BaseObject* object) {
        if (Heap::IsHeapAddress(object)) {
            pending.push_back(object);
        }
    });
    for (BaseObject* object : allocationRoots) {
        pending.push_back(object);
    }
    auto pushField = [this, &pending](RefField<>& field) {
        BaseObject* target = ResolveMinorReference(field);
        if (Heap::IsHeapAddress(target)) {
            pending.push_back(target);
        }
    };
    while (!pending.empty()) {
        BaseObject* object = pending.back();
        pending.pop_back();
        if (!reachable.insert(object).second) {
            continue;
        }
        CHECK_DETAIL(object->IsValidObject(), "minor marking validator reached invalid object %p", object);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region->IsYoungRegion()) {
            expectedYoung.insert(object);
        }
        if (!object->HasRefField()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            RefField<>* referentField = reinterpret_cast<RefField<>*>(
                reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            BaseObject* referent = ResolveMinorReference(*referentField);
            if (Heap::IsHeapAddress(referent)) {
                referent->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
            }
            continue;
        }
        object->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
    }

    size_t actualYoung = 0;
    size_t unexpectedYoung = 0;
    for (RegionInfo* region : minorCandidateRegions) {
        region->VisitAllObjects([&](BaseObject* object) {
            if (!region->IsMarkedObject(object)) {
                return;
            }
            ++actualYoung;
            if (expectedYoung.count(object) == 0 || reachableObjects.count(object) == 0) {
                ++unexpectedYoung;
            }
        });
    }
    size_t missingYoung = 0;
    for (BaseObject* object : expectedYoung) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (!region->IsMarkedObject(object) || reachableObjects.count(object) == 0) {
            ++missingYoung;
        }
    }
    VLOG(REPORT, "[GCV2Minor] mark-equivalence=%zu/%zu missing=%zu unexpected=%zu",
         actualYoung - unexpectedYoung, expectedYoung.size(), missingYoung, unexpectedYoung);
    CHECK_DETAIL(missingYoung == 0 && unexpectedYoung == 0 && actualYoung == expectedYoung.size(),
                 "minor marking differs from full marking: actual=%zu expected=%zu missing=%zu unexpected=%zu",
                 actualYoung, expectedYoung.size(), missingYoung, unexpectedYoung);
}

void WCollector::FlushAllocationRegions()
{
    theAllocator.VisitAllocBuffers([](AllocBuffer& buffer) { buffer.FlushRegion(); });
}

void WCollector::DoYoungGarbageCollection()
{
    uint64_t start = TimeUtil::NanoSeconds();
    ScopedStopTheWorld stw("young collection", true, GCPhase::GC_PHASE_ENUM);
    GcInitWin::NoteMinorCycleStart(minorTotalRuns + 1);
    TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
    FlushAllocationRegions();
    if (minorTotalRuns != 0) {
        ValidateMinorReferences("round2-start", nullptr);
    }

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    minorCandidateRegions.clear();
    YoungCollectionStats stats = manager.PrepareYoungGarbageCandidates(
        [this](RegionInfo* region) { minorCandidateRegions.insert(region); });
    if (stats.candidateRegions == 0) {
        manager.ReassembleFromSpace();
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
        ++minorTotalRuns;
        VLOG(REPORT, "[GCV2Minor] run=%zu candidates=0 candidateBytes=0 live=0 reclaimedBytes=0",
             minorTotalRuns);
        return;
    }

    MinorSlotSet rememberedSlots;
    {
        RememberedSet::Records records = Heap::GetHeap().GetRememberedSet().AcquireRecordsForMinor();
        rememberedSlots.insert(records.begin(), records.end());
    }

    const char* fallback = std::getenv("MRT_GCV2_FULL_YOUNG_SCAN");
    bool fullYoungScan = fallback == nullptr || std::strcmp(fallback, "0") != 0;
    WorkStack workStack = NewWorkStack();
    MinorObjectSet reachableObjects;
    MinorObjectSet allocationRoots;
    MinorSlotSet reachableSlots;
    MinorSlotSet weakSlots;
    WorkStack enumRoots = NewWorkStack();
    theAllocator.VisitAllocBuffers([&enumRoots](AllocBuffer& buffer) { buffer.MergeRoots(enumRoots); });
    while (!enumRoots.empty()) {
        BaseObject* object = enumRoots.back();
        enumRoots.pop_back();
        if (Heap::IsHeapAddress(object)) {
            allocationRoots.insert(object);
        }
        if (fullYoungScan) {
            if (Heap::IsHeapAddress(object)) {
                workStack.push_back(object);
            }
        } else {
            PushYoungObject(object, workStack);
        }
    }
    VisitMinorRoots([this, fullYoungScan, &workStack](BaseObject* object) {
        if (fullYoungScan) {
            if (Heap::IsHeapAddress(object)) {
                workStack.push_back(object);
            }
        } else {
            PushYoungObject(object, workStack);
        }
    });
    TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableSlots, weakSlots);
    MinorSlotSet liveRememberedSlots;
    for (MAddress slot : rememberedSlots) {
        if (weakSlots.count(slot) == 0 && (!fullYoungScan || reachableSlots.count(slot) != 0)) {
            liveRememberedSlots.insert(slot);
        }
    }
    RescanRememberedSet(workStack, liveRememberedSlots, reachableSlots, weakSlots, fullYoungScan);
    TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableSlots, weakSlots);

    size_t liveObjects = 0;
    size_t liveBytes = 0;
    for (RegionInfo* region : minorCandidateRegions) {
        liveBytes += region->GetLiveByteCount();
        region->VisitAllObjects([&](BaseObject* object) {
            if (region->IsMarkedObject(object)) {
                ++liveObjects;
            }
        });
    }
    if (fullYoungScan) {
        ValidateYoungMarking(reachableObjects, allocationRoots);
    }

    TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
    WeakRefBuffer::Instance().ClearWeakRefBuffer();
    SatbBuffer::Instance().ClearBuffer();

    size_t allocatedBefore = space.AllocatedBytes();
    EvacuateYoungRegions(reachableObjects, liveRememberedSlots);
    size_t allocatedAfter = space.AllocatedBytes();
    stats.reclaimedBytes = allocatedBefore > allocatedAfter ? allocatedBefore - allocatedAfter : 0;
    GetGCStats().collectedBytes = stats.reclaimedBytes;

    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    MergeResurrectExportObjects();
    ++minorTotalRuns;
    uint64_t pauseUs = (TimeUtil::NanoSeconds() - start) / NS_PER_US;
    VLOG(REPORT,
         "[GCV2Minor] run=%zu fallbackFullScan=%u candidates=%zu candidateBytes=%zu live=%zu liveBytes=%zu "
         "remembered=%zu reclaimedBytes=%zu evacuatedObjects=%zu evacuatedBytes=%zu pause=%zu us",
         minorTotalRuns, static_cast<unsigned>(fullYoungScan), stats.candidateRegions, stats.candidateBytes,
         liveObjects, liveBytes, liveRememberedSlots.size(), stats.reclaimedBytes,
         minorEvacuatedObjects.load(std::memory_order_relaxed),
         minorEvacuatedBytes.load(std::memory_order_relaxed), pauseUs);
}

void WCollector::DoGarbageCollection()
{
    if (gcReason == GC_REASON_YOUNG) {
        DoYoungGarbageCollection();
        return;
    }
    TraceHeap();
    PostTrace();

    Preforward();

    ForwardFromSpace();

    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    MergeResurrectExportObjects();
    PostResolveCycleTask();
    FlipTagID();
    ForwardDataManager::GetForwardDataManager().SetTagID(currentTagID);

    CollectSmallSpace();
    ForwardDataManager::GetForwardDataManager().UnbindPreviousLiveInfo();
}

void WCollector::MarkNewObject(BaseObject* obj)
{
    GCPhase mutatorPhase = Mutator::GetMutator()->GetMutatorPhase();
    if (UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_ENUM) || UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_TRACE) ||
        UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER)) {
        MarkObject(obj);
    }
}

void WCollector::ProcessFinalizers()
{
    std::function<bool(BaseObject*)> finalizable = [this](BaseObject* obj) { return !IsMarkedObject(obj); };
    FinalizerProcessor& fp = collectorResources.GetFinalizerProcessor();
    fp.EnqueueFinalizables(finalizable, snapshotFinalizerNum);
    fp.Notify();
}

BaseObject* WCollector::ForwardObject(BaseObject* obj)
{
    BaseObject* to = TryForwardObject(obj);
    return (to != nullptr) ? to : obj;
}

BaseObject* WCollector::TryForwardObject(BaseObject* obj)
{
    RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
        return nullptr;
    }

    if (fwdTable.RouteRegion(region)) {
        if (region->TryLockReadFromRegion()) {
            BaseObject* toVersion = ForwardObjectImpl(obj, region);
            region->UnlockReadFromRegion();
            return toVersion;
        } else {
            return FindToVersion(obj);
        }
    } else if (region->IsCompacted()) {
        return FindToVersion(obj);
    }
    return nullptr;
}

BaseObject* WCollector::ForwardObjectImpl(BaseObject* obj, RegionInfo* ghostFromRegion)
{
    CHECK(GetGCPhase() == GCPhase::GC_PHASE_PREFORWARD || GetGCPhase() == GCPhase::GC_PHASE_FORWARD);
    do {
        StateWord oldWord = obj->GetStateWord();

        // 1. object has already been forwarded
        if (obj->IsForwarded()) {
            auto toObj = GetForwardPointer(obj, ghostFromRegion);
            DLOG(FORWARD, "skip forwarded obj %p -> %p<%p>(%zu)", obj, toObj, toObj->GetTypeInfo(), toObj->GetSize());
            return toObj;
        }

        // 2. object is being forwarded, spin until it is forwarded (or gets its own forwarded address)
        if (oldWord.IsLockedWord()) {
            sched_yield();
            continue;
        }

        // 3. hope we can forward this object
        if (obj->TryLockObject(oldWord)) {
            return ForwardObjectExclusive(obj);
        }
    } while (true);
    LOG(RTLOG_FATAL, "forwardObject exit in wrong path");
    return nullptr;
}

BaseObject* WCollector::ForwardObjectExclusive(BaseObject* obj)
{
    size_t size = RegionSpace::GetAllocSize(*obj);
    BaseObject* toObj = fwdTable.RouteObject(obj);
    CHECK_DETAIL(toObj != nullptr, "invalid object route");
    DLOG(FORWARD, "forward obj %p<%p>(%zu) to %p", obj, obj->GetTypeInfo(), size, toObj);
    CopyObject(*obj, *toObj, size);
    toObj->SetStateCode(ObjectState::NORMAL);
    std::atomic_thread_fence(std::memory_order_release);
    obj->UnlockObject(ObjectState::FORWARDED);
    if (gcReason == GC_REASON_YOUNG) {
        minorEvacuatedObjects.fetch_add(1, std::memory_order_relaxed);
        minorEvacuatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    return toObj;
}

void WCollector::CollectSmallSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    {
        MRT_PHASE_TIMER("CollectFromSpaceGarbage");
        stats.collectedBytes += stats.smallGarbageSize;
        space.CollectFromSpaceGarbage();
    }

    size_t candidateBytes = stats.fromSpaceSize + stats.pinnedSpaceSize + stats.largeSpaceSize;
    stats.garbageRatio = (candidateBytes > 0) ? static_cast<float>(stats.collectedBytes) / candidateBytes : 0;

    stats.liveBytesAfterGC = space.AllocatedBytes();

    VLOG(REPORT,
         "collect %zu B: old small %zu - %zu B, old pinned %zu - %zu B, old large %zu - %zu B. garbage ratio %.2f%%",
         stats.collectedBytes, stats.fromSpaceSize, stats.smallGarbageSize, stats.pinnedSpaceSize,
         stats.pinnedGarbageSize, stats.largeSpaceSize, stats.largeGarbageSize,
         stats.garbageRatio * 100); // The base of the percentage is 100

    VLOG(REPORT, "start to release heap garbage memory");
#if defined(__EULER__)
    Heap::GetHeap().GetAllocator().TryReclaimGarbageMemory();
#endif
    collectorResources.GetFinalizerProcessor().NotifyToReclaimGarbage();
}

bool WCollector::ShouldIgnoreRequest(GCRequest& request) { return request.ShouldBeIgnored(); }
} // namespace MapleRuntime
