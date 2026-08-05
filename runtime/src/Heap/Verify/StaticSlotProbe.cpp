// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "StaticSlotProbe.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {

#define SSLOT_LOG(fmt, ...)                                                                                            \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][static-slot] " fmt "\n", ##__VA_ARGS__);                                           \
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

bool PageMapped(uintptr_t addr)
{
    if (addr == 0) {
        return false;
    }
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        pageSize = 4096;
    }
    uintptr_t page = addr & ~(static_cast<uintptr_t>(pageSize) - 1);
    unsigned char vec = 0;
    if (mincore(reinterpret_cast<void*>(page), static_cast<size_t>(pageSize), &vec) == 0) {
        return true;
    }
    return false;
}

TypeInfo* PeekTypeInfoAt(uintptr_t addr)
{
    if (addr == 0 || (addr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return nullptr;
    }
    if (!Heap::IsHeapAddress(addr)) {
        return nullptr;
    }
#ifdef __arm__
    uint32_t raw = 0;
    std::memcpy(&raw, reinterpret_cast<const void*>(addr), sizeof(raw));
    return reinterpret_cast<TypeInfo*>(static_cast<uintptr_t>(raw));
#else
    uint32_t low = 0;
    uint16_t high = 0;
    std::memcpy(&low, reinterpret_cast<const void*>(addr), sizeof(low));
    std::memcpy(&high, reinterpret_cast<const void*>(addr + 4), sizeof(high));
    uintptr_t tipAddr = (static_cast<uintptr_t>(high) << 32) | static_cast<uintptr_t>(low);
    return reinterpret_cast<TypeInfo*>(tipAddr);
#endif
}

bool TipLooksValid(TypeInfo* tip)
{
    if (tip == nullptr) {
        return false;
    }
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return false;
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    bool inTim = TypeInfoManager::GetTypeInfoManager().ContainsAddress(tipAddr);
    if (!inTim && !PageMapped(tipAddr)) {
        return false;
    }
    if (!tip->IsVaildType()) {
        return false;
    }
    MSize isz = tip->GetInstanceSize();
    if (isz == 0 || isz > (1u << 20)) {
        return false;
    }
    return true;
}

size_t SaneObjectSize(TypeInfo* tip, RegionInfo* region)
{
    if (tip == nullptr || region == nullptr) {
        return 0;
    }
    MSize isz = tip->GetInstanceSize();
    size_t size = (static_cast<size_t>(isz) + 8u + 7u) & ~static_cast<size_t>(7u);
    size_t regionBytes = region->GetRegionEnd() - region->GetRegionStart();
    if (size < 16 || size > regionBytes || size > (1u << 20)) {
        return 0;
    }
    return size;
}

enum class Kind : uint8_t { Base = 0, Interior = 1, Unknown = 2, NotHeap = 3, Null = 4 };

const char* KindName(Kind k)
{
    switch (k) {
        case Kind::Base:
            return "base";
        case Kind::Interior:
            return "interior";
        case Kind::Unknown:
            return "unknown";
        case Kind::NotHeap:
            return "not_heap";
        case Kind::Null:
            return "null";
        default:
            return "?";
    }
}

// interiorfix rule: legal tip at value ⇒ always Base (never adjacent-object interior FP).
// Only when value is NOT a legal base, classify interior at value-k.
void Classify(uintptr_t value, Kind& kind, uintptr_t& baseOut, size_t& offsetOut, TypeInfo*& tipAtValue,
              TypeInfo*& tipAtBase)
{
    kind = Kind::Unknown;
    baseOut = 0;
    offsetOut = 0;
    tipAtValue = nullptr;
    tipAtBase = nullptr;

    if (value == 0) {
        kind = Kind::Null;
        return;
    }
    if (!Heap::IsHeapAddress(value)) {
        kind = Kind::NotHeap;
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(value);
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        kind = Kind::Unknown;
        return;
    }

    tipAtValue = PeekTypeInfoAt(value);
    bool valueBaseLike = TipLooksValid(tipAtValue);

    if (valueBaseLike) {
        kind = Kind::Base;
        baseOut = value;
        offsetOut = 0;
        tipAtBase = tipAtValue;
        return;
    }

    static const size_t kOffs[] = {8, 16, 24, 32};
    for (size_t off : kOffs) {
        if (value < off) {
            continue;
        }
        uintptr_t cand = value - off;
        if (!Heap::IsHeapAddress(cand)) {
            continue;
        }
        RegionInfo* candRegion = RegionInfo::TryGetRegionInfoAt(cand);
        if (candRegion != region) {
            continue;
        }
        TypeInfo* tip = PeekTypeInfoAt(cand);
        if (!TipLooksValid(tip)) {
            continue;
        }
        size_t size = SaneObjectSize(tip, region);
        bool sizeOk = false;
        if (size != 0 && value >= cand && value < cand + size && cand + size <= region->GetRegionEnd()) {
            sizeOk = true;
        }
        if (!sizeOk && size == 0) {
            if (off == 8 || off == 16 || off == 24) {
                sizeOk = true;
            }
        }
        if (!sizeOk) {
            continue;
        }
        kind = Kind::Interior;
        baseOut = cand;
        offsetOut = off;
        tipAtBase = tip;
        return;
    }
    kind = Kind::Unknown;
}

struct MapEntry {
    uintptr_t start = 0;
    uintptr_t end = 0;
    char perms[8] = {};
    uintptr_t offset = 0;
    char path[512] = {};
};

bool LookupMaps(uintptr_t addr, MapEntry& out)
{
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) {
        return false;
    }
    char line[768];
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        uintptr_t s = 0;
        uintptr_t e = 0;
        uintptr_t off = 0;
        char perms[8] = {};
        char path[512] = {};
        // address-range perms offset dev inode pathname
        int n = std::sscanf(line, "%lx-%lx %7s %lx %*s %*s %511[^\n]", &s, &e, perms, &off, path);
        if (n < 4) {
            continue;
        }
        if (addr >= s && addr < e) {
            out.start = s;
            out.end = e;
            std::strncpy(out.perms, perms, sizeof(out.perms) - 1);
            out.offset = off;
            if (n >= 5) {
                // trim leading spaces in path
                const char* p = path;
                while (*p == ' ' || *p == '\t') {
                    ++p;
                }
                std::strncpy(out.path, p, sizeof(out.path) - 1);
            } else {
                out.path[0] = '\0';
            }
            found = true;
            break;
        }
    }
    std::fclose(f);
    return found;
}

const char* MapsKind(const MapEntry& m)
{
    if (m.path[0] == '\0') {
        return "ANON";
    }
    if (m.path[0] == '[') {
        return "PSEUDO";
    }
    bool r = m.perms[0] == 'r';
    bool w = m.perms[1] == 'w';
    bool x = m.perms[2] == 'x';
    if (r && x && !w) {
        return "FILE_RX";
    }
    if (r && w && !x) {
        return "FILE_RW";
    }
    if (r && !w && !x) {
        return "FILE_R";
    }
    return "FILE";
}

struct SymHit {
    char name[256] = {};
    uintptr_t symVma = 0;
    uintptr_t runtimeBase = 0;
    size_t size = 0;
    bool ok = false;
};

struct PhdrBiasCtx {
    uintptr_t addr = 0;
    const char* path = nullptr;
    uintptr_t bias = 0;
    bool found = false;
};

int PhdrBiasCb(struct dl_phdr_info* info, size_t, void* data)
{
    auto* ctx = static_cast<PhdrBiasCtx*>(data);
    if (info == nullptr) {
        return 0;
    }
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        uintptr_t start = info->dlpi_addr + ph.p_vaddr;
        uintptr_t end = start + ph.p_memsz;
        if (ctx->addr >= start && ctx->addr < end) {
            ctx->bias = info->dlpi_addr;
            ctx->found = true;
            if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
                ctx->path = info->dlpi_name;
            }
            return 1;
        }
    }
    return 0;
}

bool ResolveSymbolInElf(const char* path, uintptr_t runtimeAddr, uintptr_t /*mapStart*/, uintptr_t /*mapFileOff*/,
                        SymHit& hit)
{
    hit = SymHit{};
    if (path == nullptr || path[0] == '\0' || path[0] == '[') {
        return false;
    }
    PhdrBiasCtx bctx {};
    bctx.addr = runtimeAddr;
    dl_iterate_phdr(PhdrBiasCb, &bctx);
    uintptr_t loadBias = bctx.found ? bctx.bias : 0;

    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return false;
    }
    size_t fileSize = static_cast<size_t>(st.st_size);
    void* mapped = ::mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
        return false;
    }
    auto* base = reinterpret_cast<const uint8_t*>(mapped);
    if (fileSize < sizeof(Elf64_Ehdr)) {
        ::munmap(mapped, fileSize);
        return false;
    }
    auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        ::munmap(mapped, fileSize);
        return false;
    }

    auto scanSymtab = [&](const Elf64_Shdr* sh, const char* strtab, size_t strsz) {
        if (sh == nullptr || strtab == nullptr || sh->sh_entsize == 0) {
            return;
        }
        size_t n = sh->sh_size / sh->sh_entsize;
        const Elf64_Sym* syms = reinterpret_cast<const Elf64_Sym*>(base + sh->sh_offset);
        for (size_t i = 0; i < n; ++i) {
            const Elf64_Sym& s = syms[i];
            if (s.st_name == 0 || s.st_shndx == SHN_UNDEF) {
                continue;
            }
            unsigned char t = ELF64_ST_TYPE(s.st_info);
            if (t != STT_OBJECT && t != STT_NOTYPE && t != STT_FUNC && t != STT_COMMON) {
                continue;
            }
            if (s.st_value == 0) {
                continue;
            }
            uintptr_t rt = s.st_value + loadBias;
            if (rt > runtimeAddr) {
                continue;
            }
            // Prefer highest address <= runtimeAddr; if size known, prefer covering range.
            bool better = false;
            if (!hit.ok) {
                better = true;
            } else if (rt > hit.runtimeBase) {
                better = true;
            }
            if (s.st_size != 0 && runtimeAddr < rt + s.st_size) {
                if (!hit.ok || hit.size == 0 || rt >= hit.runtimeBase) {
                    better = true;
                }
            }
            if (!better) {
                continue;
            }
            size_t nameOff = s.st_name;
            if (nameOff >= strsz) {
                continue;
            }
            std::strncpy(hit.name, strtab + nameOff, sizeof(hit.name) - 1);
            hit.symVma = s.st_value;
            hit.runtimeBase = rt;
            hit.size = static_cast<size_t>(s.st_size);
            hit.ok = true;
        }
    };

    const Elf64_Shdr* shdr = reinterpret_cast<const Elf64_Shdr*>(base + ehdr->e_shoff);
    for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
        if (shdr[i].sh_type != SHT_SYMTAB && shdr[i].sh_type != SHT_DYNSYM) {
            continue;
        }
        if (shdr[i].sh_link >= ehdr->e_shnum) {
            continue;
        }
        const Elf64_Shdr& strSh = shdr[shdr[i].sh_link];
        const char* strtab = reinterpret_cast<const char*>(base + strSh.sh_offset);
        scanSymtab(&shdr[i], strtab, static_cast<size_t>(strSh.sh_size));
    }

    ::munmap(mapped, fileSize);
    return hit.ok;
}

std::atomic<uint64_t> gTotal{0};
std::atomic<uint64_t> gBase{0};
std::atomic<uint64_t> gInterior{0};
std::atomic<uint64_t> gUnknown{0};
std::atomic<uint64_t> gNotHeap{0};
std::atomic<uint64_t> gNull{0};
std::atomic<uint64_t> gInteriorOff16{0};
std::atomic<uint64_t> gInteriorOffOther{0};
std::atomic<uint64_t> gDumpLeft{0};
std::atomic<uint64_t> gBaseDumped{0};
std::atomic<bool> gArmedLogged{false};
std::mutex gDedupMu;
// Dedup interior slot addresses so we dump each unique slot once (plus samples).
constexpr size_t kDedupCap = 256;
uintptr_t gSeenSlots[kDedupCap];
size_t gSeenN = 0;

bool SeenSlot(uintptr_t slot)
{
    std::lock_guard<std::mutex> lock(gDedupMu);
    for (size_t i = 0; i < gSeenN; ++i) {
        if (gSeenSlots[i] == slot) {
            return true;
        }
    }
    if (gSeenN < kDedupCap) {
        gSeenSlots[gSeenN++] = slot;
    }
    return false;
}

} // namespace

bool StaticSlotProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_STATIC_SLOT");
    return on;
}

void StaticSlotProbe::NoteStaticField(RefField<>& field)
{
    if (!Enabled()) {
        return;
    }
    if (!gArmedLogged.exchange(true, std::memory_order_relaxed)) {
        size_t dumpMax = EnvSizeT("MRT_GCV2_STATIC_SLOT_DUMP_MAX", 96);
        gDumpLeft.store(dumpMax, std::memory_order_relaxed);
        SSLOT_LOG("ARMED env=MRT_GCV2_STATIC_SLOT=1 dumpMax=%zu", dumpMax);
    }

    uintptr_t slot = reinterpret_cast<uintptr_t>(&field);
    // Untag via RefField layout (address:48); no barrier write side effects.
    uintptr_t value = field.GetAddress();

    Kind kind = Kind::Unknown;
    uintptr_t base = 0;
    size_t offset = 0;
    TypeInfo* tipVal = nullptr;
    TypeInfo* tipBase = nullptr;
    Classify(value, kind, base, offset, tipVal, tipBase);

    gTotal.fetch_add(1, std::memory_order_relaxed);
    switch (kind) {
        case Kind::Base:
            gBase.fetch_add(1, std::memory_order_relaxed);
            break;
        case Kind::Interior:
            gInterior.fetch_add(1, std::memory_order_relaxed);
            if (offset == 16) {
                gInteriorOff16.fetch_add(1, std::memory_order_relaxed);
            } else {
                gInteriorOffOther.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case Kind::NotHeap:
            gNotHeap.fetch_add(1, std::memory_order_relaxed);
            break;
        case Kind::Null:
            gNull.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            gUnknown.fetch_add(1, std::memory_order_relaxed);
            break;
    }

    bool dump = false;
    if (kind == Kind::Interior) {
        // Always dump first sighting of each interior slot.
        dump = !SeenSlot(slot);
    } else if (kind == Kind::Base) {
        uint64_t n = gBaseDumped.fetch_add(1, std::memory_order_relaxed);
        dump = n < 6; // positive control: first few normal base slots
    }

    if (!dump) {
        return;
    }
    uint64_t left = gDumpLeft.load(std::memory_order_relaxed);
    if (left == 0) {
        return;
    }
    if (!gDumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
        return;
    }

    MapEntry me {};
    bool hasMap = LookupMaps(slot, me);
    uintptr_t fileOff = 0;
    if (hasMap) {
        fileOff = (slot - me.start) + me.offset;
    }

    SymHit sym {};
    bool hasSym = false;
    if (hasMap && me.path[0] != '\0' && me.path[0] != '[') {
        hasSym = ResolveSymbolInElf(me.path, slot, me.start, me.offset, sym);
    }

    Dl_info dli {};
    bool hasDl = dladdr(reinterpret_cast<void*>(slot), &dli) != 0;

    GCPhase phase = Heap::GetHeap().GetGCPhase();
    const char* phaseName = Collector::GetGCPhaseName(phase);

    SSLOT_LOG("SLOT kind=%s slot=%#zx value=%#zx base=%#zx offset=%zu tipVal=%p tipBase=%p phase=%s(%u)",
              KindName(kind), static_cast<size_t>(slot), static_cast<size_t>(value), static_cast<size_t>(base), offset,
              static_cast<void*>(tipVal), static_cast<void*>(tipBase), phaseName, static_cast<unsigned>(phase));
    if (hasMap) {
        SSLOT_LOG("  MAP kind=%s range=%#zx-%#zx perms=%s mapOff=%#zx fileOff=%#zx path=%s", MapsKind(me),
                  static_cast<size_t>(me.start), static_cast<size_t>(me.end), me.perms, static_cast<size_t>(me.offset),
                  static_cast<size_t>(fileOff), me.path[0] ? me.path : "(none)");
    } else {
        SSLOT_LOG("  MAP kind=UNMAPPED slot=%#zx", static_cast<size_t>(slot));
    }
    if (hasSym) {
        size_t delta = slot >= sym.runtimeBase ? slot - sym.runtimeBase : 0;
        SSLOT_LOG("  SYM name=%s symRt=%#zx symVma=%#zx size=%zu delta=%zu", sym.name,
                  static_cast<size_t>(sym.runtimeBase), static_cast<size_t>(sym.symVma), sym.size, delta);
    } else {
        SSLOT_LOG("  SYM name=(none)");
    }
    if (hasDl) {
        SSLOT_LOG("  DLADDR fname=%s fbase=%p sname=%s saddr=%p", dli.dli_fname ? dli.dli_fname : "(null)", dli.dli_fbase,
                  dli.dli_sname ? dli.dli_sname : "(null)", dli.dli_saddr);
    } else {
        SSLOT_LOG("  DLADDR (none)");
    }
    // Registration path (source fact): StaticRootTable::VisitRoots walks
    // array->content[i] pointers registered via CjFile::Load*CJFileMeta → RegisterStaticRoots.
    SSLOT_LOG("  REG path=CjFile.LoadLinuxCJFileMeta:146-153->Heap.RegisterStaticRoots->"
              "StaticRootTable.VisitRoots:167-184 slot_is_RefField_storage content_i_points_here");
}

void StaticSlotProbe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    SSLOT_LOG("SUMMARY site=%s total=%llu base=%llu interior=%llu unknown=%llu not_heap=%llu null=%llu "
              "int_off16=%llu int_offOther=%llu unique_slots_seen=%zu",
              site != nullptr ? site : "?", static_cast<unsigned long long>(gTotal.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(gBase.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(gInterior.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(gUnknown.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(gNotHeap.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(gNull.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(gInteriorOff16.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(gInteriorOffOther.load(std::memory_order_relaxed)), gSeenN);
}

} // namespace MapleRuntime
