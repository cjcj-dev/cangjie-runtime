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
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/SatbBuffer.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {
struct ThreadStickyLogEvent {
    RemsetCheck::StickyLogExit exit = RemsetCheck::StickyLogExit::NO_LOGOBJECT_CALL;
    MAddress heapStart = 0;
    size_t heapSize = 0;
    bool holderInHeapRange = false;
    size_t logObjectCallCount = 0;
};

thread_local ThreadStickyLogEvent threadStickyLogEvent;

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
        case RemsetCheck::HookSite::WRITE_STATIC_REF:
            return "WriteStaticRef";
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

const char* StickyLogExitName(RemsetCheck::StickyLogExit exit)
{
    switch (exit) {
        case RemsetCheck::StickyLogExit::NO_LOGOBJECT_CALL:
            return "NO_LOGOBJECT_CALL";
        case RemsetCheck::StickyLogExit::EXIT_A_NULL_OBJECT:
            return "EXIT_A";
        case RemsetCheck::StickyLogExit::EXIT_B_ALREADY_LOGGED:
            return "EXIT_B";
        case RemsetCheck::StickyLogExit::EXIT_C_NO_MUTATOR:
            return "EXIT_C_NO_MUTATOR";
        case RemsetCheck::StickyLogExit::EXIT_D_OUT_OF_HEAP_RANGE:
            return "EXIT_D_TRYLOG_FALSE_OUT_OF_HEAP_RANGE";
        case RemsetCheck::StickyLogExit::EXIT_D_BASE_NULL:
            return "EXIT_D_TRYLOG_FALSE_BASE_NULL";
        case RemsetCheck::StickyLogExit::EXIT_D_OTHER:
            return "EXIT_D_TRYLOG_FALSE_OTHER";
        case RemsetCheck::StickyLogExit::LOGGED:
            return "LOGGED";
        case RemsetCheck::StickyLogExit::COUNT:
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
    stickyLogExitControlEnabled = enabled && ReadRemsetCheckBoolean("MRT_BYTE0ROOT_POSCTRL", false);
    falseUnvisitedControlEnabled =
        enabled && ReadRemsetCheckBoolean("MRT_VISITORHOOK_POSCTRL_FALSE_UNVISITED", false);
    trueUnvisitedControlEnabled =
        enabled && ReadRemsetCheckBoolean("MRT_VISITORHOOK_POSCTRL_TRUE_UNVISITED", false);
    CHECK_DETAIL(!enabled || forceSlowPath,
                 "MRT_REMSETCHECK=1 requires MRT_STICKY_MINOR_FORCE_SLOW_PATH=1 so every measured write "
                 "reaches IdleLogBarrier");
    LOG(RTLOG_INFO,
        "remset check: enabled=%u hits=%u forceSlowPath=%u detectControl=%u produceControl=%u orphanControl=%u "
        "stickyLogExitControl=%u falseUnvisitedControl=%u trueUnvisitedControl=%u",
        static_cast<unsigned>(enabled), static_cast<unsigned>(hitCountingEnabled),
        static_cast<unsigned>(forceSlowPath), static_cast<unsigned>(detectControlEnabled),
        static_cast<unsigned>(produceControlEnabled), static_cast<unsigned>(orphanControlEnabled),
        static_cast<unsigned>(stickyLogExitControlEnabled), static_cast<unsigned>(falseUnvisitedControlEnabled),
        static_cast<unsigned>(trueUnvisitedControlEnabled));
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
    if (enabled) {
        threadStickyLogEvent.exit = StickyLogExit::NO_LOGOBJECT_CALL;
        threadStickyLogEvent.heapStart = 0;
        threadStickyLogEvent.heapSize = 0;
        threadStickyLogEvent.holderInHeapRange = false;
    }
}

void RemsetCheck::BeginLogObject()
{
    if (LIKELY(!enabled)) {
        return;
    }
    ++threadStickyLogEvent.logObjectCallCount;
    threadStickyLogEvent.exit = StickyLogExit::NO_LOGOBJECT_CALL;
    threadStickyLogEvent.heapStart = 0;
    threadStickyLogEvent.heapSize = 0;
    threadStickyLogEvent.holderInHeapRange = false;
}

void RemsetCheck::RecordStickyLogExit(StickyLogExit exit, BaseObject* object, MAddress heapStart, size_t heapSize)
{
    if (LIKELY(!enabled)) {
        return;
    }
    size_t index = static_cast<size_t>(exit);
    CHECK(index < STICKY_LOG_EXIT_COUNT);
    MAddress address = reinterpret_cast<MAddress>(object);
    threadStickyLogEvent.exit = exit;
    threadStickyLogEvent.heapStart = heapStart;
    threadStickyLogEvent.heapSize = heapSize;
    threadStickyLogEvent.holderInHeapRange = object != nullptr && address >= heapStart &&
        address - heapStart < heapSize;
    size_t previous = stickyLogExitHits[index].fetch_add(1, std::memory_order_relaxed);
    if (previous == 0) {
        VLOG(REPORT, "[BYTE0ROOT-EXIT-FIRST] exit=%s object=%p heapStart=%p heapSize=%zu inHeapRange=%u",
             StickyLogExitName(exit), object, reinterpret_cast<void*>(heapStart), heapSize,
             static_cast<unsigned>(threadStickyLogEvent.holderInHeapRange));
    }
}

void RemsetCheck::RecordNoLogObjectCall(HookSite site)
{
    if (LIKELY(!enabled)) {
        return;
    }
    size_t index = static_cast<size_t>(site);
    CHECK(index < HOOK_SITE_COUNT);
    size_t previous = noLogObjectCallHits[index].fetch_add(1, std::memory_order_relaxed);
    if (previous == 0) {
        VLOG(REPORT, "[BYTE0ROOT-NO-LOGOBJECT-FIRST] site=%s", HookSiteName(site));
    }
}

size_t RemsetCheck::GetThreadLogObjectCallCount() const
{
    return threadStickyLogEvent.logObjectCallCount;
}

void RemsetCheck::RunStickyLogExitPositiveControls(BaseObject* object)
{
    if (LIKELY(!stickyLogExitControlEnabled) || object == nullptr || Mutator::GetMutator() == nullptr) {
        return;
    }
    bool expected = false;
    if (!stickyLogExitControlStarted.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        return;
    }
    StickyLog& stickyLog = StickyLog::Instance();
    BeginLogObject();
    CJ_MCC_StickyLogLine(nullptr);
    StickyLogExit exitA = threadStickyLogEvent.exit;
    BeginLogObject();
    CJ_MCC_StickyLogLine(reinterpret_cast<BaseObject*>(stickyLog.heapStart - 1));
    StickyLogExit exitDOutOfRange = threadStickyLogEvent.exit;
    BeginLogObject();
    CJ_MCC_StickyLogLine(object);
    StickyLogExit firstObjectExit = threadStickyLogEvent.exit;
    BeginLogObject();
    CJ_MCC_StickyLogLine(object);
    StickyLogExit exitB = threadStickyLogEvent.exit;
    VLOG(REPORT, "[BYTE0ROOT-POSCTRL] A=%s D_OUT=%s FIRST_OBJECT=%s B=%s",
         StickyLogExitName(exitA), StickyLogExitName(exitDOutOfRange), StickyLogExitName(firstObjectExit),
         StickyLogExitName(exitB));
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
               targetRegion->GetIdentityEpoch(), static_cast<uint8_t>(holderRegion->GetRegionType()), site,
               threadStickyLogEvent.exit, threadStickyLogEvent.heapStart, threadStickyLogEvent.heapSize,
               threadStickyLogEvent.holderInHeapRange };
    std::lock_guard<std::mutex> lg(edgeMutex);
    edges[slot] = edge;
}

void RemsetCheck::RecordMajor(size_t completedMinorRuns, size_t minorRunsSinceMajor)
{
    if (LIKELY(!enabled)) {
        return;
    }
    ++majorCount;
    VLOG(REPORT, "[REMSETCHECK-PHASE] event=major completedMinorRuns=%zu minorRunsSinceMajor=%zu total=%zu",
         completedMinorRuns, minorRunsSinceMajor, majorCount);
}

void RemsetCheck::RecordBeginEpoch(size_t completedMinorRuns)
{
    if (LIKELY(!enabled)) {
        return;
    }
    ++beginEpochCount;
    VLOG(REPORT, "[REMSETCHECK-PHASE] event=BeginEpoch completedMinorRuns=%zu total=%zu", completedMinorRuns,
         beginEpochCount);
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
    CHECK(!visitedRoundActive);
    visitedRoundRun = run;
    visitedRoundOldMissing = 0;
    visitedRoundCandidates.clear();
    visitedLines.clear();
    retain2SkippedLines.clear();
    for (size_t i = 0; i < VISITOR_HOOK_SITE_COUNT; ++i) {
        visitedRoundLineHits[i] = 0;
    }
    retain2SkippedThisRound = 0;
    if (young == 0) {
        ++youngZeroExcluded;
        VLOG(REPORT,
             "[REMSETCHECK] run=%zu young=0 excluded=1 edgesFromBarrier=0 revalidateDropped=0 missing=0 "
             "missingPct=0.000000 orphanByteNonzeroDirtyZero=0",
             run);
        return;
    }
    visitedRoundActive = true;

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
    auto checkerRecorded = [&recorded](const Edge& edge, MAddress suppressedSlot) {
        return edge.slot != suppressedSlot && recorded(edge);
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
            bool checkerQuery = checkerRecorded(edge, edge.slot);
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
    size_t missingByteZeroThisRound = 0;
    size_t missingOrphanThisRound = 0;
    size_t missingDirtyNotBufferedThisRound = 0;
    size_t missingOtherThisRound = 0;
    std::array<size_t, STICKY_LOG_EXIT_COUNT> missingStickyLogExitsThisRound{};
    size_t missingHolderInHeapRangeThisRound = 0;
    size_t missingHolderOutOfHeapRangeThisRound = 0;
    for (const Edge& edge : candidates) {
        MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
        uint8_t byte = LoadLoggedByte(line);
        bool dirty = LoadDirtyBit(edge.holder);
        bool buffered = bufferedLines.find(line) != bufferedLines.end();
        if (byte != 0 && !dirty) {
            orphanLines.insert(line);
        }
        ++siteEdges[static_cast<size_t>(edge.site)];
        if (!checkerRecorded(edge, 0)) {
            ++missingThisRound;
            ++missingStickyLogExitsThisRound[static_cast<size_t>(edge.stickyLogExit)];
            if (edge.holderInHeapRangeAtLog) {
                ++missingHolderInHeapRangeThisRound;
            } else {
                ++missingHolderOutOfHeapRangeThisRound;
            }
            if (byte == 0) {
                ++missingByteZeroThisRound;
            } else if (!dirty) {
                ++missingOrphanThisRound;
            } else if (!buffered) {
                ++missingDirtyNotBufferedThisRound;
            } else {
                ++missingOtherThisRound;
            }
            RegionInfo* holderRegion = RegionInfo::GetRegionInfoAt(edge.holder);
            VLOG(REPORT,
                 "[REMSETCHECK-MISS] run=%zu reason=not-consumable holder=%p slot=%p target=%p line=%p "
                 "byte=%u dirty=%u buffered=%u holderRegion=%p holderRegionTypeAtRecord=%u "
                 "holderRegionTypeAtCheck=%u holderRegionEpochAtRecord=%llu holderRegionEpochAtCheck=%llu "
                 "site=%s stickyLogExit=%s logHeapStart=%p logHeapSize=%zu holderInHeapRangeAtLog=%u",
                 run, reinterpret_cast<void*>(edge.holder), reinterpret_cast<void*>(edge.slot),
                 reinterpret_cast<void*>(edge.target), reinterpret_cast<void*>(line), static_cast<unsigned>(byte),
                 static_cast<unsigned>(dirty), static_cast<unsigned>(buffered),
                 reinterpret_cast<void*>(holderRegion->GetRegionStart()),
                 static_cast<unsigned>(edge.holderRegionType),
                 static_cast<unsigned>(holderRegion->GetRegionType()),
                 static_cast<unsigned long long>(edge.holderRegionEpoch),
                 static_cast<unsigned long long>(holderRegion->GetIdentityEpoch()), HookSiteName(edge.site),
                 StickyLogExitName(edge.stickyLogExit), reinterpret_cast<void*>(edge.logHeapStart), edge.logHeapSize,
                 static_cast<unsigned>(edge.holderInHeapRangeAtLog));
        }
    }
    visitedRoundCandidates = candidates;
    visitedRoundOldMissing = missingThisRound;

    StickyLog& stickyLog = StickyLog::Instance();
    size_t validRegionCount = 0;
    size_t recentFullRegionCount = 0;
    for (MAddress address = stickyLog.heapStart; address < stickyLog.heapStart + stickyLog.heapSize;
         address += RegionInfo::UNIT_SIZE) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(address);
        if (region == nullptr || !region->IsValidRegion() || region->IsFreeRegion() ||
            region->GetRegionStart() != address) {
            continue;
        }
        ++validRegionCount;
        if (region->GetRegionType() == RegionInfo::RegionType::RECENT_FULL_REGION) {
            ++recentFullRegionCount;
        }
    }
    VLOG(REPORT,
         "[BYTE0ROOT-ROUND] run=%zu heapStart=%p heapSize=%zu validRegions=%zu recentFullRegions=%zu "
         "holderInHeapRange=%zu holderOutOfHeapRange=%zu",
         run, reinterpret_cast<void*>(stickyLog.heapStart), stickyLog.heapSize, validRegionCount,
         recentFullRegionCount, missingHolderInHeapRangeThisRound, missingHolderOutOfHeapRangeThisRound);
    for (size_t i = 0; i < STICKY_LOG_EXIT_COUNT; ++i) {
        if (missingStickyLogExitsThisRound[i] != 0) {
            VLOG(REPORT, "[BYTE0ROOT-EXIT] run=%zu exit=%s missing=%zu", run,
                 StickyLogExitName(static_cast<StickyLogExit>(i)), missingStickyLogExitsThisRound[i]);
        }
    }

    double missingPct = candidates.empty() ? 0.0 :
        static_cast<double>(missingThisRound) * 100.0 / static_cast<double>(candidates.size());
    ++includedRuns;
    edgesFromBarrier += candidates.size();
    revalidateDropped += droppedThisRound;
    missing += missingThisRound;
    missingByteZero += missingByteZeroThisRound;
    missingOrphan += missingOrphanThisRound;
    missingDirtyNotBuffered += missingDirtyNotBufferedThisRound;
    missingOther += missingOtherThisRound;
    missingHolderInHeapRange += missingHolderInHeapRangeThisRound;
    missingHolderOutOfHeapRange += missingHolderOutOfHeapRangeThisRound;
    for (size_t i = 0; i < STICKY_LOG_EXIT_COUNT; ++i) {
        missingStickyLogExits[i] += missingStickyLogExitsThisRound[i];
    }
    orphanByteNonzeroDirtyZero += orphanLines.size();
    VLOG(REPORT,
         "[REMSETCHECK] run=%zu young=%zu excluded=0 edgesFromBarrier=%zu revalidateDropped=%zu missing=%zu "
         "missingPct=%.6f orphanByteNonzeroDirtyZero=%zu bufferedLines=%zu missingByteZero=%zu "
         "missingOrphan=%zu missingDirtyNotBuffered=%zu missingOther=%zu majorsSincePreviousMinor=%zu "
         "beginEpochsSincePreviousMinor=%zu",
         run, young, candidates.size(), droppedThisRound, missingThisRound, missingPct, orphanLines.size(),
         bufferedLines.size(), missingByteZeroThisRound, missingOrphanThisRound,
         missingDirtyNotBufferedThisRound, missingOtherThisRound, majorCount - majorCountAtPreviousMinor,
         beginEpochCount - beginEpochCountAtPreviousMinor);
    majorCountAtPreviousMinor = majorCount;
    beginEpochCountAtPreviousMinor = beginEpochCount;
    for (size_t i = 0; i < HOOK_SITE_COUNT; ++i) {
        if (siteEdges[i] != 0) {
            VLOG(REPORT, "[REMSETCHECK-SITE] run=%zu site=%s edges=%zu", run,
                 HookSiteName(static_cast<HookSite>(i)), siteEdges[i]);
        }
    }
}

void RemsetCheck::RecordVisitedLine(MAddress lineStart, VisitorHookSite site)
{
    if (LIKELY(!enabled) || !visitedRoundActive) {
        return;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "visitor line recording requires stopped mutators");
    CHECK((lineStart & (StickyLog::LINE_SIZE - 1)) == 0);
    size_t index = static_cast<size_t>(site);
    CHECK(index < VISITOR_HOOK_SITE_COUNT);
    ++visitedLineHits[index];
    ++visitedRoundLineHits[index];
    visitedLines.insert(lineStart);
}

void RemsetCheck::RecordRetain2SkippedLine(MAddress lineStart)
{
    if (LIKELY(!enabled) || !visitedRoundActive) {
        return;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "retained line recording requires stopped mutators");
    CHECK((lineStart & (StickyLog::LINE_SIZE - 1)) == 0);
    ++retain2Skipped;
    ++retain2SkippedThisRound;
    retain2SkippedLines.insert(lineStart);
}

void RemsetCheck::CheckVisitedRound(size_t run)
{
    if (LIKELY(!enabled) || !visitedRoundActive) {
        return;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "visitor line check requires stopped mutators");
    CHECK(run == visitedRoundRun);

    auto visited = [this](const Edge& edge) {
        MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
        return visitedLines.find(line) != visitedLines.end();
    };
    auto checkerVisited = [&visited](const Edge& edge, MAddress suppressedSlot) {
        return edge.slot != suppressedSlot && visited(edge);
    };

    if (falseUnvisitedControlEnabled && !falseUnvisitedControlCaught) {
        for (const Edge& edge : visitedRoundCandidates) {
            if (!visited(edge)) {
                continue;
            }
            if (!checkerVisited(edge, edge.slot)) {
                falseUnvisitedControlCaught = true;
                VLOG(REPORT,
                     "[VISITORHOOK-MISS] run=%zu reason=false-unvisited-control holder=%p slot=%p target=%p",
                     run, reinterpret_cast<void*>(edge.holder), reinterpret_cast<void*>(edge.slot),
                     reinterpret_cast<void*>(edge.target));
                VLOG(REPORT,
                     "[VISITORHOOK-POSCTRL-FALSE-UNVISITED] run=%zu caught=1 slot=%p exactEdges=1",
                     run, reinterpret_cast<void*>(edge.slot));
            }
            break;
        }
    }

    if (trueUnvisitedControlEnabled && !trueUnvisitedControlCaught) {
        for (const Edge& edge : visitedRoundCandidates) {
            MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
            if (visited(edge) || retain2SkippedLines.find(line) == retain2SkippedLines.end()) {
                continue;
            }
            if (!checkerVisited(edge, 0)) {
                trueUnvisitedControlCaught = true;
                VLOG(REPORT,
                     "[VISITORHOOK-MISS] run=%zu reason=true-unvisited-retain2 holder=%p slot=%p target=%p line=%p",
                     run, reinterpret_cast<void*>(edge.holder), reinterpret_cast<void*>(edge.slot),
                     reinterpret_cast<void*>(edge.target), reinterpret_cast<void*>(line));
                VLOG(REPORT,
                     "[VISITORHOOK-POSCTRL-TRUE-UNVISITED] run=%zu caught=1 slot=%p line=%p retain2Skipped=1",
                     run, reinterpret_cast<void*>(edge.slot), reinterpret_cast<void*>(line));
            }
            break;
        }
    }

    size_t missingNewThisRound = 0;
    for (const Edge& edge : visitedRoundCandidates) {
        if (checkerVisited(edge, 0)) {
            continue;
        }
        ++missingNewThisRound;
        MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
        bool skippedByRetain2 = retain2SkippedLines.find(line) != retain2SkippedLines.end();
        VLOG(REPORT,
             "[VISITORHOOK-MISS] run=%zu reason=unvisited holder=%p slot=%p target=%p line=%p "
             "skippedByRetain2=%u site=%s",
             run, reinterpret_cast<void*>(edge.holder), reinterpret_cast<void*>(edge.slot),
             reinterpret_cast<void*>(edge.target), reinterpret_cast<void*>(line),
             static_cast<unsigned>(skippedByRetain2), HookSiteName(edge.site));
    }
    missingNewPredicate += missingNewThisRound;
    long long delta = static_cast<long long>(visitedRoundOldMissing) - static_cast<long long>(missingNewThisRound);
    VLOG(REPORT,
         "[VISITORHOOK] run=%zu edgesFromBarrier=%zu missingOldPredicate=%zu missingNewPredicate=%zu "
         "deltaOldMinusNew=%lld bufferVisitorCalls=%zu dirtyRegionVisitorCalls=%zu uniqueVisitedLines=%zu "
         "retain2Skipped=%zu uniqueRetain2SkippedLines=%zu",
         run, visitedRoundCandidates.size(), visitedRoundOldMissing, missingNewThisRound, delta,
         visitedRoundLineHits[static_cast<size_t>(VisitorHookSite::BUFFER)],
         visitedRoundLineHits[static_cast<size_t>(VisitorHookSite::DIRTY_REGION)], visitedLines.size(),
         retain2SkippedThisRound, retain2SkippedLines.size());

    visitedRoundActive = false;
    visitedRoundCandidates.clear();
    visitedLines.clear();
    retain2SkippedLines.clear();
}

void RemsetCheck::Fini()
{
    if (!configured) {
        return;
    }
    if (hitCountingEnabled) {
        VLOG(REPORT,
             "[REMSETCHECK-BARRIER-HITS] total=%zu WriteReference=%zu WriteStruct=%zu AtomicWriteReference=%zu "
             "WriteStaticRef=%zu AtomicSwapReference=%zu CompareAndSwapReference=%zu CopyStructArray=%zu "
             "WriteGeneric=%zu",
             barrierHits.load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::WRITE_REFERENCE)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::WRITE_STATIC_REF)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::WRITE_STRUCT)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::ATOMIC_WRITE_REFERENCE)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::ATOMIC_SWAP_REFERENCE)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::COMPARE_AND_SWAP_REFERENCE)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::COPY_STRUCT_ARRAY)].load(std::memory_order_relaxed),
             hookHits[static_cast<size_t>(HookSite::WRITE_GENERIC)].load(std::memory_order_relaxed));
        VLOG(REPORT,
             "[BYTE0ROOT-ALL-EXITS] A=%zu B=%zu C_NO_MUTATOR=%zu D_OUT_OF_HEAP_RANGE=%zu D_BASE_NULL=%zu "
             "D_OTHER=%zu LOGGED=%zu",
             stickyLogExitHits[static_cast<size_t>(StickyLogExit::EXIT_A_NULL_OBJECT)].load(
                 std::memory_order_relaxed),
             stickyLogExitHits[static_cast<size_t>(StickyLogExit::EXIT_B_ALREADY_LOGGED)].load(
                 std::memory_order_relaxed),
             stickyLogExitHits[static_cast<size_t>(StickyLogExit::EXIT_C_NO_MUTATOR)].load(
                 std::memory_order_relaxed),
             stickyLogExitHits[static_cast<size_t>(StickyLogExit::EXIT_D_OUT_OF_HEAP_RANGE)].load(
                 std::memory_order_relaxed),
             stickyLogExitHits[static_cast<size_t>(StickyLogExit::EXIT_D_BASE_NULL)].load(
                 std::memory_order_relaxed),
             stickyLogExitHits[static_cast<size_t>(StickyLogExit::EXIT_D_OTHER)].load(
                 std::memory_order_relaxed),
             stickyLogExitHits[static_cast<size_t>(StickyLogExit::LOGGED)].load(std::memory_order_relaxed));
        VLOG(REPORT,
             "[BYTE0ROOT-NO-LOGOBJECT] WriteReference=%zu WriteStaticRef=%zu WriteStruct=%zu "
             "AtomicWriteReference=%zu AtomicSwapReference=%zu CompareAndSwapReference=%zu "
             "CopyStructArray=%zu WriteGeneric=%zu",
             noLogObjectCallHits[static_cast<size_t>(HookSite::WRITE_REFERENCE)].load(std::memory_order_relaxed),
             noLogObjectCallHits[static_cast<size_t>(HookSite::WRITE_STATIC_REF)].load(std::memory_order_relaxed),
             noLogObjectCallHits[static_cast<size_t>(HookSite::WRITE_STRUCT)].load(std::memory_order_relaxed),
             noLogObjectCallHits[static_cast<size_t>(HookSite::ATOMIC_WRITE_REFERENCE)].load(
                 std::memory_order_relaxed),
             noLogObjectCallHits[static_cast<size_t>(HookSite::ATOMIC_SWAP_REFERENCE)].load(
                 std::memory_order_relaxed),
             noLogObjectCallHits[static_cast<size_t>(HookSite::COMPARE_AND_SWAP_REFERENCE)].load(
                 std::memory_order_relaxed),
             noLogObjectCallHits[static_cast<size_t>(HookSite::COPY_STRUCT_ARRAY)].load(
                 std::memory_order_relaxed),
             noLogObjectCallHits[static_cast<size_t>(HookSite::WRITE_GENERIC)].load(std::memory_order_relaxed));
    }
    if (enabled) {
        double missingPct = edgesFromBarrier == 0 ? 0.0 :
            static_cast<double>(missing) * 100.0 / static_cast<double>(edgesFromBarrier);
        VLOG(REPORT,
             "[REMSETCHECK-FINAL] runs=%zu young0Excluded=%zu edgesFromBarrier=%zu revalidateDropped=%zu "
             "missing=%zu missingPct=%.6f orphanByteNonzeroDirtyZero=%zu detectControlCaught=%u "
             "produceControlCaught=%u orphanControlCaught=%u missingByteZero=%zu missingOrphan=%zu "
             "missingDirtyNotBuffered=%zu missingOther=%zu majors=%zu beginEpochs=%zu",
             includedRuns, youngZeroExcluded, edgesFromBarrier, revalidateDropped, missing, missingPct,
             orphanByteNonzeroDirtyZero, static_cast<unsigned>(detectControlCaught),
             static_cast<unsigned>(produceControlCaught), static_cast<unsigned>(orphanControlCaught),
             missingByteZero, missingOrphan, missingDirtyNotBuffered, missingOther, majorCount, beginEpochCount);
        VLOG(REPORT,
             "[BYTE0ROOT-FINAL] A=%zu B=%zu C_NO_MUTATOR=%zu D_OUT_OF_HEAP_RANGE=%zu D_BASE_NULL=%zu "
             "D_OTHER=%zu NO_LOGOBJECT_CALL=%zu LOGGED=%zu HOLDER_IN_HEAP_RANGE=%zu "
             "HOLDER_OUT_OF_HEAP_RANGE=%zu",
             missingStickyLogExits[static_cast<size_t>(StickyLogExit::EXIT_A_NULL_OBJECT)],
             missingStickyLogExits[static_cast<size_t>(StickyLogExit::EXIT_B_ALREADY_LOGGED)],
             missingStickyLogExits[static_cast<size_t>(StickyLogExit::EXIT_C_NO_MUTATOR)],
             missingStickyLogExits[static_cast<size_t>(StickyLogExit::EXIT_D_OUT_OF_HEAP_RANGE)],
             missingStickyLogExits[static_cast<size_t>(StickyLogExit::EXIT_D_BASE_NULL)],
             missingStickyLogExits[static_cast<size_t>(StickyLogExit::EXIT_D_OTHER)],
             missingStickyLogExits[static_cast<size_t>(StickyLogExit::NO_LOGOBJECT_CALL)],
             missingStickyLogExits[static_cast<size_t>(StickyLogExit::LOGGED)], missingHolderInHeapRange,
             missingHolderOutOfHeapRange);
    }
    std::lock_guard<std::mutex> lg(edgeMutex);
    edges.clear();
}
} // namespace MapleRuntime
