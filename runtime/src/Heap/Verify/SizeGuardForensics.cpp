// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SizeGuardForensics.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>

#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/B2RingProbe.h"
#include "ObjectModel/MClass.h"

namespace MapleRuntime {
namespace {

#define SGF_LOG(fmt, ...)                                                                                              \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][sizeguard-forensics] " fmt "\n", ##__VA_ARGS__);                                   \
        std::fflush(stderr);                                                                                           \
    } while (0)

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

const char* ClassifyMapsKind(const char* perms, const char* path, uintptr_t addr, uintptr_t regionStart,
                             uintptr_t regionEnd)
{
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
        return "ANON_NAMED";
    }
    if (path == nullptr || path[0] == '\0') {
        if (r && w && !x) {
            return "ANON_RW";
        }
        if (r && !w && x) {
            return "ANON_RX";
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

bool FindMapsEntry(uintptr_t addr, uintptr_t* loOut, uintptr_t* hiOut, uintptr_t* offOut, char* permsOut, char* pathOut,
                   size_t pathCap)
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
        char path[400] = {};
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

void DumpMapsForAddr(const char* tag, uintptr_t addr, uintptr_t regionStart, uintptr_t regionEnd)
{
    uintptr_t lo = 0;
    uintptr_t hi = 0;
    uintptr_t off = 0;
    char perms[8] = {};
    char path[400] = {};
    if (!FindMapsEntry(addr, &lo, &hi, &off, perms, path, sizeof(path))) {
        SGF_LOG("%s maps: UNMAPPED addr=%#zx", tag, addr);
        return;
    }
    const char* kind = ClassifyMapsKind(perms, path, addr, regionStart, regionEnd);
    uintptr_t fileOff = off + (addr - lo);
    SGF_LOG("%s maps: kind=%s addr=%#zx range=%#zx-%#zx perms=%s fileOff=%#zx path=%s", tag, kind, addr, lo, hi, perms,
            fileOff, path[0] ? path : "(anon)");
    unsigned bit48 = static_cast<unsigned>((addr >> 48) & 1u);
    SGF_LOG("%s ptrbits: bit48=%u high16=%#x low48=%#zx", tag, bit48, static_cast<unsigned>((addr >> 48) & 0xffffu),
            addr & ((UINT64_C(1) << 48) - 1));
}

void DumpHexAround(const char* tag, const void* center, size_t before, size_t after)
{
    if (center == nullptr) {
        return;
    }
    auto c = reinterpret_cast<uintptr_t>(center);
    uintptr_t mapLo = 0;
    uintptr_t mapHi = 0;
    uintptr_t mapOff = 0;
    char perms[8] = {};
    char path[400] = {};
    if (!FindMapsEntry(c, &mapLo, &mapHi, &mapOff, perms, path, sizeof(path))) {
        SGF_LOG("%s hex: UNMAPPED center=%p", tag, center);
        return;
    }
    uintptr_t lo = (c >= before) ? (c - before) : mapLo;
    uintptr_t hi = c + after;
    if (lo < mapLo) {
        lo = mapLo;
    }
    if (hi > mapHi) {
        hi = mapHi;
    }
    if (lo >= hi) {
        return;
    }
    size_t len = static_cast<size_t>(hi - lo);
    if (len > 160) {
        lo = (c > 80) ? (c - 80) : mapLo;
        hi = c + 80;
        if (lo < mapLo) {
            lo = mapLo;
        }
        if (hi > mapHi) {
            hi = mapHi;
        }
        len = static_cast<size_t>(hi - lo);
    }
    SGF_LOG("%s hex lo=%#zx hi=%#zx len=%zu center=%p", tag, lo, hi, len, center);
    char hexLine[3 * 16 + 8];
    const auto* bytes = reinterpret_cast<const unsigned char*>(lo);
    for (size_t i = 0; i < len; i += 16) {
        size_t chunk = (len - i < 16) ? (len - i) : 16;
        size_t pos = 0;
        for (size_t j = 0; j < chunk && pos + 3 < sizeof(hexLine); ++j) {
            pos += static_cast<size_t>(std::snprintf(hexLine + pos, sizeof(hexLine) - pos, "%02x ", bytes[i + j]));
        }
        SGF_LOG("%s  %#zx: %s", tag, lo + i, hexLine);
    }
}

void DumpDladdr(const char* tag, uintptr_t addr)
{
    Dl_info info {};
    if (addr == 0 || dladdr(reinterpret_cast<void*>(addr), &info) == 0) {
        SGF_LOG("%s dladdr: fail addr=%#zx", tag, addr);
        return;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    uintptr_t saddr = reinterpret_cast<uintptr_t>(info.dli_saddr);
    SGF_LOG("%s dladdr: fname=%s fbase=%#zx sname=%s saddr=%#zx offset_from_s=%#zx offset_from_f=%#zx", tag,
            info.dli_fname != nullptr ? info.dli_fname : "(null)", base, info.dli_sname != nullptr ? info.dli_sname : "(null)",
            saddr, saddr != 0 ? (addr - saddr) : 0, base != 0 ? (addr - base) : 0);
}

void DumpBacktrace()
{
    void* frames[40];
    int n = ::backtrace(frames, 40);
    if (n <= 0) {
        SGF_LOG("bt: empty");
        return;
    }
    char** syms = ::backtrace_symbols(frames, n);
    SGF_LOG("bt: n=%d", n);
    for (int i = 0; i < n; ++i) {
        SGF_LOG("bt[%d]: %s", i, syms != nullptr ? syms[i] : "?");
    }
    if (syms != nullptr) {
        std::free(syms);
    }
}

// Reconstruct non-array GetSize from raw instanceSize@TI+12 (U32).
size_t ReconGetSizeFromInst(uint32_t instSz)
{
    size_t recon = static_cast<size_t>(instSz) + 8u;
    recon = (recon + 7u) & ~static_cast<size_t>(7u);
    return recon;
}

void ProbeAsObjectHeaderFixed(const char* tag, uintptr_t cand, uintptr_t regionStart, uintptr_t regionEnd)
{
    if (cand == 0) {
        return;
    }
    uintptr_t mapLo = 0;
    uintptr_t mapHi = 0;
    uintptr_t mapOff = 0;
    char perms[8] = {};
    char path[400] = {};
    if (!FindMapsEntry(cand, &mapLo, &mapHi, &mapOff, perms, path, sizeof(path))) {
        SGF_LOG("%s cand=%#zx UNMAPPED", tag, cand);
        return;
    }
    if (perms[0] != 'r') {
        SGF_LOG("%s cand=%#zx not-readable perms=%s", tag, cand, perms);
        return;
    }
    uint64_t w0 = 0;
    uint64_t w1 = 0;
    std::memcpy(&w0, reinterpret_cast<const void*>(cand), sizeof(w0));
    if (cand + 16 <= mapHi) {
        std::memcpy(&w1, reinterpret_cast<const void*>(cand + 8), sizeof(w1));
    }
    SGF_LOG("%s cand=%#zx hdr w0=%#llx w1=%#llx in_region=%u", tag, cand, static_cast<unsigned long long>(w0),
            static_cast<unsigned long long>(w1),
            (regionStart != 0 && cand >= regionStart && cand < regionEnd) ? 1u : 0u);

    uintptr_t tip48 = static_cast<uintptr_t>(w0) & ((UINT64_C(1) << 48) - 1);
    char tipTag[48];
    std::snprintf(tipTag, sizeof(tipTag), "%s.tip", tag);
    DumpMapsForAddr(tipTag, tip48, 0, 0);
    DumpDladdr(tipTag, tip48);

    if (tip48 != 0) {
        uintptr_t tLo = 0;
        uintptr_t tHi = 0;
        uintptr_t tOff = 0;
        char tPerms[8] = {};
        char tPath[400] = {};
        if (FindMapsEntry(tip48, &tLo, &tHi, &tOff, tPerms, tPath, sizeof(tPath)) && tPerms[0] == 'r' &&
            tip48 + 16 <= tHi) {
            uint32_t instSz = 0;
            int8_t ty = 0;
            uint8_t fl = 0;
            uint16_t fnum = 0;
            std::memcpy(&ty, reinterpret_cast<const void*>(tip48 + 8), 1);
            std::memcpy(&fl, reinterpret_cast<const void*>(tip48 + 9), 1);
            std::memcpy(&fnum, reinterpret_cast<const void*>(tip48 + 10), 2);
            std::memcpy(&instSz, reinterpret_cast<const void*>(tip48 + 12), 4);
            size_t recon = ReconGetSizeFromInst(instSz);
            SGF_LOG("%s.tip raw type=%d flag=%u fieldNum=%u instSz_u32=%u (0x%x) recon_GetSize=%zu magic1200=%u "
                    "magic2p32=%u",
                    tag, static_cast<int>(ty), static_cast<unsigned>(fl), static_cast<unsigned>(fnum), instSz, instSz,
                    recon, recon == 1200310576u ? 1u : 0u, recon == 4294967304ull ? 1u : 0u);
        }
    }
}

} // namespace

bool SizeGuardForensics::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_SIZEGUARD_FORENSICS");
    return on;
}

void SizeGuardForensics::DumpBeforeAbort(const BaseObject* obj, size_t objSize, const RegionInfo* region,
                                         uintptr_t regionStart, uintptr_t regionEnd)
{
    auto objAddr = reinterpret_cast<uintptr_t>(obj);
    // Ring dump is independent of sizeguard-forensics env (own gate MRT_GCV2_B2RING).
    B2RingProbe::DumpAllRings("sizeguard", objAddr);
    if (!Enabled()) {
        return;
    }
    SGF_LOG("BEGIN obj=%p objSize=%zu region=%p regionStart=%#zx regionEnd=%#zx", static_cast<const void*>(obj),
            objSize, static_cast<const void*>(region), regionStart, regionEnd);
    SGF_LOG("magic_match: size1200=%u size2p32p8=%u", objSize == 1200310576u ? 1u : 0u,
            objSize == 4294967304ull ? 1u : 0u);

    DumpMapsForAddr("OBJ", objAddr, regionStart, regionEnd);
    DumpHexAround("OBJ", obj, 80, 80);

    // Current header interpretation (as MarkObject/GetSize would).
    ProbeAsObjectHeaderFixed("OBJ_AS_HDR", objAddr, regionStart, regionEnd);

    // Interior hypothesis: reported obj is true_obj + delta.
    // Closure layout (natural_wave disasm): TI@+0, $g@+8, $i@+0x10.
    // If GC walks a field that holds interior = closure+$i_slot, then:
    //   obj points at $i code ptr → GetTypeInfo = code → magic size.
    static const int kDeltas[] = { -8, -16, -24, -32, -40, -48, -0x10, -0x18, -0x20, -0x28, -0x30 };
    for (int d : kDeltas) {
        if (static_cast<intptr_t>(objAddr) + d < 0) {
            continue;
        }
        uintptr_t cand = objAddr + static_cast<uintptr_t>(static_cast<intptr_t>(d));
        char tag[32];
        std::snprintf(tag, sizeof(tag), "INTERIOR_d%+d", d);
        ProbeAsObjectHeaderFixed(tag, cand, regionStart, regionEnd);
    }

    // Also classify reported typeinfo slot directly via GetTypeInfo path.
    TypeInfo* ti = nullptr;
    ti = const_cast<BaseObject*>(obj)->GetTypeInfo();
    auto tiAddr = reinterpret_cast<uintptr_t>(ti);
    SGF_LOG("OBJ GetTypeInfo()=%p", static_cast<void*>(ti));
    DumpMapsForAddr("TI", tiAddr, 0, 0);
    DumpDladdr("TI", tiAddr);
    if (ti != nullptr) {
        DumpHexAround("TI", ti, 32, 48);
        uint32_t instSz = 0;
        int8_t ty = 0;
        uint8_t fl = 0;
        uint16_t fnum = 0;
        uintptr_t tLo = 0;
        uintptr_t tHi = 0;
        uintptr_t tOff = 0;
        char tPerms[8] = {};
        char tPath[400] = {};
        if (FindMapsEntry(tiAddr, &tLo, &tHi, &tOff, tPerms, tPath, sizeof(tPath)) && tPerms[0] == 'r' &&
            tiAddr + 16 <= tHi) {
            std::memcpy(&ty, reinterpret_cast<const void*>(tiAddr + 8), 1);
            std::memcpy(&fl, reinterpret_cast<const void*>(tiAddr + 9), 1);
            std::memcpy(&fnum, reinterpret_cast<const void*>(tiAddr + 10), 2);
            std::memcpy(&instSz, reinterpret_cast<const void*>(tiAddr + 12), 4);
            size_t recon = ReconGetSizeFromInst(instSz);
            SGF_LOG("TI raw type=%d flag=%u fieldNum=%u instSz_u32=%u (0x%x) recon=%zu reported=%zu match=%u",
                    static_cast<int>(ty), static_cast<unsigned>(fl), static_cast<unsigned>(fnum), instSz, instSz, recon,
                    objSize, recon == objSize ? 1u : 0u);
        }
    }

    DumpBacktrace();
    SGF_LOG("END");
}

} // namespace MapleRuntime
