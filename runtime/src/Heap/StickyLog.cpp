// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "StickyLog.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "Allocator/MemMap.h"
#include "Allocator/RegionInfo.h"
#include "Base/ImmortalWrapper.h"
#include "Base/Log.h"
#include "Base/MemUtils.h"
#include "Base/Panic.h"
#include "Heap/RemsetCheck.h"
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

// Sticky minor remset is only complete when the main managed executable embeds the
// compiler sticky-logged-map consumer (`__cj_sticky_logged_base`). Runtime defines
// that symbol; scanning /proc/self/exe (not the runtime DSO) detects consumer code.
// Missing consumer + fast minor ⇒ unreclaimed live young objects (L355 bare rc139).
// StickyLog.cpp:ConfigureMinorFromEnvironment / Heap.cpp:190-193
bool MainExecutableHasStickyConsumer()
{
    FILE* file = std::fopen("/proc/self/exe", "rb");
    if (file == nullptr) {
        return false;
    }
    static constexpr char needle[] = "__cj_sticky_logged_base";
    static constexpr size_t needleLen = sizeof(needle) - 1;
    static constexpr size_t chunkSize = 1 << 20;
    std::vector<char> buf(chunkSize + needleLen);
    size_t carry = 0;
    bool found = false;
    while (!found) {
        size_t n = std::fread(buf.data() + carry, 1, chunkSize, file);
        if (n == 0) {
            break;
        }
        size_t total = carry + n;
        for (size_t i = 0; i + needleLen <= total; ++i) {
            if (std::memcmp(buf.data() + i, needle, needleLen) == 0) {
                found = true;
                break;
            }
        }
        if (found || n < chunkSize) {
            break;
        }
        if (needleLen > 1) {
            std::memmove(buf.data(), buf.data() + total - (needleLen - 1), needleLen - 1);
            carry = needleLen - 1;
        } else {
            carry = 0;
        }
    }
    std::fclose(file);
    return found;
}
} // namespace

StickyLog& StickyLog::Instance() noexcept { return *g_stickyLog; }

void StickyLog::ConfigureMinorFromEnvironment()
{
    // Product default ON (0.0.2 form A). Exact MRT_STICKY_MINOR=0 is the escape hatch.
    const char* minorEnv = std::getenv("MRT_STICKY_MINOR");
    const bool envExplicitOff = minorEnv != nullptr && strcmp(minorEnv, "0") == 0;
    minorEnabled = ReadStickyBoolean("MRT_STICKY_MINOR", true);
    minorValidatorEnabled = ReadStickyBoolean("MRT_STICKY_MINOR_VALIDATE", false);
    forceSlowPathEnabled = ReadStickyBoolean("MRT_STICKY_MINOR_FORCE_SLOW_PATH", false);
    RemsetCheck::Instance().ConfigureFromEnvironment(forceSlowPathEnabled);
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
    // Fail-safe for non-sticky main ELF (L355 / stdiofd): fast sticky minor with empty remset
    // reclaims live young objects. Prefer disable minor over force-slow: force-slow is
    // a harness that still aborts under this load; major-only matches sticky0 green path.
    // Does not claim full sticky product closure (see REPORT-stickyclosure.md).
    const char* mode = "on(default)";
    if (envExplicitOff) {
        mode = "off(env)";
    } else if (minorEnabled && !forceSlowPathEnabled && !MainExecutableHasStickyConsumer()) {
        minorEnabled = false;
        mode = "auto-disabled(no consumer)";
        LOG(RTLOG_WARNING,
            "sticky minor on by default but main executable has no sticky barrier consumer "
            "(__cj_sticky_logged_base); disabling sticky minor to avoid incorrect young "
            "reclamation (use a sticky-lowered main binary, or MRT_STICKY_MINOR=0 / "
            "MRT_STICKY_MINOR_FORCE_SLOW_PATH=1)");
    }
    LOG(RTLOG_INFO, "sticky minor: %s", mode);
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
    RemsetCheck::Instance().Fini();
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
    minorEnabled = false;
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
    RemsetCheck& check = RemsetCheck::Instance();
    StickyLog& stickyLog = StickyLog::Instance();
    if (object == nullptr) {
        check.RecordStickyLogExit(RemsetCheck::StickyLogExit::EXIT_A_NULL_OBJECT, object, __cj_sticky_heap_base,
                                  __cj_sticky_heap_size);
        return;
    }
    MAddress address = reinterpret_cast<MAddress>(object);
    if (LIKELY(stickyLog.IsLoggedLine(address))) {
        check.RecordStickyLogExit(RemsetCheck::StickyLogExit::EXIT_B_ALREADY_LOGGED, object, __cj_sticky_heap_base,
                                  __cj_sticky_heap_size);
        return;
    }
    Mutator* mutator = Mutator::GetMutator();
    if (UNLIKELY(mutator == nullptr)) {
        check.RecordStickyLogExit(RemsetCheck::StickyLogExit::EXIT_C_NO_MUTATOR, object, __cj_sticky_heap_base,
                                  __cj_sticky_heap_size);
        return;
    }
    MAddress lineStart = 0;
    if (stickyLog.TryLogLine(address, lineStart)) {
        mutator->RememberLineInStickyLogBuffer(lineStart);
        check.RecordStickyLogExit(RemsetCheck::StickyLogExit::LOGGED, object, __cj_sticky_heap_base,
                                  __cj_sticky_heap_size);
        return;
    }
    RemsetCheck::StickyLogExit exit = RemsetCheck::StickyLogExit::EXIT_D_OTHER;
    if (address < __cj_sticky_heap_base || address - __cj_sticky_heap_base >= __cj_sticky_heap_size) {
        exit = RemsetCheck::StickyLogExit::EXIT_D_OUT_OF_HEAP_RANGE;
    } else if (__cj_sticky_logged_base == nullptr) {
        exit = RemsetCheck::StickyLogExit::EXIT_D_BASE_NULL;
    }
    check.RecordStickyLogExit(exit, object, __cj_sticky_heap_base, __cj_sticky_heap_size);
}
} // namespace MapleRuntime
