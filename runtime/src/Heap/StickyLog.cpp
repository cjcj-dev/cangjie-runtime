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

// Sticky minor is only sound when *some* already-mapped object *consumes*
// `__cj_sticky_logged_base` (compiler barrier lowering emits UND references).
// Runtime itself *defines* the symbol, so a raw byte scan of any loaded image
// that includes the runtime DSO is a permanent false positive, and a scan of
// only /proc/self/exe is a false negative when the consumer lives in a .so
// (the normal --dy-std shape). Ask the process-wide question via the dynamic
// symbol table: look for an undefined reference, never a definition.
// Missing consumer + fast minor ⇒ unreclaimed live young objects (L355 bare rc139).
static constexpr char kStickyConsumerSym[] = "__cj_sticky_logged_base";

#if defined(__linux__) || defined(__OHOS__)
bool FileHasStickyConsumerUnd(const char* path)
{
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
    bool found = false;
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
    if (ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(ElfW(Shdr)) ||
        static_cast<size_t>(ehdr->e_shoff) + static_cast<size_t>(ehdr->e_shnum) * sizeof(ElfW(Shdr)) > fileSize) {
        ::munmap(mapped, fileSize);
        return false;
    }
    auto* shdrs = reinterpret_cast<const ElfW(Shdr)*>(base + ehdr->e_shoff);
    for (ElfW(Half) i = 0; i < ehdr->e_shnum && !found; ++i) {
        const ElfW(Shdr)& sec = shdrs[i];
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
                found = true;
                break;
            }
        }
    }
    ::munmap(mapped, fileSize);
    return found;
}

struct ConsumerScan {
    bool found;
};

int InspectLoadedForConsumer(struct dl_phdr_info* info, size_t, void* data)
{
    auto* scan = static_cast<ConsumerScan*>(data);
    if (info == nullptr) {
        return 0;
    }
    char pathBuf[PATH_MAX];
    const char* path = info->dlpi_name;
    if (path == nullptr || path[0] == '\0') {
        // Main executable: dlpi_name is empty; resolve via /proc/self/exe.
        ssize_t n = ::readlink("/proc/self/exe", pathBuf, sizeof(pathBuf) - 1);
        if (n <= 0) {
            return 0;
        }
        pathBuf[n] = '\0';
        path = pathBuf;
    }
    if (FileHasStickyConsumerUnd(path)) {
        scan->found = true;
        return 1; // stop iteration
    }
    return 0;
}

// Process-wide: true iff any currently mapped ELF has an UND ref to the sticky consumer.
bool ProcessHasStickyConsumer()
{
    ConsumerScan scan = {false};
    (void)dl_iterate_phdr(InspectLoadedForConsumer, &scan);
    return scan.found;
}
#elif defined(_WIN32)
bool FileHasStickyConsumerUnd(const char* path)
{
    // PE has no direct ELF UND analogue we can cheaply scan here. Keep the historical
    // fail-safe: if we cannot prove a consumer, report none (major-only).
    (void)path;
    return false;
}

bool ProcessHasStickyConsumer()
{
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
    for (HMODULE module : modules) {
        char path[MAX_PATH];
        DWORD written = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
        if (written == 0 || written >= sizeof(path) - 1) {
            continue;
        }
        // On Windows the consumer shows up as an import of the sticky symbol from
        // the runtime DLL. Walk the import table for the name.
        auto* base = reinterpret_cast<const uint8_t*>(module);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            continue;
        }
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            continue;
        }
        const IMAGE_DATA_DIRECTORY& impDir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (impDir.VirtualAddress == 0) {
            continue;
        }
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
                    return true;
                }
            }
        }
    }
    return false;
}
#elif defined(__APPLE__)
bool FileHasStickyConsumerUnd(const char* path)
{
    (void)path;
    return false;
}

bool ProcessHasStickyConsumer()
{
    // Walk loaded images' symbol tables for an undefined sticky consumer.
    uint32_t count = _dyld_image_count();
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
        size_t symEnt = is64 ? sizeof(struct nlist_64) : sizeof(struct nlist);
        for (uint32_t c = 0; c < ncmds; ++c) {
            auto* cmd = reinterpret_cast<const struct load_command*>(cursor);
            if (cmd->cmd == LC_SYMTAB) {
                auto* st = reinterpret_cast<const struct symtab_command*>(cursor);
                strtab = reinterpret_cast<const char*>(base + st->stroff);
                strSize = st->strsize;
                symtab = base + st->symoff;
                nsyms = st->nsyms;
            }
            cursor += cmd->cmdsize;
        }
        if (strtab == nullptr || symtab == nullptr) {
            continue;
        }
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
            // Undefined external: N_EXT set, N_TYPE bits clear (N_UNDF).
            if ((nType & N_TYPE) != N_UNDF || (nType & N_EXT) == 0) {
                continue;
            }
            const char* name = strtab + strx;
            // Mach-O C symbols are typically underscore-prefixed.
            if (std::strcmp(name, kStickyConsumerSym) == 0 ||
                (name[0] == '_' && std::strcmp(name + 1, kStickyConsumerSym) == 0)) {
                return true;
            }
        }
    }
    return false;
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
    // Fail-safe (L355 / stdiofd): fast sticky minor with empty remset reclaims live
    // young objects. Prefer disable minor over force-slow. Ask the *process* whether
    // any mapped object consumes the sticky map — not just /proc/self/exe, and not
    // via a raw byte scan that confuses the runtime's own definition for a consumer.
    const char* mode = "on(default)";
    const bool minorOn = minorEnabled.load(std::memory_order_relaxed);
    if (envExplicitOff) {
        mode = "off(env)";
    } else if (minorOn && !forceSlowPathEnabled && !ProcessHasStickyConsumer()) {
        minorEnabled.store(false, std::memory_order_relaxed);
        mode = "auto-disabled(no consumer)";
        LOG(RTLOG_WARNING,
            "sticky minor on by default but no loaded object has a sticky barrier consumer "
            "(__cj_sticky_logged_base UND); disabling sticky minor to avoid incorrect young "
            "reclamation (use a sticky-lowered main or std, or MRT_STICKY_MINOR=0 / "
            "MRT_STICKY_MINOR_FORCE_SLOW_PATH=1)");
    }
    LOG(RTLOG_INFO, "sticky minor: %s", mode);
}

// Init-time answer only covers objects mapped then. LoadCJLibrary can add a
// managed participant afterwards; re-ask, and only ever disable (cannot prove
// every write since the load was logged).
void StickyLog::RevalidateConsumerAfterLibraryLoad(const char* libName)
{
    if (!minorEnabled.load(std::memory_order_relaxed) || forceSlowPathEnabled) {
        return;
    }
    if (ProcessHasStickyConsumer()) {
        return;
    }
    // Still no consumer after the load: keep minor off. (If minor was already on
    // because some earlier participant had a consumer, a new library without one
    // does not revoke that — ProcessHasStickyConsumer stays true. This path only
    // fires when the process still has zero consumers, which is a no-op disable.)
    (void)libName;
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
