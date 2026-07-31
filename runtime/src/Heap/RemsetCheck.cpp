// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RemsetCheck.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "Allocator/MemMap.h"
#include "Allocator/RegionInfo.h"
#include "Base/Log.h"
#include "Base/Panic.h"
#include "Common/BaseObject.h"
#include "Heap/Heap.h"
#include "Heap/StickyLog.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/SatbBuffer.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {
bool ReadRemsetCheckBoolean(const char* name, bool defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return defaultValue;
    }
    if (std::strcmp(value, "1") == 0) {
        return true;
    }
    if (std::strcmp(value, "0") == 0) {
        return false;
    }
    LOG(RTLOG_ERROR, "Unsupported %s=%s; expected 0 or 1, using default %u", name, value,
        static_cast<unsigned>(defaultValue));
    return defaultValue;
}

const char* HookSiteName(RemsetCheck::HookSite site)
{
    switch (site) {
        case RemsetCheck::HookSite::WRITE_REFERENCE:
            return "WriteReference";
        case RemsetCheck::HookSite::WRITE_STRUCT:
            return "WriteStruct";
        case RemsetCheck::HookSite::ATOMIC_WRITE_REFERENCE:
            return "AtomicWriteReference";
        case RemsetCheck::HookSite::ATOMIC_SWAP_REFERENCE:
            return "AtomicSwapReference";
        case RemsetCheck::HookSite::COMPARE_AND_SWAP_REFERENCE:
            return "CompareAndSwapReference";
        case RemsetCheck::HookSite::COPY_STRUCT_ARRAY:
            return "CopyStructArray";
        case RemsetCheck::HookSite::WRITE_GENERIC:
            return "WriteGeneric";
        case RemsetCheck::HookSite::COUNT:
            break;
    }
    return "unknown";
}
} // namespace

RemsetCheck& RemsetCheck::Instance() noexcept
{
    static RemsetCheck instance;
    return instance;
}

void RemsetCheck::ConfigureFromEnvironment(bool forceSlowPath)
{
    MRT_ASSERT(!configured, "remset check configured twice");
    configured = true;
    enabled = ReadRemsetCheckBoolean("MRT_REMSETCHECK", false);
    hitCountingEnabled = ReadRemsetCheckBoolean("MRT_REMSETCHECK_HITS", enabled);
    detectControlEnabled = enabled && ReadRemsetCheckBoolean("MRT_REMSETCHECK_POSCTRL_DETECT", false);
    produceControlEnabled = enabled && ReadRemsetCheckBoolean("MRT_REMSETCHECK_POSCTRL_PRODUCE", false);
    orphanControlEnabled = enabled && ReadRemsetCheckBoolean("MRT_REMSETCHECK_POSCTRL_ORPHAN", false);
    CHECK_DETAIL(!enabled || forceSlowPath,
                 "MRT_REMSETCHECK=1 requires MRT_STICKY_MINOR_FORCE_SLOW_PATH=1 so every measured write "
                 "reaches IdleLogBarrier");
    LOG(RTLOG_INFO,
        "remset check: enabled=%u hits=%u forceSlowPath=%u detectControl=%u produceControl=%u orphanControl=%u",
        static_cast<unsigned>(enabled), static_cast<unsigned>(hitCountingEnabled),
        static_cast<unsigned>(forceSlowPath), static_cast<unsigned>(detectControlEnabled),
        static_cast<unsigned>(produceControlEnabled), static_cast<unsigned>(orphanControlEnabled));
}

void RemsetCheck::RecordHookHit(HookSite site)
{
    if (LIKELY(!hitCountingEnabled)) {
        return;
    }
    size_t index = static_cast<size_t>(site);
    CHECK(index < HOOK_SITE_COUNT);
    barrierHits.fetch_add(1, std::memory_order_relaxed);
    hookHits[index].fetch_add(1, std::memory_order_relaxed);
}

void RemsetCheck::RecordBarrierEdge(BaseObject* holder, MAddress slot, BaseObject* target, HookSite site)
{
    if (LIKELY(!enabled) || holder == nullptr || target == nullptr || !Heap::IsHeapAddress(holder) ||
        !Heap::IsHeapAddress(slot) || !Heap::IsHeapAddress(target)) {
        return;
    }
    RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    RegionInfo* slotRegion = RegionInfo::TryGetRegionInfoAt(slot);
    RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    if (holderRegion == nullptr || slotRegion != holderRegion || targetRegion == nullptr ||
        !holderRegion->IsValidRegion() || holderRegion->IsFreeRegion() || holderRegion->IsGarbageRegion() ||
        !targetRegion->IsValidRegion() || targetRegion->IsFreeRegion() || targetRegion->IsGarbageRegion() ||
        !targetRegion->IsYoungRegion() || holderRegion == targetRegion) {
        return;
    }
    Edge edge{ reinterpret_cast<MAddress>(holder), slot, reinterpret_cast<MAddress>(target),
               holderRegion->GetRegionStart(), targetRegion->GetRegionStart(), holderRegion->GetIdentityEpoch(),
               targetRegion->GetIdentityEpoch(), site };
    std::lock_guard<std::mutex> lg(edgeMutex);
    edges[slot] = edge;
}

bool RemsetCheck::Revalidate(const Edge& edge, RegionInfo*& holderRegion, RegionInfo*& targetRegion) const
{
    if (!Heap::IsHeapAddress(edge.holder) || !Heap::IsHeapAddress(edge.slot) ||
        !Heap::IsHeapAddress(edge.target)) {
        return false;
    }
    holderRegion = RegionInfo::TryGetRegionInfoAt(edge.holder);
    RegionInfo* slotRegion = RegionInfo::TryGetRegionInfoAt(edge.slot);
    targetRegion = RegionInfo::TryGetRegionInfoAt(edge.target);
    if (holderRegion == nullptr || slotRegion != holderRegion || targetRegion == nullptr ||
        !holderRegion->IsValidRegion() || holderRegion->IsFreeRegion() || holderRegion->IsGarbageRegion() ||
        !targetRegion->IsValidRegion() || targetRegion->IsFreeRegion() || targetRegion->IsGarbageRegion() ||
        holderRegion->GetRegionStart() != edge.holderRegionStart ||
        targetRegion->GetRegionStart() != edge.targetRegionStart ||
        holderRegion->GetIdentityEpoch() != edge.holderRegionEpoch ||
        targetRegion->GetIdentityEpoch() != edge.targetRegionEpoch || edge.holder < holderRegion->GetRegionStart() ||
        edge.holder >= holderRegion->GetRegionAllocPtr() || edge.target < targetRegion->GetRegionStart() ||
        edge.target >= targetRegion->GetRegionAllocPtr() || edge.slot < edge.holder + TYPEINFO_PTR_SIZE ||
        edge.slot + sizeof(MAddress) > holderRegion->GetRegionAllocPtr()) {
        return false;
    }
    BaseObject* holder = reinterpret_cast<BaseObject*>(edge.holder);
    BaseObject* target = reinterpret_cast<BaseObject*>(edge.target);
    if (!holder->IsValidObject() || !target->IsValidObject() || !holder->GetTypeInfo()->IsVaildType() ||
        !target->GetTypeInfo()->IsVaildType()) {
        return false;
    }
    size_t holderSize = holder->GetSize();
    if (edge.slot + sizeof(MAddress) > edge.holder + holderSize) {
        return false;
    }
    MAddress value = __atomic_load_n(reinterpret_cast<MAddress*>(edge.slot), __ATOMIC_ACQUIRE);
    RefField<> current(value);
    return reinterpret_cast<MAddress>(current.GetTargetObject()) == edge.target;
}

uint8_t RemsetCheck::LoadLoggedByte(MAddress line) const
{
    StickyLog& stickyLog = StickyLog::Instance();
    if (line < stickyLog.heapStart || line >= stickyLog.heapStart + stickyLog.heapSize ||
        __cj_sticky_logged_base == nullptr) {
        return 0;
    }
    size_t lineIndex = (line - stickyLog.heapStart) >> StickyLog::LINE_SHIFT;
    return __atomic_load_n(__cj_sticky_logged_base + lineIndex, __ATOMIC_ACQUIRE);
}

uint8_t RemsetCheck::ExchangeLoggedByte(MAddress line, uint8_t value) const
{
    StickyLog& stickyLog = StickyLog::Instance();
    CHECK(line >= stickyLog.heapStart && line < stickyLog.heapStart + stickyLog.heapSize &&
          __cj_sticky_logged_base != nullptr);
    size_t lineIndex = (line - stickyLog.heapStart) >> StickyLog::LINE_SHIFT;
    return __atomic_exchange_n(__cj_sticky_logged_base + lineIndex, value, __ATOMIC_ACQ_REL);
}

bool RemsetCheck::LoadDirtyBit(MAddress holder) const
{
    StickyLog& stickyLog = StickyLog::Instance();
    if (holder < stickyLog.heapStart || holder >= stickyLog.heapStart + stickyLog.heapSize ||
        stickyLog.dirtyRegionMap == nullptr) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(holder);
    if (region == nullptr) {
        return false;
    }
    size_t regionIndex = (region->GetRegionStart() - stickyLog.heapStart) / RegionInfo::UNIT_SIZE;
    uint8_t* dirtyByte = reinterpret_cast<uint8_t*>(stickyLog.dirtyRegionMap->GetBaseAddr()) + regionIndex / 8;
    uint8_t mask = static_cast<uint8_t>(1U << (regionIndex % 8));
    return (__atomic_load_n(dirtyByte, __ATOMIC_ACQUIRE) & mask) != 0;
}

bool RemsetCheck::ExchangeDirtyBit(MAddress holder, bool value) const
{
    StickyLog& stickyLog = StickyLog::Instance();
    CHECK(holder >= stickyLog.heapStart && holder < stickyLog.heapStart + stickyLog.heapSize &&
          stickyLog.dirtyRegionMap != nullptr);
    RegionInfo* region = RegionInfo::GetRegionInfoAt(holder);
    size_t regionIndex = (region->GetRegionStart() - stickyLog.heapStart) / RegionInfo::UNIT_SIZE;
    uint8_t* dirtyByte = reinterpret_cast<uint8_t*>(stickyLog.dirtyRegionMap->GetBaseAddr()) + regionIndex / 8;
    uint8_t mask = static_cast<uint8_t>(1U << (regionIndex % 8));
    uint8_t old = value ? __atomic_fetch_or(dirtyByte, mask, __ATOMIC_ACQ_REL)
                        : __atomic_fetch_and(dirtyByte, static_cast<uint8_t>(~mask), __ATOMIC_ACQ_REL);
    return (old & mask) != 0;
}

void RemsetCheck::CheckRound(size_t run, size_t young)
{
    if (LIKELY(!enabled)) {
        return;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "remset check requires stopped mutators");
    if (young == 0) {
        ++youngZeroExcluded;
        VLOG(REPORT,
             "[REMSETCHECK] run=%zu young=0 excluded=1 edgesFromBarrier=0 revalidateDropped=0 missing=0 "
             "missingPct=0.000000 orphanByteNonzeroDirtyZero=0",
             run);
        return;
    }

    std::vector<MAddress> bufferedLineVector;
    SatbBuffer::Instance().SnapshotStickyLogLines(bufferedLineVector);
    std::unordered_set<MAddress> bufferedLines(bufferedLineVector.begin(), bufferedLineVector.end());
    std::vector<Edge> candidates;
    size_t droppedThisRound = 0;
    {
        std::lock_guard<std::mutex> lg(edgeMutex);
        for (auto it = edges.begin(); it != edges.end();) {
            RegionInfo* holderRegion = nullptr;
            RegionInfo* targetRegion = nullptr;
            if (!Revalidate(it->second, holderRegion, targetRegion)) {
                ++droppedThisRound;
                it = edges.erase(it);
                continue;
            }
            if (!targetRegion->IsYoungRegion()) {
                it = edges.erase(it);
                continue;
            }
            if (!holderRegion->IsYoungRegion()) {
                candidates.push_back(it->second);
            }
            ++it;
        }
    }

    auto recorded = [this, &bufferedLines](const Edge& edge) {
        MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
        return LoadLoggedByte(line) != 0 &&
            (LoadDirtyBit(edge.holder) || bufferedLines.find(line) != bufferedLines.end());
    };
    auto emitControlMiss = [run](const char* control, const Edge& edge) {
        VLOG(REPORT, "[REMSETCHECK-MISS] run=%zu reason=%s holder=%p slot=%p target=%p", run, control,
             reinterpret_cast<void*>(edge.holder), reinterpret_cast<void*>(edge.slot),
             reinterpret_cast<void*>(edge.target));
    };

    if (detectControlEnabled && !detectControlCaught) {
        for (const Edge& edge : candidates) {
            if (!recorded(edge)) {
                continue;
            }
            bool checkerQuery = false;
            if (!checkerQuery) {
                detectControlCaught = true;
                emitControlMiss("detect-control", edge);
                VLOG(REPORT, "[REMSETCHECK-POSCTRL-DETECT] run=%zu caught=1 slot=%p", run,
                     reinterpret_cast<void*>(edge.slot));
            }
            break;
        }
    }

    if (produceControlEnabled && !produceControlCaught) {
        for (const Edge& edge : candidates) {
            if (!recorded(edge)) {
                continue;
            }
            MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
            uint8_t oldByte = ExchangeLoggedByte(line, 0);
            bool checkerQuery = recorded(edge);
            (void)ExchangeLoggedByte(line, oldByte);
            if (!checkerQuery) {
                produceControlCaught = true;
                emitControlMiss("produce-control", edge);
                VLOG(REPORT,
                     "[REMSETCHECK-POSCTRL-PRODUCE] run=%zu caught=1 slot=%p line=%p previousByte=%u restored=1",
                     run, reinterpret_cast<void*>(edge.slot), reinterpret_cast<void*>(line),
                     static_cast<unsigned>(oldByte));
            }
            break;
        }
    }

    if (orphanControlEnabled && !orphanControlCaught) {
        for (const Edge& edge : candidates) {
            if (!recorded(edge)) {
                continue;
            }
            MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
            uint8_t oldByte = ExchangeLoggedByte(line, 1);
            bool oldDirty = ExchangeDirtyBit(edge.holder, false);
            bool counted = LoadLoggedByte(line) != 0 && !LoadDirtyBit(edge.holder);
            (void)ExchangeDirtyBit(edge.holder, oldDirty);
            (void)ExchangeLoggedByte(line, oldByte);
            if (counted) {
                orphanControlCaught = true;
                VLOG(REPORT,
                     "[REMSETCHECK-POSCTRL-ORPHAN] run=%zu caught=1 line=%p previousByte=%u previousDirty=%u "
                     "restored=1",
                     run, reinterpret_cast<void*>(line), static_cast<unsigned>(oldByte),
                     static_cast<unsigned>(oldDirty));
            }
            break;
        }
    }

    std::unordered_set<MAddress> orphanLines;
    std::array<size_t, HOOK_SITE_COUNT> siteEdges{};
    size_t missingThisRound = 0;
    size_t missingSamples = 0;
    for (const Edge& edge : candidates) {
        MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
        if (LoadLoggedByte(line) != 0 && !LoadDirtyBit(edge.holder)) {
            orphanLines.insert(line);
        }
        ++siteEdges[static_cast<size_t>(edge.site)];
        if (!recorded(edge)) {
            ++missingThisRound;
            if (missingSamples < 20) {
                ++missingSamples;
                VLOG(REPORT, "[REMSETCHECK-MISS] run=%zu reason=not-consumable holder=%p slot=%p target=%p",
                     run, reinterpret_cast<void*>(edge.holder), reinterpret_cast<void*>(edge.slot),
                     reinterpret_cast<void*>(edge.target));
            }
        }
    }

    double missingPct = candidates.empty() ? 0.0 :
        static_cast<double>(missingThisRound) * 100.0 / static_cast<double>(candidates.size());
    ++includedRuns;
    edgesFromBarrier += candidates.size();
    revalidateDropped += droppedThisRound;
    missing += missingThisRound;
    orphanByteNonzeroDirtyZero += orphanLines.size();
    VLOG(REPORT,
         "[REMSETCHECK] run=%zu young=%zu excluded=0 edgesFromBarrier=%zu revalidateDropped=%zu missing=%zu "
         "missingPct=%.6f orphanByteNonzeroDirtyZero=%zu bufferedLines=%zu",
         run, young, candidates.size(), droppedThisRound, missingThisRound, missingPct, orphanLines.size(),
         bufferedLines.size());
    for (size_t i = 0; i < HOOK_SITE_COUNT; ++i) {
        if (siteEdges[i] != 0) {
            VLOG(REPORT, "[REMSETCHECK-SITE] run=%zu site=%s edges=%zu", run,
                 HookSiteName(static_cast<HookSite>(i)), siteEdges[i]);
        }
    }
}

void RemsetCheck::Fini()
{
    if (!configured) {
        return;
    }
    if (hitCountingEnabled) {
        VLOG(REPORT,
             "[REMSETCHECK-BARRIER-HITS] total=%zu WriteReference=%zu WriteStruct=%zu AtomicWriteReference=%zu "
             "AtomicSwapReference=%zu CompareAndSwapReference=%zu CopyStructArray=%zu WriteGeneric=%zu",
             barrierHits.load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::WRITE_REFERENCE)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::WRITE_STRUCT)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::ATOMIC_WRITE_REFERENCE)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::ATOMIC_SWAP_REFERENCE)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::COMPARE_AND_SWAP_REFERENCE)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::COPY_STRUCT_ARRAY)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::WRITE_GENERIC)].load(std::memory_order_relaxed));
    }
    if (enabled) {
        double missingPct = edgesFromBarrier == 0 ? 0.0 :
            static_cast<double>(missing) * 100.0 / static_cast<double>(edgesFromBarrier);
        VLOG(REPORT,
             "[REMSETCHECK-FINAL] runs=%zu young0Excluded=%zu edgesFromBarrier=%zu revalidateDropped=%zu "
             "missing=%zu missingPct=%.6f orphanByteNonzeroDirtyZero=%zu detectControlCaught=%u "
             "produceControlCaught=%u orphanControlCaught=%u",
             includedRuns, youngZeroExcluded, edgesFromBarrier, revalidateDropped, missing, missingPct,
             orphanByteNonzeroDirtyZero, static_cast<unsigned>(detectControlCaught),
             static_cast<unsigned>(produceControlCaught), static_cast<unsigned>(orphanControlCaught));
    }
    std::lock_guard<std::mutex> lg(edgeMutex);
    edges.clear();
}
} // namespace MapleRuntime
