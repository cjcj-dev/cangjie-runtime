// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Verify/StatHealDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "Base/Log.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace StatHealDiag {
namespace {

struct MapEntry {
    uintptr_t start{ 0 };
    uintptr_t end{ 0 };
    char perms[5]{ '-', '-', '-', '-', '\0' };
    char path[256]{ 0 };
};

enum class Mapping : uint8_t {
    Unmapped,
    ReadOnly,
    Writable,
};

struct SlotReads {
    uint64_t bad{ 0 };
    uint64_t routed{ 0 };
    uint64_t resolved{ 0 };
    bool mappingCounted{ false };
};

struct CycleReads {
    uint64_t bad{ 0 };
    uint64_t badRepeat{ 0 };
    uint64_t routed{ 0 };
    uint64_t routedRepeat{ 0 };
    uint64_t resolved{ 0 };
    uint64_t resolvedRepeat{ 0 };
    uint64_t healAttempted{ 0 };
    uint64_t healSucceeded{ 0 };
    uint64_t uniqueSlots{ 0 };
    uint64_t writableSlots{ 0 };
    uint64_t readOnlySlots{ 0 };
    uint64_t unmappedSlots{ 0 };
};

struct RootScan {
    uint64_t registered{ 0 };
    uint64_t writable{ 0 };
    uint64_t readOnly{ 0 };
    uint64_t unmapped{ 0 };
    uint64_t fileBacked{ 0 };
    uint64_t anonymous{ 0 };
};

std::mutex g_readLock;
std::mutex g_scanLock;
std::map<std::pair<uint32_t, uintptr_t>, SlotReads> g_slots;
std::map<uint32_t, CycleReads> g_cycles;
std::vector<MapEntry> g_scanMaps;
RootScan g_scan;
RootScan g_lastScan;
uint32_t g_lastScanGc{ UINT32_MAX };
bool g_inScan{ false };
std::atomic<bool> g_atexit{ false };
std::once_flag g_mappingSelfTestOnce;

std::vector<MapEntry> ReadMaps()
{
    std::vector<MapEntry> result;
#if defined(__linux__)
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) {
        return result;
    }
    char line[768];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0;
        unsigned long end = 0;
        char perms[5]{};
        int consumed = 0;
        if (std::sscanf(line, "%lx-%lx %4s %*x %*s %*s %n", &start, &end, perms, &consumed) < 3) {
            continue;
        }
        MapEntry entry;
        entry.start = static_cast<uintptr_t>(start);
        entry.end = static_cast<uintptr_t>(end);
        std::memcpy(entry.perms, perms, sizeof(entry.perms));
        const char* path = line + consumed;
        while (*path == ' ' || *path == '\t') {
            ++path;
        }
        size_t len = std::strcspn(path, "\r\n");
        if (len >= sizeof(entry.path)) {
            len = sizeof(entry.path) - 1;
        }
        std::memcpy(entry.path, path, len);
        entry.path[len] = '\0';
        result.push_back(entry);
    }
    std::fclose(maps);
#endif
    return result;
}

const MapEntry* FindMap(const std::vector<MapEntry>& maps, uintptr_t address)
{
    for (const MapEntry& entry : maps) {
        if (address >= entry.start && address < entry.end) {
            return &entry;
        }
    }
    return nullptr;
}

Mapping Classify(const MapEntry* entry)
{
    if (entry == nullptr) {
        return Mapping::Unmapped;
    }
    return entry->perms[1] == 'w' ? Mapping::Writable : Mapping::ReadOnly;
}

void RunMappingSelfTest()
{
    std::call_once(g_mappingSelfTestOnce, []() {
#if defined(__linux__)
        int writableSample = 0;
        const long pageSize = sysconf(_SC_PAGESIZE);
        void* readOnlySample = pageSize > 0
            ? mmap(nullptr, static_cast<size_t>(pageSize), PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
            : MAP_FAILED;
        const std::vector<MapEntry> maps = ReadMaps();
        const bool writableOk = Classify(FindMap(maps, reinterpret_cast<uintptr_t>(&writableSample))) ==
            Mapping::Writable;
        const bool readOnlyOk = readOnlySample != MAP_FAILED &&
            Classify(FindMap(maps, reinterpret_cast<uintptr_t>(readOnlySample))) == Mapping::ReadOnly;
        const bool unmappedOk = Classify(FindMap(maps, 0)) == Mapping::Unmapped;
        if (readOnlySample != MAP_FAILED) {
            (void)munmap(readOnlySample, static_cast<size_t>(pageSize));
        }
        const unsigned trust = static_cast<unsigned>(writableOk) + static_cast<unsigned>(readOnlyOk) +
            static_cast<unsigned>(unmappedOk);
        LOG(RTLOG_ERROR,
            "[STATHEAL][maps-selftest] trust=%u/3 writable=%u readonly=%u unmapped=%u",
            trust, static_cast<unsigned>(writableOk), static_cast<unsigned>(readOnlyOk),
            static_cast<unsigned>(unmappedOk));
        CHECK_DETAIL(trust == 3, "STATHEAL /proc/self/maps classifier self-test failed: trust=%u/3", trust);
#else
        LOG(RTLOG_ERROR, "[STATHEAL][maps-selftest] trust=0/0 unsupported-platform");
#endif
    });
}

bool SameScan(const RootScan& left, const RootScan& right)
{
    return left.registered == right.registered && left.writable == right.writable &&
        left.readOnly == right.readOnly && left.unmapped == right.unmapped &&
        left.fileBacked == right.fileBacked && left.anonymous == right.anonymous;
}

void InstallAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

} // namespace

bool Enabled()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        return DiagGate::LegacyOrToken("MRT_GCV2_STATHEAL", "statheal");
    }();
    return on;
}

bool SuppressHealForAB()
{
    if (!Enabled()) {
        return false;
    }
    static const bool suppress = []() {
        const char* value = std::getenv("MRT_GCV2_STATHEAL_NOHEAL");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return suppress;
}

void NoteStaticRead(const RootSlot& slot, uintptr_t observed, bool badColour, bool routed,
                    bool resolvedChanged, bool healAttempted, bool healSucceeded)
{
    if (!Enabled() || (!badColour && !routed && !resolvedChanged && !healAttempted)) {
        return;
    }
    InstallAtexit();
    RunMappingSelfTest();
    const uint32_t gc = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    const uintptr_t address = reinterpret_cast<uintptr_t>(&slot);
    std::lock_guard<std::mutex> lock(g_readLock);
    SlotReads& reads = g_slots[std::make_pair(gc, address)];
    CycleReads& cycle = g_cycles[gc];
    if (!reads.mappingCounted) {
        reads.mappingCounted = true;
        ++cycle.uniqueSlots;
        const std::vector<MapEntry> maps = ReadMaps();
        switch (Classify(FindMap(maps, address))) {
            case Mapping::Writable:
                ++cycle.writableSlots;
                break;
            case Mapping::ReadOnly:
                ++cycle.readOnlySlots;
                break;
            default:
                ++cycle.unmappedSlots;
                break;
        }
    }
    if (badColour) {
        ++cycle.bad;
        if (reads.bad != 0) {
            ++cycle.badRepeat;
        }
        ++reads.bad;
    }
    if (routed) {
        ++cycle.routed;
        if (reads.routed != 0) {
            ++cycle.routedRepeat;
        }
        ++reads.routed;
    }
    if (resolvedChanged) {
        ++cycle.resolved;
        if (reads.resolved != 0) {
            ++cycle.resolvedRepeat;
        }
        ++reads.resolved;
    }
    if (healAttempted) {
        ++cycle.healAttempted;
    }
    if (healSucceeded) {
        ++cycle.healSucceeded;
    }
    const uint64_t n = resolvedChanged ? cycle.resolved : (badColour ? cycle.bad : cycle.routed);
    if (n <= 8 || (n & (n - 1)) == 0) {
        LOG(RTLOG_ERROR,
            "[STATHEAL][read] gc=%u slot=%p observed=%#lx bad=%u routed=%u resolved=%u "
            "healAttempted=%u healSucceeded=%u cycle{bad=%lu badRepeat=%lu routed=%lu "
            "routedRepeat=%lu resolved=%lu resolvedRepeat=%lu unique=%lu}",
            gc, static_cast<const void*>(&slot), static_cast<unsigned long>(observed),
            static_cast<unsigned>(badColour), static_cast<unsigned>(routed),
            static_cast<unsigned>(resolvedChanged), static_cast<unsigned>(healAttempted),
            static_cast<unsigned>(healSucceeded),
            static_cast<unsigned long>(cycle.bad), static_cast<unsigned long>(cycle.badRepeat),
            static_cast<unsigned long>(cycle.routed), static_cast<unsigned long>(cycle.routedRepeat),
            static_cast<unsigned long>(cycle.resolved), static_cast<unsigned long>(cycle.resolvedRepeat),
            static_cast<unsigned long>(cycle.uniqueSlots));
    }
}

void BeginStaticRootScan()
{
    if (!Enabled()) {
        return;
    }
    InstallAtexit();
    RunMappingSelfTest();
    g_scanLock.lock();
    g_scanMaps = ReadMaps();
    g_scan = RootScan{};
    g_inScan = true;
}

void NoteStaticRootSlot(const RootSlot& slot)
{
    if (!Enabled() || !g_inScan) {
        return;
    }
    ++g_scan.registered;
    const MapEntry* entry = FindMap(g_scanMaps, reinterpret_cast<uintptr_t>(&slot));
    switch (Classify(entry)) {
        case Mapping::Writable:
            ++g_scan.writable;
            break;
        case Mapping::ReadOnly:
            ++g_scan.readOnly;
            if (g_scan.readOnly <= 16) {
                LOG(RTLOG_ERROR,
                    "[STATHEAL][readonly-root] slot=%p map=%p-%p perms=%s path=%s",
                    static_cast<const void*>(&slot),
                    reinterpret_cast<void*>(entry->start), reinterpret_cast<void*>(entry->end),
                    entry->perms, entry->path[0] == '\0' ? "-" : entry->path);
            }
            break;
        default:
            ++g_scan.unmapped;
            break;
    }
    if (entry != nullptr) {
        if (entry->path[0] == '/') {
            ++g_scan.fileBacked;
        } else {
            ++g_scan.anonymous;
        }
    }
}

void EndStaticRootScan()
{
    if (!Enabled() || !g_inScan) {
        return;
    }
    const uint32_t gc = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    if (g_lastScanGc != gc || !SameScan(g_scan, g_lastScan)) {
        LOG(RTLOG_ERROR,
            "[STATHEAL][root-scan] gc=%u registered=%lu writable=%lu readonly=%lu unmapped=%lu "
            "fileBacked=%lu anonymous=%lu",
            gc, static_cast<unsigned long>(g_scan.registered), static_cast<unsigned long>(g_scan.writable),
            static_cast<unsigned long>(g_scan.readOnly), static_cast<unsigned long>(g_scan.unmapped),
            static_cast<unsigned long>(g_scan.fileBacked), static_cast<unsigned long>(g_scan.anonymous));
        g_lastScanGc = gc;
        g_lastScan = g_scan;
    }
    g_scanMaps.clear();
    g_inScan = false;
    g_scanLock.unlock();
}

void Report(const char* point)
{
    if (!Enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_readLock);
    uint64_t totalBad = 0;
    uint64_t totalBadRepeat = 0;
    uint64_t totalRouted = 0;
    uint64_t totalRoutedRepeat = 0;
    uint64_t totalResolved = 0;
    uint64_t totalResolvedRepeat = 0;
    uint64_t totalHealAttempted = 0;
    uint64_t totalHealSucceeded = 0;
    for (const auto& entry : g_cycles) {
        const CycleReads& cycle = entry.second;
        totalBad += cycle.bad;
        totalBadRepeat += cycle.badRepeat;
        totalRouted += cycle.routed;
        totalRoutedRepeat += cycle.routedRepeat;
        totalResolved += cycle.resolved;
        totalResolvedRepeat += cycle.resolvedRepeat;
        totalHealAttempted += cycle.healAttempted;
        totalHealSucceeded += cycle.healSucceeded;
        LOG(RTLOG_ERROR,
            "[STATHEAL][cycle] point=%s gc=%u bad=%lu badRepeat=%lu routed=%lu routedRepeat=%lu "
            "resolved=%lu resolvedRepeat=%lu healAttempted=%lu healSucceeded=%lu uniqueSlots=%lu "
            "readMap{writable=%lu readonly=%lu unmapped=%lu}",
            point == nullptr ? "?" : point, entry.first,
            static_cast<unsigned long>(cycle.bad), static_cast<unsigned long>(cycle.badRepeat),
            static_cast<unsigned long>(cycle.routed), static_cast<unsigned long>(cycle.routedRepeat),
            static_cast<unsigned long>(cycle.resolved), static_cast<unsigned long>(cycle.resolvedRepeat),
            static_cast<unsigned long>(cycle.healAttempted), static_cast<unsigned long>(cycle.healSucceeded),
            static_cast<unsigned long>(cycle.uniqueSlots), static_cast<unsigned long>(cycle.writableSlots),
            static_cast<unsigned long>(cycle.readOnlySlots), static_cast<unsigned long>(cycle.unmappedSlots));
    }
    LOG(RTLOG_ERROR,
        "[STATHEAL][sum] point=%s cycles=%zu bad=%lu badRepeat=%lu routed=%lu routedRepeat=%lu "
        "resolved=%lu resolvedRepeat=%lu healAttempted=%lu healSucceeded=%lu "
        "lastRootScan{registered=%lu writable=%lu readonly=%lu unmapped=%lu fileBacked=%lu anonymous=%lu}",
        point == nullptr ? "?" : point, g_cycles.size(), static_cast<unsigned long>(totalBad),
        static_cast<unsigned long>(totalBadRepeat), static_cast<unsigned long>(totalRouted),
        static_cast<unsigned long>(totalRoutedRepeat), static_cast<unsigned long>(totalResolved),
        static_cast<unsigned long>(totalResolvedRepeat), static_cast<unsigned long>(totalHealAttempted),
        static_cast<unsigned long>(totalHealSucceeded), static_cast<unsigned long>(g_lastScan.registered),
        static_cast<unsigned long>(g_lastScan.writable), static_cast<unsigned long>(g_lastScan.readOnly),
        static_cast<unsigned long>(g_lastScan.unmapped), static_cast<unsigned long>(g_lastScan.fileBacked),
        static_cast<unsigned long>(g_lastScan.anonymous));
}

} // namespace StatHealDiag
} // namespace MapleRuntime
