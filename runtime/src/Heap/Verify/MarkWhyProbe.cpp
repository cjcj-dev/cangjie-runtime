// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "MarkWhyProbe.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/MClass.h"

namespace MapleRuntime {
namespace {

// Always-on stderr: VLOG(REPORT) is gated off by DEFAULT_MRT_REPORT=0.
#define MARKWHY_LOG(fmt, ...)                                                                                          \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][mark-why] " fmt "\n", ##__VA_ARGS__);                                              \
        std::fflush(stderr);                                                                                           \
    } while (0)

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v) {
        return def;
    }
    return static_cast<size_t>(n);
}

// Classify mapping for addr via /proc/self/maps (same moment as FAIL). Returns kind token.
// kind: HEAP_REGION | ANON | FILE_RX | FILE_RW | STACK | HEAP_BRK | UNKNOWN | UNMAPPED
const char* ClassifyMapsKind(const char* perms, const char* path, uintptr_t lo, uintptr_t hi, uintptr_t addr,
                             uintptr_t regionStart, uintptr_t regionEnd)
{
    (void)hi;
    (void)addr;
    if (regionStart != 0 && addr >= regionStart && addr < regionEnd) {
        return "HEAP_REGION";
    }
    bool r = perms[0] == 'r';
    bool w = perms[1] == 'w';
    bool x = perms[2] == 'x';
    if (path != nullptr && path[0] == '[') {
        if (std::strncmp(path, "[stack", 6) == 0) {
            return "STACK";
        }
        if (std::strcmp(path, "[heap]") == 0) {
            return "HEAP_BRK";
        }
        if (std::strncmp(path, "[vdso", 5) == 0 || std::strncmp(path, "[vvar", 5) == 0) {
            return "VDSO";
        }
        return "ANON_NAMED";
    }
    if (path == nullptr || path[0] == '\0') {
        if (r && w && !x) {
            return "ANON_RW";
        }
        if (r && !w && x) {
            return "ANON_RX";
        }
        if (r && w && x) {
            return "ANON_RWX";
        }
        return "ANON";
    }
    if (r && x && !w) {
        return "FILE_RX";
    }
    if (r && w && !x) {
        return "FILE_RW";
    }
    if (r && !w && !x) {
        return "FILE_RO";
    }
    return "FILE_OTHER";
}

// Find maps line containing addr; fill lo/hi/offset/perms/path. Return true if found.
bool FindMapsEntry(uintptr_t addr, uintptr_t* loOut, uintptr_t* hiOut, uintptr_t* offOut, char* permsOut,
                   char* pathOut, size_t pathCap)
{
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) {
        return false;
    }
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        uintptr_t lo = 0;
        uintptr_t hi = 0;
        uintptr_t off = 0;
        char perms[8] = {};
        // path may be empty
        char path[400] = {};
        // format: start-end perms offset dev inode pathname
        int n = std::sscanf(line, "%lx-%lx %7s %lx %*s %*s %399[^\n]", &lo, &hi, perms, &off, path);
        if (n < 4) {
            continue;
        }
        if (addr >= lo && addr < hi) {
            *loOut = lo;
            *hiOut = hi;
            *offOut = off;
            std::strncpy(permsOut, perms, 7);
            permsOut[7] = '\0';
            if (n >= 5) {
                // trim leading spaces in path
                const char* p = path;
                while (*p == ' ' || *p == '\t') {
                    ++p;
                }
                std::strncpy(pathOut, p, pathCap - 1);
                pathOut[pathCap - 1] = '\0';
            } else {
                pathOut[0] = '\0';
            }
            found = true;
            break;
        }
    }
    std::fclose(f);
    return found;
}

void DumpHexAround(const char* tag, const void* center, size_t before, size_t after)
{
    if (center == nullptr) {
        MARKWHY_LOG("%s hex: null", tag);
        return;
    }
    auto c = reinterpret_cast<uintptr_t>(center);
    uintptr_t lo = (c >= before) ? (c - before) : 0;
    uintptr_t hi = c + after;
    // clamp to page-readable range via maps
    uintptr_t mapLo = 0;
    uintptr_t mapHi = 0;
    uintptr_t mapOff = 0;
    char perms[8] = {};
    char path[400] = {};
    if (!FindMapsEntry(c, &mapLo, &mapHi, &mapOff, perms, path, sizeof(path))) {
        MARKWHY_LOG("%s hex: UNMAPPED center=%p", tag, center);
        return;
    }
    if (lo < mapLo) {
        lo = mapLo;
    }
    if (hi > mapHi) {
        hi = mapHi;
    }
    if (lo >= hi) {
        MARKWHY_LOG("%s hex: empty window", tag);
        return;
    }
    size_t len = static_cast<size_t>(hi - lo);
    // cap dump
    if (len > 256) {
        // keep centered window
        uintptr_t mid = c;
        lo = (mid > 64) ? (mid - 64) : mapLo;
        hi = mid + 64;
        if (lo < mapLo) {
            lo = mapLo;
        }
        if (hi > mapHi) {
            hi = mapHi;
        }
        len = static_cast<size_t>(hi - lo);
    }
    MARKWHY_LOG("%s hex lo=%#zx hi=%#zx len=%zu center=%p", tag, lo, hi, len, center);
    char hexLine[3 * 16 + 8];
    const auto* bytes = reinterpret_cast<const unsigned char*>(lo);
    for (size_t i = 0; i < len; i += 16) {
        size_t chunk = (len - i < 16) ? (len - i) : 16;
        size_t pos = 0;
        for (size_t j = 0; j < chunk && pos + 3 < sizeof(hexLine); ++j) {
            pos += static_cast<size_t>(std::snprintf(hexLine + pos, sizeof(hexLine) - pos, "%02x ", bytes[i + j]));
        }
        MARKWHY_LOG("%s  %#zx: %s", tag, lo + i, hexLine);
    }
}

void DumpMapsForAddr(const char* tag, uintptr_t addr, uintptr_t regionStart, uintptr_t regionEnd)
{
    uintptr_t lo = 0;
    uintptr_t hi = 0;
    uintptr_t off = 0;
    char perms[8] = {};
    char path[400] = {};
    if (!FindMapsEntry(addr, &lo, &hi, &off, perms, path, sizeof(path))) {
        MARKWHY_LOG("%s maps: UNMAPPED addr=%#zx", tag, addr);
        return;
    }
    const char* kind = ClassifyMapsKind(perms, path, lo, hi, addr, regionStart, regionEnd);
    uintptr_t fileOff = off + (addr - lo);
    MARKWHY_LOG("%s maps: kind=%s addr=%#zx range=%#zx-%#zx perms=%s fileOff=%#zx path=%s", tag, kind, addr, lo, hi,
                perms, fileOff, path[0] ? path : "(anon)");
    // bit48 for pointer shape (tagged ref family)
    unsigned bit48 = static_cast<unsigned>((addr >> 48) & 1u);
    unsigned bit49 = static_cast<unsigned>((addr >> 49) & 1u);
    MARKWHY_LOG("%s ptrbits: bit48=%u bit49=%u high16=%#x low48=%#zx", tag, bit48, bit49,
                static_cast<unsigned>((addr >> 48) & 0xffffu), addr & ((UINT64_C(1) << 48) - 1));
}

void DumpBacktrace(const char* tag)
{
    void* frames[32];
    int n = ::backtrace(frames, 32);
    if (n <= 0) {
        MARKWHY_LOG("%s bt: empty", tag);
        return;
    }
    char** syms = ::backtrace_symbols(frames, n);
    MARKWHY_LOG("%s bt: n=%d", tag, n);
    for (int i = 0; i < n; ++i) {
        MARKWHY_LOG("%s bt[%d]: %s", tag, i, syms != nullptr ? syms[i] : "?");
    }
    if (syms != nullptr) {
        std::free(syms);
    }
}

// Dump object header + TypeInfo raw layout that feeds GetSize (instanceSize / array path).
void DumpObjIdentity(const BaseObject* obj, size_t reportedObjSize, uintptr_t regionStart, uintptr_t regionEnd)
{
    if (obj == nullptr) {
        return;
    }
    auto objAddr = reinterpret_cast<uintptr_t>(obj);
    DumpMapsForAddr("OBJ", objAddr, regionStart, regionEnd);
    DumpHexAround("OBJ", obj, 64, 64);

    // Raw 16B header (StateWord)
    uint64_t w0 = 0;
    uint64_t w1 = 0;
    std::memcpy(&w0, reinterpret_cast<const void*>(objAddr), sizeof(w0));
    if (objAddr + 16 <= regionEnd || objAddr + 16 > objAddr) {
        std::memcpy(&w1, reinterpret_cast<const void*>(objAddr + 8), sizeof(w1));
    }
    MARKWHY_LOG("OBJ hdr: w0=%#llx w1=%#llx", static_cast<unsigned long long>(w0),
                static_cast<unsigned long long>(w1));

    TypeInfo* ti = nullptr;
    // Prefer public accessor; may fault if header garbage — maps already said heap region so usually ok.
    ti = const_cast<BaseObject*>(obj)->GetTypeInfo();
    auto tiAddr = reinterpret_cast<uintptr_t>(ti);
    MARKWHY_LOG("OBJ typeinfo_ptr=%p reportedObjSize=%zu magic=0x478b4930_match=%u", static_cast<void*>(ti),
                reportedObjSize, reportedObjSize == 1200310576u ? 1u : 0u);
    DumpMapsForAddr("TI", tiAddr, 0, 0);
    if (ti != nullptr) {
        DumpHexAround("TI", ti, 64, 64);
        // Layout (MClass.h): name@0, type@8, flag@9, fieldNum@10, instanceSize@12 (U32), ...
        uint64_t nameQ = 0;
        uint32_t instSz = 0;
        int8_t ty = 0;
        uint8_t fl = 0;
        uint16_t fnum = 0;
        std::memcpy(&nameQ, reinterpret_cast<const void*>(tiAddr), sizeof(nameQ));
        std::memcpy(&ty, reinterpret_cast<const void*>(tiAddr + 8), 1);
        std::memcpy(&fl, reinterpret_cast<const void*>(tiAddr + 9), 1);
        std::memcpy(&fnum, reinterpret_cast<const void*>(tiAddr + 10), 2);
        std::memcpy(&instSz, reinterpret_cast<const void*>(tiAddr + 12), 4);
        MARKWHY_LOG("TI raw: nameQ=%#llx type=%d flag=%u fieldNum=%u instanceSize_u32=%u (0x%x) "
                    "instanceSize_as_i32=%d",
                    static_cast<unsigned long long>(nameQ), static_cast<int>(ty), static_cast<unsigned>(fl),
                    static_cast<unsigned>(fnum), instSz, instSz, static_cast<int>(instSz));
        // Try name string if maps says readable
        uintptr_t nLo = 0;
        uintptr_t nHi = 0;
        uintptr_t nOff = 0;
        char nPerms[8] = {};
        char nPath[400] = {};
        if (FindMapsEntry(static_cast<uintptr_t>(nameQ), &nLo, &nHi, &nOff, nPerms, nPath, sizeof(nPath)) &&
            nPerms[0] == 'r') {
            char nameBuf[96] = {};
            const char* np = reinterpret_cast<const char*>(static_cast<uintptr_t>(nameQ));
            std::strncpy(nameBuf, np, sizeof(nameBuf) - 1);
            MARKWHY_LOG("TI name_str=%.95s name_maps=%s", nameBuf, nPath[0] ? nPath : "(anon)");
        } else {
            MARKWHY_LOG("TI name_str: unreadable nameQ=%#llx", static_cast<unsigned long long>(nameQ));
        }
        // Public getters (may diverge if TI corrupt mid-read)
        MARKWHY_LOG("TI api: IsArray=%u IsObject=%u IsVaild=%u GetInstanceSize=%u GetName=%s",
                    ti->IsArrayType() ? 1u : 0u, ti->IsObjectType() ? 1u : 0u, ti->IsVaildType() ? 1u : 0u,
                    static_cast<unsigned>(ti->GetInstanceSize()), ti->GetName() != nullptr ? ti->GetName() : "(null)");
    }
    DumpBacktrace("CALLER");
}

std::atomic<uint64_t> gN{0};
std::atomic<uint64_t> gOk{0};
std::atomic<uint64_t> gFail{0};
std::atomic<uint64_t> gBmMismatch{0};     // writeBm != readBm
std::atomic<uint64_t> gLiMismatch{0};     // liveInfo changed vs write path (bindedRegion / pointer)
std::atomic<uint64_t> gOffsetOob{0};      // offset beyond bitmap capacity
std::atomic<uint64_t> gNullReadBm{0};
std::atomic<uint64_t> gZeroObjSize{0};
std::atomic<uint64_t> gSmallObjSize{0}; // 0 < size < 8
std::atomic<uint64_t> gAllocEvents{0};
std::atomic<uint64_t> gAllocMulti{0}; // same region saw >1 alloc (approx via last-region race)

// Coarse multi-alloc detector: last region that allocated + count in short window.
std::atomic<uintptr_t> gLastAllocRegion{0};
std::atomic<uint64_t> gLastAllocCount{0};

const char* ThreadRole()
{
    if (IsGcThread()) {
        return "gc";
    }
    if (IsRuntimeThread()) {
        return "runtime";
    }
    return "mutator";
}

void DumpSummaryIfNeeded()
{
    uint64_t n = gN.load(std::memory_order_relaxed);
    if (n == 0 || (n & 0xffff) != 0) {
        return;
    }
    MARKWHY_LOG("SUMMARY n=%llu ok=%llu fail=%llu bm_mismatch=%llu li_mismatch=%llu "
                "oob=%llu null_read_bm=%llu zero_sz=%llu small_sz=%llu alloc_n=%llu alloc_multi=%llu",
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(gOk.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gFail.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gBmMismatch.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gLiMismatch.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gOffsetOob.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gNullReadBm.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gZeroObjSize.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gSmallObjSize.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gAllocEvents.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gAllocMulti.load(std::memory_order_relaxed)));
}

} // namespace

bool MarkWhyProbe::Enabled()
{
    static const bool on = false /* pinned:MRT_GCV2_MARK_WHY */;
    return on;
}

bool MarkWhyProbe::AllocTrackEnabled()
{
    static const bool on = false /* pinned:MRT_GCV2_MARK_WHY_ALLOC */;
    return on;
}

void MarkWhyProbe::NoteMarkBitmapAlloc(RegionInfo* region, RegionBitmap* allocated)
{
    if (!AllocTrackEnabled() || region == nullptr) {
        return;
    }
    static std::atomic<bool> armedLogged{false};
    if (!armedLogged.exchange(true, std::memory_order_relaxed)) {
        MARKWHY_LOG("ARMED_ALLOC env=MRT_GCV2_MARK_WHY_ALLOC=1");
    }
    gAllocEvents.fetch_add(1, std::memory_order_relaxed);
    uintptr_t r = reinterpret_cast<uintptr_t>(region);
    uintptr_t prev = gLastAllocRegion.exchange(r, std::memory_order_relaxed);
    if (prev == r) {
        uint64_t c = gLastAllocCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c >= 1) {
            gAllocMulti.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<uint64_t> dumpLeft{32};
            uint64_t left = dumpLeft.load(std::memory_order_relaxed);
            if (left > 0 && dumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
                MARKWHY_LOG("MULTI_ALLOC region=%p bm=%p consecutive=%llu start=%#zx type=%u",
                            static_cast<void*>(region), static_cast<void*>(allocated),
                            static_cast<unsigned long long>(c + 1), region->GetRegionStart(),
                            static_cast<unsigned>(region->GetRegionType()));
            }
        }
    } else {
        gLastAllocCount.store(0, std::memory_order_relaxed);
    }
}

bool MarkWhyProbe::NoteAfterMarkBits(RegionInfo* region, const BaseObject* obj, size_t offsetWrite, size_t objSize,
                                     size_t regionSizeArg, RegionBitmap* writeBm, bool markBitsReturnedAlreadyMarked,
                                     const char* site, Generation generation)
{
    // Both call sites discard the result ((void)... in RegionInfo.h:453,477), and this sits in the
    // marking hot path, so the disabled build must not pay for a bitmap read it will throw away.
    // Same shape as the idlewrite gate fix (7f37316e): the gate goes before the work, not before
    // the printing. If a caller ever needs the read-back value, it should take it from
    // IsMarkedObject directly rather than from a diagnostic that is off by default.
    if (!Enabled() || region == nullptr) {
        return false;
    }
    static std::atomic<bool> armedLogged{false};
    if (!armedLogged.exchange(true, std::memory_order_relaxed)) {
        MARKWHY_LOG("ARMED env=MRT_GCV2_MARK_WHY=1 sample=%zu site=%s", (65536) /* pinned:MRT_GCV2_MARK_WHY_SAMPLE */,
                    site);
    }

    gN.fetch_add(1, std::memory_order_relaxed);

    MAddress regionStart = region->GetRegionStart();
    MAddress regionEnd = region->GetRegionEnd();
    size_t regionSizeMeta = region->GetRegionSize();
    size_t offsetRecompute =
        (obj != nullptr && reinterpret_cast<MAddress>(obj) >= regionStart)
            ? (reinterpret_cast<MAddress>(obj) - regionStart)
            : static_cast<size_t>(-1);
    bool offsetSame = (offsetWrite == offsetRecompute);

    LiveInfo* liveInfo = region->GetLiveInfo();
    RegionBitmap* readBm = generation == Generation::Young
        ? region->GetMarkBitmap(region->GetMarkView<Generation::Young>())
        : region->GetMarkBitmap(region->GetMarkView<Generation::Old>());
    bool bmSame = (writeBm == readBm);
    if (!bmSame) {
        gBmMismatch.fetch_add(1, std::memory_order_relaxed);
    }
    if (readBm == nullptr) {
        gNullReadBm.fetch_add(1, std::memory_order_relaxed);
    }
    if (objSize == 0) {
        gZeroObjSize.fetch_add(1, std::memory_order_relaxed);
    } else if (objSize < kMarkedBytesPerBit) {
        gSmallObjSize.fetch_add(1, std::memory_order_relaxed);
    }

    size_t wordCnt = 0;
    size_t bitCapacity = 0; // bits covering region (one bit per 8 bytes)
    if (writeBm != nullptr) {
        wordCnt = writeBm->wordCnt.load(std::memory_order_acquire);
        bitCapacity = wordCnt * kBitsPerWord;
    }
    // offset is byte offset; bit index = offset / 8
    size_t bitIndex = offsetWrite / kMarkedBytesPerBit;
    bool oob = (writeBm != nullptr) && (bitIndex >= bitCapacity || offsetWrite >= regionSizeMeta);
    if (oob) {
        gOffsetOob.fetch_add(1, std::memory_order_relaxed);
    }

    bool markedNow = generation == Generation::Young
        ? region->IsMarkedObject(region->GetMarkView<Generation::Young>(), offsetWrite)
        : region->IsMarkedObject(region->GetMarkView<Generation::Old>(), offsetWrite);
    // Direct bit read on writeBm (bypass GetMarkBitmap) to detect identity skew.
    bool markedOnWriteBm = false;
    if (writeBm != nullptr && bitIndex < bitCapacity) {
        markedOnWriteBm = writeBm->IsMarked(offsetWrite);
    }
    bool markedOnReadBm = false;
    if (readBm != nullptr) {
        markedOnReadBm = readBm->IsMarked(offsetWrite);
    }

    if (liveInfo != nullptr && liveInfo->bindedRegion != region) {
        gLiMismatch.fetch_add(1, std::memory_order_relaxed);
    }

    GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase();
    uint16_t prevTag = ForwardDataManager::GetForwardDataManager().GetPreviousTagID();
    uint16_t tagId = static_cast<uint16_t>(prevTag ^ 1u); // currentTagID = previous ^ 1

    if (markedNow) {
        gOk.fetch_add(1, std::memory_order_relaxed);
    } else {
        gFail.fetch_add(1, std::memory_order_relaxed);
    }

    bool interesting = !markedNow || !bmSame || oob || objSize < kMarkedBytesPerBit || !offsetSame ||
                       (liveInfo != nullptr && liveInfo->bindedRegion != region) ||
                       (markedOnWriteBm != markedOnReadBm);

    static std::atomic<uint64_t> failDumpLeft{128};
    static std::atomic<uint64_t> sampleDumpLeft{32};
    size_t sampleEvery = (65536) /* pinned:MRT_GCV2_MARK_WHY_SAMPLE */;
    uint64_t n = gN.load(std::memory_order_relaxed);
    bool sampleOk = markedNow && sampleEvery > 0 && (n % sampleEvery) == 0;

    if (interesting) {
        uint64_t left = failDumpLeft.load(std::memory_order_relaxed);
        if (left > 0 && failDumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
            MARKWHY_LOG(
                "FAIL site=%s obj=%p region=%p rStart=%#zx rEnd=%#zx rSize=%zu type=%u young=%u "
                "offsetW=%zu offsetR=%zu offSame=%u objSize=%zu regionSizeArg=%zu "
                "writeBm=%p readBm=%p bmSame=%u wordCnt=%zu bitCap=%zu bitIdx=%zu oob=%u "
                "markedNow=%u markedWriteBm=%u markedReadBm=%u markBitsWasAlready=%u "
                "liveInfo=%p binded=%p liOk=%u phase=%u tagCurGuess=%u prevTag=%u role=%s",
                site, static_cast<const void*>(obj), static_cast<void*>(region), static_cast<uintptr_t>(regionStart),
                static_cast<uintptr_t>(regionEnd), regionSizeMeta, static_cast<unsigned>(region->GetRegionType()),
                region->IsYoungRegion() ? 1u : 0u, offsetWrite, offsetRecompute, offsetSame ? 1u : 0u, objSize,
                regionSizeArg, static_cast<void*>(writeBm), static_cast<void*>(readBm), bmSame ? 1u : 0u, wordCnt,
                bitCapacity, bitIndex, oob ? 1u : 0u, markedNow ? 1u : 0u, markedOnWriteBm ? 1u : 0u,
                markedOnReadBm ? 1u : 0u, markBitsReturnedAlreadyMarked ? 1u : 0u, static_cast<void*>(liveInfo),
                liveInfo != nullptr ? static_cast<void*>(liveInfo->bindedRegion) : nullptr,
                (liveInfo == nullptr || liveInfo->bindedRegion == region) ? 1u : 0u, static_cast<unsigned>(phase),
                static_cast<unsigned>(tagId), static_cast<unsigned>(prevTag), ThreadRole());
            // T1 forensic extension (magicaddr): maps + hex + TypeInfo raw + backtrace
            DumpObjIdentity(obj, objSize, static_cast<uintptr_t>(regionStart), static_cast<uintptr_t>(regionEnd));
        }
    } else if (sampleOk) {
        uint64_t left = sampleDumpLeft.load(std::memory_order_relaxed);
        if (left > 0 && sampleDumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
            MARKWHY_LOG(
                "OK_SAMPLE site=%s obj=%p region=%p rStart=%#zx offsetW=%zu objSize=%zu writeBm=%p readBm=%p "
                "bmSame=%u markedNow=%u phase=%u role=%s",
                site, static_cast<const void*>(obj), static_cast<void*>(region), static_cast<uintptr_t>(regionStart),
                offsetWrite, objSize, static_cast<void*>(writeBm), static_cast<void*>(readBm), bmSame ? 1u : 0u,
                markedNow ? 1u : 0u, static_cast<unsigned>(phase), ThreadRole());
        }
    }

    DumpSummaryIfNeeded();
    return markedNow;
}

} // namespace MapleRuntime
