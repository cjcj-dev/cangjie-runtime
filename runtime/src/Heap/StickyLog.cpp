// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "StickyLog.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#if defined(__linux__) || defined(__OHOS__)
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#endif

#include "Allocator/MemMap.h"
#include "Allocator/RegionInfo.h"
#include "Base/ImmortalWrapper.h"
#include "Base/Log.h"
#include "Base/MemUtils.h"
#include "Base/Panic.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/SatbBuffer.h"

namespace MapleRuntime {
extern "C" MRT_EXPORT uint8_t* __cj_sticky_logged_base = nullptr;
extern "C" MRT_EXPORT uintptr_t __cj_sticky_heap_base = 0;
extern "C" MRT_EXPORT uintptr_t __cj_sticky_heap_size = 0;
extern "C" MRT_EXPORT const uint8_t __cj_sticky_line_shift = StickyLog::LINE_SHIFT;

static ImmortalWrapper<StickyLog> g_stickyLog;

namespace {
bool ReadStickyBoolean(const char* name, bool defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return defaultValue;
    }
    if (strcmp(value, "1") == 0) {
        return true;
    }
    if (strcmp(value, "0") == 0) {
        return false;
    }
    LOG(RTLOG_ERROR, "Unsupported %s=%s; expected 0 or 1, using default %u", name, value,
        static_cast<unsigned int>(defaultValue));
    return defaultValue;
}

size_t ReadStickyPositiveInteger(const char* name, size_t defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return defaultValue;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<size_t>::max()) {
        LOG(RTLOG_ERROR, "Unsupported %s=%s; using default %zu", name, value, defaultValue);
        return defaultValue;
    }
    return static_cast<size_t>(parsed);
}

size_t ReadStickyNonNegativeInteger(const char* name, size_t defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return defaultValue;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (std::strchr(value, '-') != nullptr || errno != 0 || end == value || *end != '\0' ||
        parsed > std::numeric_limits<size_t>::max()) {
        LOG(RTLOG_ERROR, "Unsupported %s=%s; using default %zu", name, value, defaultValue);
        return defaultValue;
    }
    return static_cast<size_t>(parsed);
}

// Sticky minor is only sound when *every* already-mapped managed object
// *consumes* `__cj_sticky_logged_base` (compiler barrier lowering emits UND).
// Runtime itself *defines* the symbol (no .cjmetadata) and is not a managed
// participant. A raw byte scan confuses DEF for consumer; an *any*-UND scan
// opens minor when only sticky std is present while a non-sticky user module
// still does old→young writes (L355 bare rc139). Safety condition is *all*
// managed modules (identified by .cjmetadata, same as StackManager) carry UND.
// Fail-closed: unreadable image, empty managed set, or any managed module
// without UND ⇒ report no (major-only).
static constexpr char kStickyConsumerSym[] = "__cj_sticky_logged_base";
static constexpr char kCjMetadataSection[] = ".cjmetadata";

#if defined(__linux__) || defined(__OHOS__)
// Open+mmap ELF once; fill hasCjMetadata / hasStickyUnd. Returns false if the
// file cannot be proven well-formed (caller must fail-closed).
bool InspectElfStickyFacts(const char* path, bool& hasCjMetadata, bool& hasStickyUnd)
{
    hasCjMetadata = false;
    hasStickyUnd = false;
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return false;
    }
    size_t fileSize = static_cast<size_t>(st.st_size);
    void* mapped = ::mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
        return false;
    }
    auto* base = static_cast<const uint8_t*>(mapped);
    if (fileSize < sizeof(ElfW(Ehdr))) {
        ::munmap(mapped, fileSize);
        return false;
    }
    auto* ehdr = reinterpret_cast<const ElfW(Ehdr)*>(base);
    if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        ::munmap(mapped, fileSize);
        return false;
    }
    if (ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(ElfW(Shdr)) || ehdr->e_shnum == 0 ||
        ehdr->e_shstrndx >= ehdr->e_shnum ||
        static_cast<size_t>(ehdr->e_shoff) + static_cast<size_t>(ehdr->e_shnum) * sizeof(ElfW(Shdr)) > fileSize) {
        ::munmap(mapped, fileSize);
        return false;
    }
    auto* shdrs = reinterpret_cast<const ElfW(Shdr)*>(base + ehdr->e_shoff);
    const ElfW(Shdr)& shstr = shdrs[ehdr->e_shstrndx];
    if (shstr.sh_type != SHT_STRTAB ||
        static_cast<size_t>(shstr.sh_offset) + static_cast<size_t>(shstr.sh_size) > fileSize) {
        ::munmap(mapped, fileSize);
        return false;
    }
    auto* shstrtab = reinterpret_cast<const char*>(base + shstr.sh_offset);
    size_t shstrSize = static_cast<size_t>(shstr.sh_size);
    for (ElfW(Half) i = 0; i < ehdr->e_shnum; ++i) {
        const ElfW(Shdr)& sec = shdrs[i];
        if (sec.sh_name < shstrSize) {
            const char* secName = shstrtab + sec.sh_name;
            if (std::strcmp(secName, kCjMetadataSection) == 0) {
                hasCjMetadata = true;
            }
        }
        if (sec.sh_type != SHT_DYNSYM && sec.sh_type != SHT_SYMTAB) {
            continue;
        }
        if (sec.sh_entsize != sizeof(ElfW(Sym)) || sec.sh_link >= ehdr->e_shnum) {
            continue;
        }
        const ElfW(Shdr)& strSec = shdrs[sec.sh_link];
        if (strSec.sh_type != SHT_STRTAB ||
            static_cast<size_t>(strSec.sh_offset) + static_cast<size_t>(strSec.sh_size) > fileSize ||
            static_cast<size_t>(sec.sh_offset) + static_cast<size_t>(sec.sh_size) > fileSize) {
            continue;
        }
        auto* strtab = reinterpret_cast<const char*>(base + strSec.sh_offset);
        size_t strSize = static_cast<size_t>(strSec.sh_size);
        size_t nSym = static_cast<size_t>(sec.sh_size) / sizeof(ElfW(Sym));
        auto* syms = reinterpret_cast<const ElfW(Sym)*>(base + sec.sh_offset);
        for (size_t s = 0; s < nSym; ++s) {
            if (syms[s].st_name == 0 || syms[s].st_name >= strSize) {
                continue;
            }
            // Only UND: a defined symbol is the runtime provider, not a consumer.
            if (syms[s].st_shndx != SHN_UNDEF) {
                continue;
            }
            const char* name = strtab + syms[s].st_name;
            if (std::strcmp(name, kStickyConsumerSym) == 0) {
                hasStickyUnd = true;
                break;
            }
        }
    }
    ::munmap(mapped, fileSize);
    return true;
}

struct ConsumerScan {
    // all-predicate accumulators (Linux/OHOS).
    bool ok;              // still true so far; false ⇒ fail-closed
    bool sawManaged;      // at least one .cjmetadata object
    bool sawUnbarriered;  // managed object without sticky UND
};

int InspectLoadedForConsumer(struct dl_phdr_info* info, size_t, void* data)
{
    auto* scan = static_cast<ConsumerScan*>(data);
    if (info == nullptr || !scan->ok) {
        return 0;
    }
    char pathBuf[PATH_MAX];
    const char* path = info->dlpi_name;
    if (path == nullptr || path[0] == '\0') {
        // Main executable: dlpi_name is empty; resolve via /proc/self/exe.
        ssize_t n = ::readlink("/proc/self/exe", pathBuf, sizeof(pathBuf) - 1);
        if (n <= 0) {
            // Cannot identify main image: fail-closed (cannot prove all).
            scan->ok = false;
            return 1;
        }
        pathBuf[n] = '\0';
        path = pathBuf;
    }
    // Skip anonymous/vdso-style entries with no resolvable path (already handled main).
    if (path[0] == '\0') {
        return 0;
    }
    bool hasMeta = false;
    bool hasUnd = false;
    if (!InspectElfStickyFacts(path, hasMeta, hasUnd)) {
        // open/mmap/parse failed. linux-vdso and similar have no openable path —
        // treat as non-managed and skip. A true managed image that cannot be
        // read would be invisible; empty managed set still fails closed below.
        return 0;
    }
    if (!hasMeta) {
        return 0; // not a managed Cangjie image (runtime, libc, …)
    }
    scan->sawManaged = true;
    if (!hasUnd) {
        scan->sawUnbarriered = true;
        scan->ok = false;
        return 1; // early out: mixed / unbarriered managed module
    }
    return 0;
}

// Process-wide all-predicate: every mapped image with .cjmetadata has sticky UND.
// Empty managed set or any unbarriered managed module ⇒ false (major-only).
bool ProcessHasStickyConsumer()
{
    ConsumerScan scan = {true, false, false};
    (void)dl_iterate_phdr(InspectLoadedForConsumer, &scan);
    if (!scan.ok || !scan.sawManaged || scan.sawUnbarriered) {
        return false;
    }
    return true;
}
#elif defined(_WIN32)
bool FileHasStickyConsumerUnd(const char* path)
{
    // PE path: no cheap .cjmetadata+UND all-scan here. Fail-safe major-only.
    (void)path;
    return false;
}

bool ProcessHasStickyConsumer()
{
    // Windows: require *every* loaded module that looks like a Cangjie image
    // (section name ".header", same marker IsCangjieExecutable uses) to import
    // the sticky consumer. Any Cangjie image without the import, or no Cangjie
    // image at all ⇒ false (major-only).
    HANDLE process = GetCurrentProcess();
    DWORD needed = 0;
    if (EnumProcessModules(process, nullptr, 0, &needed) == 0 || needed == 0) {
        return false;
    }
    std::vector<HMODULE> modules(needed / sizeof(HMODULE));
    if (EnumProcessModules(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                           &needed) == 0) {
        return false;
    }
    bool sawManaged = false;
    for (HMODULE module : modules) {
        auto* base = reinterpret_cast<const uint8_t*>(module);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            continue;
        }
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            continue;
        }
        const auto* sectionTable = reinterpret_cast<const uint8_t*>(&nt->OptionalHeader) +
            nt->FileHeader.SizeOfOptionalHeader;
        auto section = reinterpret_cast<const IMAGE_SECTION_HEADER*>(sectionTable);
        bool isCangjie = false;
        for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if (strncmp(reinterpret_cast<const char*>(section->Name), ".header", sizeof(".header") - 1) == 0) {
                isCangjie = true;
                break;
            }
        }
        if (!isCangjie) {
            continue;
        }
        sawManaged = true;
        bool hasImport = false;
        const IMAGE_DATA_DIRECTORY& impDir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (impDir.VirtualAddress != 0) {
            auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + impDir.VirtualAddress);
            for (; imp->Name != 0; ++imp) {
                auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(
                    base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
                for (; thunk->u1.AddressOfData != 0; ++thunk) {
                    if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal)) {
                        continue;
                    }
                    auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + thunk->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), kStickyConsumerSym) == 0) {
                        hasImport = true;
                        break;
                    }
                }
                if (hasImport) {
                    break;
                }
            }
        }
        if (!hasImport) {
            return false; // managed without sticky import
        }
    }
    return sawManaged;
}
#elif defined(__APPLE__)
bool FileHasStickyConsumerUnd(const char* path)
{
    (void)path;
    return false;
}

bool ProcessHasStickyConsumer()
{
    // all-predicate: every loaded image with __cjmetaheader must have sticky UND.
    uint32_t count = _dyld_image_count();
    bool sawManaged = false;
    for (uint32_t i = 0; i < count; ++i) {
        const struct mach_header* mh = _dyld_get_image_header(i);
        if (mh == nullptr) {
            continue;
        }
        bool is64 = mh->magic == MH_MAGIC_64 || mh->magic == MH_CIGAM_64;
        const uint8_t* base = reinterpret_cast<const uint8_t*>(mh);
        const uint8_t* cursor = base + (is64 ? sizeof(struct mach_header_64) : sizeof(struct mach_header));
        uint32_t ncmds = mh->ncmds;
        const char* strtab = nullptr;
        size_t strSize = 0;
        const void* symtab = nullptr;
        uint32_t nsyms = 0;
        bool isCangjie = false;
        for (uint32_t c = 0; c < ncmds; ++c) {
            auto* cmd = reinterpret_cast<const struct load_command*>(cursor);
            if (cmd->cmd == LC_SYMTAB) {
                auto* st = reinterpret_cast<const struct symtab_command*>(cursor);
                strtab = reinterpret_cast<const char*>(base + st->stroff);
                strSize = st->strsize;
                symtab = base + st->symoff;
                nsyms = st->nsyms;
            } else if (cmd->cmd == LC_SEGMENT_64) {
                auto* segment = reinterpret_cast<const struct segment_command_64*>(cursor);
                auto* section = reinterpret_cast<const struct section_64*>(segment + 1);
                for (uint32_t j = 0; j < segment->nsects; ++j) {
                    if (strncmp(section[j].sectname, "__cjmetaheader", sizeof("__cjmetaheader") - 1) == 0) {
                        isCangjie = true;
                    }
                }
            } else if (cmd->cmd == LC_SEGMENT) {
                auto* segment = reinterpret_cast<const struct segment_command*>(cursor);
                auto* section = reinterpret_cast<const struct section*>(segment + 1);
                for (uint32_t j = 0; j < segment->nsects; ++j) {
                    if (strncmp(section[j].sectname, "__cjmetaheader", sizeof("__cjmetaheader") - 1) == 0) {
                        isCangjie = true;
                    }
                }
            }
            cursor += cmd->cmdsize;
        }
        if (!isCangjie) {
            continue;
        }
        sawManaged = true;
        bool hasUnd = false;
        if (strtab != nullptr && symtab != nullptr) {
            for (uint32_t s = 0; s < nsyms; ++s) {
                uint32_t strx;
                uint8_t nType;
                if (is64) {
                    auto* nl = reinterpret_cast<const struct nlist_64*>(symtab) + s;
                    strx = nl->n_un.n_strx;
                    nType = nl->n_type;
                } else {
                    auto* nl = reinterpret_cast<const struct nlist*>(symtab) + s;
                    strx = nl->n_un.n_strx;
                    nType = nl->n_type;
                }
                if (strx == 0 || strx >= strSize) {
                    continue;
                }
                if ((nType & N_TYPE) != N_UNDF || (nType & N_EXT) == 0) {
                    continue;
                }
                const char* name = strtab + strx;
                if (std::strcmp(name, kStickyConsumerSym) == 0 ||
                    (name[0] == '_' && std::strcmp(name + 1, kStickyConsumerSym) == 0)) {
                    hasUnd = true;
                    break;
                }
            }
        }
        if (!hasUnd) {
            return false;
        }
    }
    return sawManaged;
}
#else
bool ProcessHasStickyConsumer()
{
    // No module enumeration: fail-safe to "no consumer" so minor stays off.
    return false;
}
#endif
} // namespace

StickyLog& StickyLog::Instance() noexcept { return *g_stickyLog; }

void StickyLog::ConfigureMinorFromEnvironment()
{
    // Product default ON (0.0.2 form A). Exact MRT_STICKY_MINOR=0 is the escape hatch.
    const char* minorEnv = std::getenv("MRT_STICKY_MINOR");
    const bool envExplicitOff = minorEnv != nullptr && strcmp(minorEnv, "0") == 0;
    minorEnabled.store(ReadStickyBoolean("MRT_STICKY_MINOR", true), std::memory_order_relaxed);
    minorValidatorEnabled = ReadStickyBoolean("MRT_STICKY_MINOR_VALIDATE", false);
    forceSlowPathEnabled = ReadStickyBoolean("MRT_STICKY_MINOR_FORCE_SLOW_PATH", false);
    youngBytesThreshold = ReadStickyPositiveInteger("MRT_STICKY_MINOR_YOUNG_BYTES", DEFAULT_YOUNG_BYTES);
    size_t configuredMajorInterval = ReadStickyPositiveInteger("MRT_STICKY_MINOR_MAJOR_INTERVAL", 8);
    majorInterval = static_cast<uint32_t>(std::min(configuredMajorInterval,
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    // Measurement knob for the promotion-age sweep: default 1 keeps the shipped
    // aging decision bit-identical. Clamped to the widened youngAge field
    // (RegionInfo::MAX_YOUNG_AGE); the interval knob above bounds how many
    // minors an epoch can run, which in turn bounds reachable ages.
    size_t configuredPromoteAge = ReadStickyPositiveInteger("MRT_STICKY_MINOR_PROMOTE_AGE", 1);
    promoteAge = static_cast<uint8_t>(std::min(configuredPromoteAge,
        static_cast<size_t>(RegionInfo::MAX_YOUNG_AGE)));
    evacuationThreshold = std::min(ReadStickyNonNegativeInteger("MRT_STICKY_EVAC_THRESHOLD", 0),
                                   static_cast<size_t>(100));
    evacuationMaxRegions = ReadStickyNonNegativeInteger("MRT_STICKY_EVAC_MAX_REGIONS", 8);
    // Fail-safe (L355): fast sticky minor with incomplete remset reclaims live young.
    // Prefer disable minor over force-slow. Require *every* managed (.cjmetadata)
    // mapped image to UND-consume the sticky map — not any, not /proc/self/exe alone,
    // and not a raw byte scan that confuses the runtime's own DEF for a consumer.
    const char* mode = "on(default)";
    const bool minorOn = minorEnabled.load(std::memory_order_relaxed);
    if (envExplicitOff) {
        mode = "off(env)";
    } else if (minorOn && !forceSlowPathEnabled && !ProcessHasStickyConsumer()) {
        minorEnabled.store(false, std::memory_order_relaxed);
        mode = "auto-disabled(no consumer)";
        LOG(RTLOG_WARNING,
            "sticky minor on by default but not every loaded managed object has a sticky "
            "barrier consumer (__cj_sticky_logged_base UND on each .cjmetadata image); "
            "disabling sticky minor to avoid incorrect young reclamation (use a fully "
            "sticky-lowered main+std, or MRT_STICKY_MINOR=0 / "
            "MRT_STICKY_MINOR_FORCE_SLOW_PATH=1)");
    }
    LOG(RTLOG_INFO, "sticky minor: %s", mode);
}

// Init-time answer only covers objects mapped then. LoadCJLibrary can change the
// set of participants; re-ask. Only ever disables (L355): re-enabling would need
// proof that every write since the load was logged, which nothing here has.
void StickyLog::RevalidateConsumerAfterLibraryLoad(const char* libName)
{
    if (!minorEnabled.load(std::memory_order_relaxed) || forceSlowPathEnabled) {
        return;
    }
    if (ProcessHasStickyConsumer()) {
        return;
    }
    minorEnabled.store(false, std::memory_order_relaxed);
    LOG(RTLOG_WARNING,
        "after loading managed library %s not every loaded managed object has a sticky "
        "barrier consumer (__cj_sticky_logged_base UND); disabling sticky minor from here on "
        "(sticky minor: auto-disabled(no consumer, late load))",
        libName == nullptr ? "<unnamed>" : libName);
}

void StickyLog::Init(MAddress start, size_t size)
{
    MRT_ASSERT(loggedMap == nullptr && dirtyRegionMap == nullptr, "sticky logged map initialized twice");
    MRT_ASSERT((start & (LINE_SIZE - 1)) == 0, "heap start is not sticky-line aligned");
    heapStart = start;
    heapSize = size;
    loggedByteCount = (size + LINE_SIZE - 1) >> LINE_SHIFT;

    MemMap::Option option = MemMap::DEFAULT_OPTIONS;
    option.tag = "cangjie_sticky_logged";
    loggedMap = MemMap::MapMemory(loggedByteCount, loggedByteCount, option);
    size_t regionCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;
    dirtyRegionByteCount = (regionCount + 7) / 8;
    option.tag = "cangjie_sticky_dirty_regions";
    dirtyRegionMap = MemMap::MapMemory(dirtyRegionByteCount, dirtyRegionByteCount, option);
#ifdef _WIN64
    MemMap::CommitMemory(loggedMap->GetBaseAddr(), loggedByteCount);
    MemMap::CommitMemory(dirtyRegionMap->GetBaseAddr(), dirtyRegionByteCount);
#endif
    __cj_sticky_logged_base = reinterpret_cast<uint8_t*>(loggedMap->GetBaseAddr());
    __cj_sticky_heap_base = heapStart;
    __cj_sticky_heap_size = heapSize;
}

void StickyLog::Fini() noexcept
{
    __cj_sticky_logged_base = nullptr;
    __cj_sticky_heap_base = 0;
    __cj_sticky_heap_size = 0;
    MemMap::DestroyMemMap(loggedMap);
    MemMap::DestroyMemMap(dirtyRegionMap);
    heapStart = 0;
    heapSize = 0;
    loggedByteCount = 0;
    dirtyRegionByteCount = 0;
    enabled = false;
    minorEnabled.store(false, std::memory_order_relaxed);
    minorValidatorEnabled = false;
    forceSlowPathEnabled = false;
}

bool StickyLog::IsLoggedLine(MAddress address) const
{
    if (UNLIKELY(address < heapStart || address >= heapStart + heapSize || __cj_sticky_logged_base == nullptr)) {
        return false;
    }
    size_t lineIndex = (address - heapStart) >> LINE_SHIFT;
    return *reinterpret_cast<volatile uint8_t*>(__cj_sticky_logged_base + lineIndex) != 0;
}

bool StickyLog::TryLogLine(MAddress address, MAddress& lineStart) const
{
    if (UNLIKELY(address < heapStart || address >= heapStart + heapSize || __cj_sticky_logged_base == nullptr)) {
        return false;
    }
    size_t lineIndex = (address - heapStart) >> LINE_SHIFT;
    volatile uint8_t* loggedByte = __cj_sticky_logged_base + lineIndex;
    if (*loggedByte != 0) {
        return false;
    }
    *loggedByte = 1;
    RegionInfo* region = RegionInfo::GetRegionInfoAt(address);
    size_t regionIndex = (region->GetRegionStart() - heapStart) / RegionInfo::UNIT_SIZE;
    uint8_t* dirtyByte = reinterpret_cast<uint8_t*>(dirtyRegionMap->GetBaseAddr()) + regionIndex / 8;
    __atomic_fetch_or(dirtyByte, static_cast<uint8_t>(1U << (regionIndex % 8)), __ATOMIC_RELEASE);
    lineStart = heapStart + (lineIndex << LINE_SHIFT);
    return true;
}

void StickyLog::ClearUnavailableRegion(MAddress regionStart, size_t regionSize)
{
    MRT_ASSERT(regionStart >= heapStart && regionStart + regionSize <= heapStart + heapSize,
               "sticky region clear is outside heap");
    MRT_ASSERT((regionStart & (LINE_SIZE - 1)) == 0 && (regionSize & (LINE_SIZE - 1)) == 0,
               "sticky region clear is not line aligned");
    size_t firstLine = (regionStart - heapStart) >> LINE_SHIFT;
    size_t lineCount = regionSize >> LINE_SHIFT;
    MemorySet(reinterpret_cast<uintptr_t>(__cj_sticky_logged_base + firstLine), lineCount, 0, lineCount);
    size_t regionIndex = (regionStart - heapStart) / RegionInfo::UNIT_SIZE;
    uint8_t* dirtyByte = reinterpret_cast<uint8_t*>(dirtyRegionMap->GetBaseAddr()) + regionIndex / 8;
    __atomic_fetch_and(dirtyByte, static_cast<uint8_t>(~(1U << (regionIndex % 8))), __ATOMIC_RELEASE);
}

void StickyLog::BeginEpoch()
{
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "sticky epoch may only advance while mutators are stopped");
    MemorySet(reinterpret_cast<uintptr_t>(__cj_sticky_logged_base), loggedByteCount, 0, loggedByteCount);
    MemorySet(reinterpret_cast<uintptr_t>(dirtyRegionMap->GetBaseAddr()), dirtyRegionByteCount, 0,
              dirtyRegionByteCount);
}

void StickyLog::RescanLoggedLines(const LoggedLineVisitor& visitor)
{
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "sticky lines may only be consumed while mutators are stopped");
    SatbBuffer::Instance().VisitStickyLogLines([this, &visitor](MAddress lineStart) {
        if (!IsLoggedLine(lineStart)) {
            return;
        }
        size_t lineIndex = (lineStart - heapStart) >> LINE_SHIFT;
        uint8_t retained = visitor(lineStart, lineStart + LINE_SIZE) ? 2 : 0;
        __atomic_store_n(__cj_sticky_logged_base + lineIndex, retained, __ATOMIC_RELEASE);
    });

    uint8_t* dirtyBytes = reinterpret_cast<uint8_t*>(dirtyRegionMap->GetBaseAddr());
    size_t regionCount = (heapSize + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;
    for (size_t regionIndex = 0; regionIndex < regionCount; ++regionIndex) {
        uint8_t mask = static_cast<uint8_t>(1U << (regionIndex % 8));
        uint8_t* dirtyByte = dirtyBytes + regionIndex / 8;
        if ((__atomic_load_n(dirtyByte, __ATOMIC_ACQUIRE) & mask) == 0) {
            continue;
        }
        MAddress regionAddress = heapStart + regionIndex * RegionInfo::UNIT_SIZE;
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddress);
        bool regionRetained = false;
        if (region->IsValidRegion() && region->GetRegionStart() == regionAddress) {
            size_t firstLine = (regionAddress - heapStart) >> LINE_SHIFT;
            size_t lineCount = region->GetRegionSize() >> LINE_SHIFT;
            for (size_t lineOffset = 0; lineOffset < lineCount; ++lineOffset) {
                uint8_t* loggedByte = __cj_sticky_logged_base + firstLine + lineOffset;
                uint8_t logged = __atomic_load_n(loggedByte, __ATOMIC_ACQUIRE);
                if (logged == 0) {
                    continue;
                }
                bool retain = logged == 2;
                if (!retain) {
                    MAddress lineStart = regionAddress + (lineOffset << LINE_SHIFT);
                    retain = visitor(lineStart, lineStart + LINE_SIZE);
                }
                __atomic_store_n(loggedByte, static_cast<uint8_t>(retain), __ATOMIC_RELEASE);
                regionRetained |= retain;
            }
        }
        if (!regionRetained) {
            __atomic_fetch_and(dirtyByte, static_cast<uint8_t>(~mask), __ATOMIC_RELEASE);
        }
    }
}

extern "C" MRT_EXPORT void CJ_MCC_StickyLogLine(BaseObject* object)
{
    if (object == nullptr) {
        return;
    }
    StickyLog& stickyLog = StickyLog::Instance();
    MAddress address = reinterpret_cast<MAddress>(object);
    if (LIKELY(stickyLog.IsLoggedLine(address))) {
        return;
    }
    Mutator* mutator = Mutator::GetMutator();
    if (UNLIKELY(mutator == nullptr)) {
        return;
    }
    MAddress lineStart = 0;
    if (stickyLog.TryLogLine(address, lineStart)) {
        mutator->RememberLineInStickyLogBuffer(lineStart);
    }
}
} // namespace MapleRuntime
