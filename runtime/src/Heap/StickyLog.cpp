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
#include "Heap/Heap.h"
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
    edgeCompleteEnabled = ReadStickyBoolean("MRT_EDGECOMPLETE", false);
    edgeCompleteFakeMissEnabled =
        edgeCompleteEnabled ? ReadStickyBoolean("MRT_EDGECOMPLETE_FAKEMISS", false) : false;
    edgeCompleteDropN = edgeCompleteEnabled ? ReadStickyPositiveInteger("MRT_EDGECOMPLETE_DROP_N", 0) : 0;
    if (edgeCompleteDropN != 0) {
        forceSlowPathEnabled = true;
        LOG(RTLOG_WARNING,
            "MRT_EDGECOMPLETE_DROP_N=%zu enables the runtime write-barrier path for the edge-completeness "
            "positive control",
            edgeCompleteDropN);
    }
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
    if (edgeCompleteEnabled) {
        double pct = edgeCompleteEdgesToYoung == 0 ? 0.0 :
            static_cast<double>(edgeCompleteMissing) * 100.0 / static_cast<double>(edgeCompleteEdgesToYoung);
        VLOG(REPORT,
             "[EDGECOMPLETE-FINAL] runs=%zu oldObjs=%zu refFields=%zu edgesToYoung=%zu inRemset=%zu "
             "missing=%zu pct=%.6f posctrlN=%zu eligibleStores=%zu candidateAttempts=%zu candidateRejects=%zu "
             "candidateWaitRuns=%zu dropped=%u caught=%u fakeMiss=%u fakeMissCaught=%u",
             edgeCompleteRuns, edgeCompleteOldObjects, edgeCompleteRefFields, edgeCompleteEdgesToYoung,
             edgeCompleteInRemset, edgeCompleteMissing, pct, edgeCompleteDropN,
             edgeCompleteEligibleStores.load(std::memory_order_acquire),
             edgeCompleteCandidateAttempts.load(std::memory_order_acquire), edgeCompleteCandidateRejects,
             edgeCompleteCandidateWaitRuns,
             static_cast<unsigned>(edgeCompleteDropInjected.load(std::memory_order_acquire)),
             static_cast<unsigned>(edgeCompleteDropCaught.load(std::memory_order_acquire)),
             static_cast<unsigned>(edgeCompleteFakeMissSlot != 0),
             static_cast<unsigned>(edgeCompleteFakeMissCaught));
    }
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
    edgeCompleteEnabled = false;
    edgeCompleteFakeMissEnabled = false;
    edgeCompleteDropN = 0;
    edgeCompleteEligibleStores.store(0, std::memory_order_release);
    edgeCompleteCandidateClaimed.store(false, std::memory_order_release);
    edgeCompleteCandidateHolder.store(0, std::memory_order_release);
    edgeCompleteCandidateSlot.store(0, std::memory_order_release);
    edgeCompleteCandidateTarget.store(0, std::memory_order_release);
    edgeCompleteCandidateAttempts.store(0, std::memory_order_release);
    edgeCompleteCandidateRejects = 0;
    edgeCompleteCandidateWaitRuns = 0;
    edgeCompleteDropInjected.store(false, std::memory_order_release);
    edgeCompleteDroppedLine.store(0, std::memory_order_release);
    edgeCompleteDroppedValue = 0;
    edgeCompleteDropCaught.store(false, std::memory_order_release);
    edgeCompleteFakeMissLine = 0;
    edgeCompleteFakeMissSlot = 0;
    edgeCompleteFakeMissTarget = 0;
    edgeCompleteFakeMissCaught = false;
    edgeCompleteRuns = 0;
    edgeCompleteOldObjects = 0;
    edgeCompleteRefFields = 0;
    edgeCompleteEdgesToYoung = 0;
    edgeCompleteInRemset = 0;
    edgeCompleteMissing = 0;
}

bool StickyLog::IsEdgeCompleteDroppedLine(MAddress address) const
{
    MAddress droppedLine = edgeCompleteDroppedLine.load(std::memory_order_acquire);
    return droppedLine != 0 && (address & ~(LINE_SIZE - 1)) == droppedLine;
}

uint8_t StickyLog::GetLoggedByte(MAddress address) const
{
    if (UNLIKELY(address < heapStart || address >= heapStart + heapSize || __cj_sticky_logged_base == nullptr)) {
        return 0;
    }
    size_t lineIndex = (address - heapStart) >> LINE_SHIFT;
    return __atomic_load_n(__cj_sticky_logged_base + lineIndex, __ATOMIC_ACQUIRE);
}

bool StickyLog::IsDirtyRegion(MAddress address) const
{
    if (UNLIKELY(address < heapStart || address >= heapStart + heapSize || dirtyRegionMap == nullptr)) {
        return false;
    }
    size_t regionIndex = (address - heapStart) / RegionInfo::UNIT_SIZE;
    uint8_t* dirtyByte = reinterpret_cast<uint8_t*>(dirtyRegionMap->GetBaseAddr()) + regionIndex / 8;
    uint8_t mask = static_cast<uint8_t>(1U << (regionIndex % 8));
    return (__atomic_load_n(dirtyByte, __ATOMIC_ACQUIRE) & mask) != 0;
}

size_t StickyLog::CountLoggedLinesInCleanRegions() const
{
    if (__cj_sticky_logged_base == nullptr || dirtyRegionMap == nullptr) {
        return 0;
    }
    size_t count = 0;
    for (size_t lineIndex = 0; lineIndex < loggedByteCount; ++lineIndex) {
        if (__atomic_load_n(__cj_sticky_logged_base + lineIndex, __ATOMIC_ACQUIRE) == 0) {
            continue;
        }
        MAddress lineAddress = heapStart + (lineIndex << LINE_SHIFT);
        if (!IsDirtyRegion(lineAddress)) {
            ++count;
        }
    }
    return count;
}

bool StickyLog::IsLoggedLine(MAddress address) const
{
    if (UNLIKELY(IsEdgeCompleteDroppedLine(address))) {
        return false;
    }
    if (UNLIKELY(address < heapStart || address >= heapStart + heapSize || __cj_sticky_logged_base == nullptr)) {
        return false;
    }
    size_t lineIndex = (address - heapStart) >> LINE_SHIFT;
    return *reinterpret_cast<volatile uint8_t*>(__cj_sticky_logged_base + lineIndex) != 0;
}

bool StickyLog::HasPendingEdgeCompleteLine(MAddress rangeStart, MAddress rangeEnd) const
{
    if (rangeStart >= rangeEnd || rangeStart < heapStart || rangeEnd > heapStart + heapSize ||
        __cj_sticky_logged_base == nullptr) {
        return false;
    }
    size_t firstLine = (rangeStart - heapStart) >> LINE_SHIFT;
    size_t lastLine = (rangeEnd - heapStart + LINE_SIZE - 1) >> LINE_SHIFT;
    for (size_t line = firstLine; line < lastLine; ++line) {
        if (__atomic_load_n(__cj_sticky_logged_base + line, __ATOMIC_ACQUIRE) == 1) {
            return true;
        }
    }
    return false;
}

bool StickyLog::TryLogLine(MAddress address, MAddress& lineStart) const
{
    if (UNLIKELY(IsEdgeCompleteDroppedLine(address))) {
        return false;
    }
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

void StickyLog::RecordEdgeCompleteStoreCandidate(BaseObject* holder, MAddress slot, BaseObject* target)
{
    if (LIKELY(edgeCompleteDropN == 0) || holder == nullptr || !Heap::IsHeapAddress(holder) ||
        !Heap::IsHeapAddress(target) || edgeCompleteDropInjected.load(std::memory_order_acquire) ||
        edgeCompleteCandidateAttempts.load(std::memory_order_acquire) >= MAX_DROP_CANDIDATES) {
        return;
    }
    RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    if (holderRegion == nullptr || targetRegion == nullptr || !holderRegion->IsValidRegion() ||
        holderRegion->IsFreeRegion() || holderRegion->IsGarbageRegion() || holderRegion->IsYoungRegion() ||
        !targetRegion->IsValidRegion() || targetRegion->IsFreeRegion() || targetRegion->IsGarbageRegion() ||
        !targetRegion->IsYoungRegion() ||
        IsLoggedLine(reinterpret_cast<MAddress>(holder))) {
        return;
    }
    size_t store = edgeCompleteEligibleStores.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (store < edgeCompleteDropN) {
        return;
    }
    bool expected = false;
    if (!edgeCompleteCandidateClaimed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    size_t attempt = edgeCompleteCandidateAttempts.fetch_add(1, std::memory_order_acq_rel) + 1;
    edgeCompleteCandidateHolder.store(reinterpret_cast<MAddress>(holder), std::memory_order_relaxed);
    edgeCompleteCandidateTarget.store(reinterpret_cast<MAddress>(target), std::memory_order_relaxed);
    edgeCompleteCandidateSlot.store(slot, std::memory_order_release);
    VLOG(REPORT,
         "[EDGECOMPLETE-STWDROP-CANDIDATE] store=%zu attempt=%zu holder=%p slot=%#zx target=%p "
         "holderRegion=%u targetRegion=%u",
         store, attempt, holder, slot, target, static_cast<unsigned>(holderRegion->GetRegionType()),
         static_cast<unsigned>(targetRegion->GetRegionType()));
}

void StickyLog::RejectEdgeCompleteStoreCandidate(size_t run, const char* reason)
{
    ++edgeCompleteCandidateRejects;
    VLOG(REPORT,
         "[EDGECOMPLETE-STWDROP-REJECT] run=%zu attempt=%zu waitRuns=%zu reason=%s",
         run, edgeCompleteCandidateAttempts.load(std::memory_order_acquire), edgeCompleteCandidateWaitRuns, reason);
    edgeCompleteCandidateSlot.store(0, std::memory_order_release);
    edgeCompleteCandidateHolder.store(0, std::memory_order_relaxed);
    edgeCompleteCandidateTarget.store(0, std::memory_order_relaxed);
    edgeCompleteCandidateClaimed.store(false, std::memory_order_release);
}

bool StickyLog::TryDropEdgeCompleteStoreAtSTW(size_t run)
{
    if (edgeCompleteDropN == 0 || edgeCompleteDropInjected.load(std::memory_order_acquire)) {
        return false;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "edge-completeness store drop requires stopped mutators");
    if (edgeCompleteCandidateWaitRuns >= MAX_DROP_WAIT_RUNS) {
        return false;
    }
    ++edgeCompleteCandidateWaitRuns;

    MAddress slot = edgeCompleteCandidateSlot.load(std::memory_order_acquire);
    if (slot == 0) {
        VLOG(REPORT, "[EDGECOMPLETE-STWDROP-WAIT] run=%zu attempts=%zu waitRuns=%zu",
             run, edgeCompleteCandidateAttempts.load(std::memory_order_acquire), edgeCompleteCandidateWaitRuns);
        return false;
    }
    MAddress holderAddress = edgeCompleteCandidateHolder.load(std::memory_order_relaxed);
    MAddress targetAddress = edgeCompleteCandidateTarget.load(std::memory_order_relaxed);
    if (!Heap::IsHeapAddress(reinterpret_cast<BaseObject*>(holderAddress)) ||
        !Heap::IsHeapAddress(reinterpret_cast<BaseObject*>(targetAddress))) {
        RejectEdgeCompleteStoreCandidate(run, "address-outside-heap");
        return false;
    }
    RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(holderAddress);
    RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(targetAddress);
    if (holderRegion == nullptr || !holderRegion->IsValidRegion() || holderRegion->IsFreeRegion() ||
        holderRegion->IsGarbageRegion() || holderRegion->IsYoungRegion()) {
        RejectEdgeCompleteStoreCandidate(run, "holder-region-not-active-old");
        return false;
    }
    if (targetRegion == nullptr || !targetRegion->IsValidRegion() || targetRegion->IsFreeRegion() ||
        targetRegion->IsGarbageRegion() || !targetRegion->IsYoungRegion()) {
        RejectEdgeCompleteStoreCandidate(run, "target-region-not-active-young");
        return false;
    }
    BaseObject* holder = reinterpret_cast<BaseObject*>(holderAddress);
    BaseObject* target = reinterpret_cast<BaseObject*>(targetAddress);
    if (!holder->IsValidObject()) {
        RejectEdgeCompleteStoreCandidate(run, "holder-invalid-header");
        return false;
    }
    if (!target->IsValidObject()) {
        RejectEdgeCompleteStoreCandidate(run, "target-invalid-header");
        return false;
    }
    if (slot < holderRegion->GetRegionStart() || slot + sizeof(RefField<>) > holderRegion->GetRegionAllocPtr()) {
        RejectEdgeCompleteStoreCandidate(run, "slot-outside-holder-region");
        return false;
    }
    RefField<> current(*reinterpret_cast<RefField<>*>(slot));
    if (current.GetTargetObject() != target) {
        RejectEdgeCompleteStoreCandidate(run, "slot-overwritten");
        return false;
    }
    MAddress holderLine = holderAddress & ~(LINE_SIZE - 1);
    if (!IsLoggedLine(holderLine)) {
        RejectEdgeCompleteStoreCandidate(run, "line-not-logged");
        return false;
    }

    size_t lineIndex = (holderLine - heapStart) >> LINE_SHIFT;
    edgeCompleteDroppedValue = __atomic_load_n(__cj_sticky_logged_base + lineIndex, __ATOMIC_ACQUIRE);
    edgeCompleteDroppedLine.store(holderLine, std::memory_order_release);
    __atomic_store_n(__cj_sticky_logged_base + lineIndex, 0, __ATOMIC_RELEASE);
    edgeCompleteDropInjected.store(true, std::memory_order_release);
    VLOG(REPORT,
         "[EDGECOMPLETE-STWDROP] run=%zu attempt=%zu waitRuns=%zu holder=%p slot=%#zx target=%p "
         "holderRegion=%u targetRegion=%u previousLogged=%u",
         run, edgeCompleteCandidateAttempts.load(std::memory_order_acquire), edgeCompleteCandidateWaitRuns,
         holder, slot, target, static_cast<unsigned>(holderRegion->GetRegionType()),
         static_cast<unsigned>(targetRegion->GetRegionType()), static_cast<unsigned>(edgeCompleteDroppedValue));
    return true;
}

bool StickyLog::QueryEdgeCompleteLine(MAddress line, MAddress slot, BaseObject* target)
{
    if (!IsLoggedLine(line)) {
        return false;
    }
    if (!edgeCompleteFakeMissEnabled || edgeCompleteFakeMissSlot != 0) {
        return true;
    }
    edgeCompleteFakeMissLine = line;
    edgeCompleteFakeMissSlot = slot;
    edgeCompleteFakeMissTarget = reinterpret_cast<MAddress>(target);
    VLOG(REPORT, "[EDGECOMPLETE-FAKEMISS-INJECT] line=%#zx slot=%#zx target=%p", line, slot, target);
    return false;
}

bool StickyLog::IsEdgeCompleteDroppedEdge(MAddress slot, BaseObject* target) const
{
    return edgeCompleteDropInjected.load(std::memory_order_acquire) &&
        slot == edgeCompleteCandidateSlot.load(std::memory_order_acquire) &&
        reinterpret_cast<MAddress>(target) == edgeCompleteCandidateTarget.load(std::memory_order_acquire);
}

bool StickyLog::IsEdgeCompleteFakeMissEdge(MAddress slot, BaseObject* target) const
{
    return slot == edgeCompleteFakeMissSlot &&
        reinterpret_cast<MAddress>(target) == edgeCompleteFakeMissTarget;
}

void StickyLog::MarkEdgeCompleteDropCaught()
{
    edgeCompleteDropCaught.store(true, std::memory_order_release);
}

void StickyLog::MarkEdgeCompleteFakeMissCaught()
{
    edgeCompleteFakeMissCaught = true;
}

bool StickyLog::RepairEdgeCompleteDroppedLine()
{
    MAddress droppedLine = edgeCompleteDroppedLine.exchange(0, std::memory_order_acq_rel);
    if (droppedLine == 0) {
        return false;
    }
    size_t lineIndex = (droppedLine - heapStart) >> LINE_SHIFT;
    __atomic_store_n(__cj_sticky_logged_base + lineIndex, edgeCompleteDroppedValue, __ATOMIC_RELEASE);
    return true;
}

void StickyLog::RecordEdgeCompleteRun(size_t oldObjects, size_t refFields, size_t edgesToYoung, size_t inRemset,
                                      size_t missing)
{
    ++edgeCompleteRuns;
    edgeCompleteOldObjects += oldObjects;
    edgeCompleteRefFields += refFields;
    edgeCompleteEdgesToYoung += edgesToYoung;
    edgeCompleteInRemset += inRemset;
    edgeCompleteMissing += missing;
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
