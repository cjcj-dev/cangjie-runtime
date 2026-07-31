// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RemsetCheck.h"

#include <algorithm>
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
    RemsetCheck::HookSite hookSite = RemsetCheck::HookSite::COUNT;
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

const char* DirtyLinePathName(RemsetCheck::DirtyLinePath path)
{
    switch (path) {
        case RemsetCheck::DirtyLinePath::NONE:
            return "none";
        case RemsetCheck::DirtyLinePath::RETAIN2_SKIP:
            return "retain2-skip";
        case RemsetCheck::DirtyLinePath::VISITED:
            return "visited";
        case RemsetCheck::DirtyLinePath::BYTE0_CONTINUE:
            return "byte0-continue";
    }
    return "unknown";
}

const char* LogLineSourceName(RemsetCheck::LogLineSource source)
{
    switch (source) {
        case RemsetCheck::LogLineSource::BARRIER:
            return "barrier";
        case RemsetCheck::LogLineSource::DEFERRED_LOG_RING:
            return "deferred-log-ring";
        case RemsetCheck::LogLineSource::PROMOTION:
            return "promotion";
    }
    return "unknown";
}

const char* ClearWhenEventKindName(RemsetCheck::ClearWhenEventKind kind)
{
    switch (kind) {
        case RemsetCheck::ClearWhenEventKind::LOG_LINE:
            return "log-line";
        case RemsetCheck::ClearWhenEventKind::BARRIER_WRITE:
            return "barrier-write";
        case RemsetCheck::ClearWhenEventKind::CONSUME_START:
            return "consume-start";
        case RemsetCheck::ClearWhenEventKind::RESCAN_BUFFER_WRITE:
            return "RescanLoggedLines-buffer";
        case RemsetCheck::ClearWhenEventKind::RESCAN_DIRTY_WRITE:
            return "RescanLoggedLines-dirty-line";
        case RemsetCheck::ClearWhenEventKind::RESCAN_DIRTY_CLEAR:
            return "RescanLoggedLines-dirty-region-retained-false";
        case RemsetCheck::ClearWhenEventKind::CLEAR_UNAVAILABLE_REGION:
            return "ClearUnavailableRegion";
        case RemsetCheck::ClearWhenEventKind::BEGIN_EPOCH:
            return "BeginEpoch";
        case RemsetCheck::ClearWhenEventKind::POSITIVE_CONTROL_CLEAR:
            return "positive-control-clear";
        case RemsetCheck::ClearWhenEventKind::POSITIVE_CONTROL_RESTORE:
            return "positive-control-restore";
    }
    return "unknown";
}

const char* ClearedByName(RemsetCheck::ClearedBy clearedBy)
{
    switch (clearedBy) {
        case RemsetCheck::ClearedBy::RESCAN_BUFFER:
            return "RescanLoggedLines-buffer";
        case RemsetCheck::ClearedBy::RESCAN_DIRTY_REGION:
            return "RescanLoggedLines-dirty-region";
        case RemsetCheck::ClearedBy::CLEAR_UNAVAILABLE_REGION:
            return "ClearUnavailableRegion";
        case RemsetCheck::ClearedBy::BEGIN_EPOCH:
            return "BeginEpoch";
        case RemsetCheck::ClearedBy::POSITIVE_CONTROL:
            return "positive-control";
        case RemsetCheck::ClearedBy::NOT_OBSERVED:
            return "not-observed";
        case RemsetCheck::ClearedBy::COUNT:
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
    retain2SkipControlEnabled = enabled && ReadRemsetCheckBoolean("MRT_RETAIN2SKIP_POSCTRL", false);
    clearWhenControlEnabled = enabled && ReadRemsetCheckBoolean("MRT_CLEARWHEN_POSCTRL", false);
    CHECK_DETAIL(!enabled || forceSlowPath,
                 "MRT_REMSETCHECK=1 requires MRT_STICKY_MINOR_FORCE_SLOW_PATH=1 so every measured write "
                 "reaches IdleLogBarrier");
    LOG(RTLOG_INFO,
        "remset check: enabled=%u hits=%u forceSlowPath=%u detectControl=%u produceControl=%u orphanControl=%u "
        "stickyLogExitControl=%u falseUnvisitedControl=%u trueUnvisitedControl=%u retain2SkipControl=%u "
        "clearWhenControl=%u",
        static_cast<unsigned>(enabled), static_cast<unsigned>(hitCountingEnabled),
        static_cast<unsigned>(forceSlowPath), static_cast<unsigned>(detectControlEnabled),
        static_cast<unsigned>(produceControlEnabled), static_cast<unsigned>(orphanControlEnabled),
        static_cast<unsigned>(stickyLogExitControlEnabled), static_cast<unsigned>(falseUnvisitedControlEnabled),
        static_cast<unsigned>(trueUnvisitedControlEnabled), static_cast<unsigned>(retain2SkipControlEnabled),
        static_cast<unsigned>(clearWhenControlEnabled));
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
        threadStickyLogEvent.hookSite = site;
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

void RemsetCheck::RecordLoggedLineWrite(MAddress lineStart, LogLineSource source, bool dirtyBefore)
{
    if (LIKELY(!enabled)) {
        return;
    }
    CHECK((lineStart & (StickyLog::LINE_SIZE - 1)) == 0);
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(lineStart);
    if (region == nullptr) {
        return;
    }
    HookSite hookSite = source == LogLineSource::BARRIER ? threadStickyLogEvent.hookSite : HookSite::COUNT;
    StickyLogExit exit = source == LogLineSource::BARRIER ? StickyLogExit::LOGGED : StickyLogExit::COUNT;
    ClearWhenPendingEvent event;
    event.sequence = ReserveClearWhenSequence();
    event.run = GetClearWhenRun();
    event.kind = ClearWhenEventKind::LOG_LINE;
    event.line = lineStart;
    event.regionStart = region->GetRegionStart();
    event.regionSize = 0;
    event.slot = 0;
    event.byteBefore = 0;
    event.byteAfter = 1;
    event.dirtyBefore = dirtyBefore;
    event.dirtyAfter = true;
    event.hookSite = hookSite;
    event.stickyLogExit = exit;
    event.logLineSource = source;
    (void)RecordPendingClearWhenEvent(event);
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
               threadStickyLogEvent.holderInHeapRange, 0 };
    MAddress line = reinterpret_cast<MAddress>(holder) & ~(StickyLog::LINE_SIZE - 1);
    uint8_t byte = LoadLoggedByte(line);
    bool dirty = LoadDirtyBit(reinterpret_cast<MAddress>(holder));
    ClearWhenPendingEvent event;
    event.sequence = ReserveClearWhenSequence();
    event.run = GetClearWhenRun();
    event.kind = ClearWhenEventKind::BARRIER_WRITE;
    event.line = line;
    event.regionStart = holderRegion->GetRegionStart();
    event.regionSize = 0;
    event.slot = slot;
    event.byteBefore = byte;
    event.byteAfter = byte;
    event.dirtyBefore = dirty;
    event.dirtyAfter = dirty;
    event.hookSite = site;
    event.stickyLogExit = edge.stickyLogExit;
    event.logLineSource = LogLineSource::BARRIER;
    edge.writeSequence = RecordPendingClearWhenEvent(event);
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

uint64_t RemsetCheck::AppendClearWhenEventLocked(MAddress line, MAddress regionStart, ClearWhenEventKind kind,
                                                  uint8_t byteBefore, uint8_t byteAfter, bool dirtyBefore,
                                                  bool dirtyAfter, HookSite hookSite, StickyLogExit stickyLogExit,
                                                  LogLineSource logLineSource, MAddress slot, uint64_t sequence,
                                                  size_t eventRun)
{
    if (sequence == 0) {
        sequence = ReserveClearWhenSequence();
    }
    if (eventRun == 0) {
        eventRun = GetClearWhenRun();
    }
    auto inserted = lineTimelines.emplace(
        line, LineTimeline{ regionStart, eventRun, byteBefore, dirtyBefore, {} });
    LineTimeline& timeline = inserted.first->second;
    if (!inserted.second) {
        timeline.regionStart = regionStart;
    }
    timeline.events.push_back(ClearWhenEvent{ sequence, eventRun, kind, byteBefore, byteAfter, dirtyBefore,
                                              dirtyAfter, hookSite, stickyLogExit, logLineSource, slot });
    return sequence;
}

uint64_t RemsetCheck::ReserveClearWhenSequence()
{
    return nextClearWhenSequence.fetch_add(1, std::memory_order_relaxed);
}

size_t RemsetCheck::GetClearWhenRun() const
{
    return clearWhenRun.load(std::memory_order_acquire);
}

void RemsetCheck::RecordClearWhenEventDrop(bool firstOverflow)
{
    if (firstOverflow) {
        clearWhenEventRingOverflows.fetch_add(1, std::memory_order_relaxed);
    }
    clearWhenEventsDropped.fetch_add(1, std::memory_order_relaxed);
}

uint64_t RemsetCheck::RecordPendingClearWhenEvent(ClearWhenPendingEvent& event)
{
    if (event.sequence == 0) {
        event.sequence = ReserveClearWhenSequence();
    }
    if (event.run == 0) {
        event.run = GetClearWhenRun();
    }
    Mutator* mutator = Mutator::GetMutator();
    if (!MutatorManager::Instance().WorldStopped() && mutator != nullptr) {
        (void)mutator->RecordClearWhenEvent(event);
        return event.sequence;
    }
    std::lock_guard<std::mutex> lg(edgeMutex);
    ReplayClearWhenEventLocked(event);
    return event.sequence;
}

void RemsetCheck::ReplayClearWhenEventLocked(const ClearWhenPendingEvent& event)
{
    if (event.kind == ClearWhenEventKind::CLEAR_UNAVAILABLE_REGION) {
        RecordClearWhenRangeLocked(event.regionStart, event.regionSize, event.kind, false, event.sequence,
                                   event.run);
        return;
    }
    CHECK(event.kind == ClearWhenEventKind::LOG_LINE || event.kind == ClearWhenEventKind::BARRIER_WRITE);
    (void)AppendClearWhenEventLocked(event.line, event.regionStart, event.kind, event.byteBefore, event.byteAfter,
                                     event.dirtyBefore, event.dirtyAfter, event.hookSite, event.stickyLogExit,
                                     event.logLineSource, event.slot, event.sequence, event.run);
}

void RemsetCheck::ReplayClearWhenEvents(const ClearWhenPendingEvent* events, size_t count)
{
    if (LIKELY(!enabled) || count == 0) {
        return;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "clear timing events may only replay while stopped");
    std::lock_guard<std::mutex> lg(edgeMutex);
    for (size_t i = 0; i < count; ++i) {
        uint64_t sequence = __atomic_load_n(&events[i].sequence, __ATOMIC_ACQUIRE);
        if (sequence == 0) {
            RecordClearWhenEventDrop(false);
            continue;
        }
        replayEvents.push_back(events[i]);
        replayEvents.back().sequence = sequence;
    }
}

void RemsetCheck::FinishReplayClearWhenEvents()
{
    if (LIKELY(!enabled)) {
        return;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "clear timing replay may only finish while stopped");
    std::lock_guard<std::mutex> lg(edgeMutex);
    std::sort(replayEvents.begin(), replayEvents.end(), [](const ClearWhenPendingEvent& left,
                                                           const ClearWhenPendingEvent& right) {
        return left.sequence < right.sequence;
    });
    for (const ClearWhenPendingEvent& event : replayEvents) {
        ReplayClearWhenEventLocked(event);
    }
    replayEvents.clear();
}

void RemsetCheck::RecordRescanByteWrite(MAddress lineStart, RescanWritePath path, uint8_t before, uint8_t after)
{
    if (LIKELY(!enabled)) {
        return;
    }
    std::lock_guard<std::mutex> lg(edgeMutex);
    auto timeline = lineTimelines.find(lineStart);
    if (timeline == lineTimelines.end()) {
        return;
    }
    bool dirty = LoadDirtyBit(lineStart);
    ClearWhenEventKind kind = path == RescanWritePath::BUFFER ? ClearWhenEventKind::RESCAN_BUFFER_WRITE
                                                              : ClearWhenEventKind::RESCAN_DIRTY_WRITE;
    (void)AppendClearWhenEventLocked(lineStart, timeline->second.regionStart, kind, before, after, dirty, dirty);
}

void RemsetCheck::RecordRescanDirtyClear(MAddress regionStart, size_t regionSize, bool dirtyBefore)
{
    if (LIKELY(!enabled) || !dirtyBefore) {
        return;
    }
    std::lock_guard<std::mutex> lg(edgeMutex);
    MAddress regionEnd = regionStart + regionSize;
    for (MAddress line = regionStart; line < regionEnd; line += StickyLog::LINE_SIZE) {
        auto entry = lineTimelines.find(line);
        if (entry == lineTimelines.end()) {
            continue;
        }
        uint8_t byte = LoadLoggedByte(line);
        (void)AppendClearWhenEventLocked(line, entry->second.regionStart,
                                         ClearWhenEventKind::RESCAN_DIRTY_CLEAR, byte, byte, true, false);
    }
}

void RemsetCheck::RecordClearWhenRangeLocked(MAddress regionStart, size_t regionSize, ClearWhenEventKind kind,
                                              bool dirtyAfter, uint64_t sequence, size_t eventRun)
{
    MAddress regionEnd = regionStart + regionSize;
    for (MAddress line = regionStart; line < regionEnd; line += StickyLog::LINE_SIZE) {
        auto entry = lineTimelines.find(line);
        if (entry == lineTimelines.end()) {
            continue;
        }
        uint8_t byte = 0;
        bool dirty = false;
        if (sequence == 0) {
            byte = LoadLoggedByte(line);
            dirty = LoadDirtyBit(line);
        } else {
            uint64_t latestSequence = 0;
            byte = entry->second.startByte;
            dirty = entry->second.startDirty;
            for (const ClearWhenEvent& previous : entry->second.events) {
                if (previous.sequence < sequence && previous.sequence > latestSequence) {
                    latestSequence = previous.sequence;
                    byte = previous.byteAfter;
                    dirty = previous.dirtyAfter;
                }
            }
        }
        if (byte == 0 && dirty == dirtyAfter) {
            continue;
        }
        (void)AppendClearWhenEventLocked(line, entry->second.regionStart, kind, byte, 0, dirty, dirtyAfter,
                                         HookSite::COUNT, StickyLogExit::COUNT, LogLineSource::BARRIER, 0,
                                         sequence, eventRun);
    }
}

void RemsetCheck::RecordClearUnavailableRegion(MAddress regionStart, size_t regionSize)
{
    if (LIKELY(!enabled)) {
        return;
    }
    ClearWhenPendingEvent event;
    event.sequence = ReserveClearWhenSequence();
    event.run = GetClearWhenRun();
    event.kind = ClearWhenEventKind::CLEAR_UNAVAILABLE_REGION;
    event.line = 0;
    event.regionStart = regionStart;
    event.regionSize = regionSize;
    event.slot = 0;
    event.byteBefore = 0;
    event.byteAfter = 0;
    event.dirtyBefore = false;
    event.dirtyAfter = false;
    event.hookSite = HookSite::COUNT;
    event.stickyLogExit = StickyLogExit::COUNT;
    event.logLineSource = LogLineSource::BARRIER;
    (void)RecordPendingClearWhenEvent(event);
}

void RemsetCheck::RecordBeginEpochClear()
{
    if (LIKELY(!enabled)) {
        return;
    }
    std::lock_guard<std::mutex> lg(edgeMutex);
    for (auto& entry : lineTimelines) {
        uint8_t byte = LoadLoggedByte(entry.first);
        bool dirty = LoadDirtyBit(entry.first);
        if (byte == 0 && !dirty) {
            continue;
        }
        (void)AppendClearWhenEventLocked(entry.first, entry.second.regionStart, ClearWhenEventKind::BEGIN_EPOCH,
                                         byte, 0, dirty, false);
    }
}

RemsetCheck::ClearedBy RemsetCheck::ClassifyClearedBy(const Edge& edge, const LineTrace& trace,
                                                       const ClearWhenEvent*& clearEvent,
                                                       size_t& minorsSinceWrite) const
{
    clearEvent = nullptr;
    minorsSinceWrite = 0;
    MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
    auto timeline = lineTimelines.find(line);
    if (timeline == lineTimelines.end()) {
        return ClearedBy::NOT_OBSERVED;
    }
    for (const ClearWhenEvent& event : timeline->second.events) {
        if (event.sequence <= edge.writeSequence) {
            continue;
        }
        if (event.kind == ClearWhenEventKind::POSITIVE_CONTROL_CLEAR && event.slot == edge.slot) {
            clearEvent = &event;
            continue;
        }
        if (event.sequence > trace.consumeSequence) {
            continue;
        }
        if (event.kind == ClearWhenEventKind::CONSUME_START) {
            ++minorsSinceWrite;
        }
        bool matches = !trace.dirtyAtRoundStart ? event.dirtyBefore && !event.dirtyAfter
                                                : trace.byteAtRoundStart == 0 && event.byteBefore != 0 &&
                event.byteAfter == 0;
        if (matches) {
            clearEvent = &event;
        }
    }
    if (minorsSinceWrite == 0 && trace.consumeSequence > edge.writeSequence) {
        minorsSinceWrite = 1;
    }
    if (clearEvent == nullptr) {
        return ClearedBy::NOT_OBSERVED;
    }
    switch (clearEvent->kind) {
        case ClearWhenEventKind::RESCAN_BUFFER_WRITE:
            return ClearedBy::RESCAN_BUFFER;
        case ClearWhenEventKind::RESCAN_DIRTY_WRITE:
        case ClearWhenEventKind::RESCAN_DIRTY_CLEAR:
            return ClearedBy::RESCAN_DIRTY_REGION;
        case ClearWhenEventKind::CLEAR_UNAVAILABLE_REGION:
            return ClearedBy::CLEAR_UNAVAILABLE_REGION;
        case ClearWhenEventKind::BEGIN_EPOCH:
            return ClearedBy::BEGIN_EPOCH;
        case ClearWhenEventKind::POSITIVE_CONTROL_CLEAR:
            return ClearedBy::POSITIVE_CONTROL;
        case ClearWhenEventKind::LOG_LINE:
        case ClearWhenEventKind::BARRIER_WRITE:
        case ClearWhenEventKind::CONSUME_START:
        case ClearWhenEventKind::POSITIVE_CONTROL_RESTORE:
            return ClearedBy::NOT_OBSERVED;
    }
    return ClearedBy::NOT_OBSERVED;
}

void RemsetCheck::EmitClearWhenTimeline(const Edge& edge, const LineTrace& trace, size_t run, bool visited)
{
    MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
    auto timeline = lineTimelines.find(line);
    const ClearWhenEvent* clearEvent = nullptr;
    size_t minorsSinceWrite = 0;
    ClearedBy clearedBy = ClassifyClearedBy(edge, trace, clearEvent, minorsSinceWrite);
    ++clearWhenEdges;
    ++clearedByCounts[static_cast<size_t>(clearedBy)];
    ++missingMinorsSinceWrite[minorsSinceWrite];
    if (clearEvent != nullptr && clearEvent->run < run && minorsSinceWrite > 1) {
        ++missingClearAfterMiddleMinor;
    }
    uint64_t firstLogSequence = 0;
    size_t firstEventIndex = 0;
    if (timeline != lineTimelines.end()) {
        for (size_t i = 0; i < timeline->second.events.size(); ++i) {
            const ClearWhenEvent& event = timeline->second.events[i];
            if (event.sequence > edge.writeSequence) {
                break;
            }
            if (event.kind == ClearWhenEventKind::LOG_LINE) {
                firstLogSequence = event.sequence;
                firstEventIndex = i;
            }
        }
    }
    VLOG(REPORT,
         "[CLEARWHEN-EDGE] run=%zu holder=%p slot=%p target=%p line=%p startRun=%zu startByte=%u "
         "startDirty=%u firstLogSequence=%llu writeSequence=%llu writeSite=%s writeExit=%s "
         "consumeSequence=%llu consumeByte=%u consumeDirty=%u enteredDirtyLoop=%u visitorVisited=%u "
         "minorsSinceWrite=%zu clearedBy=%s clearSequence=%llu clearRun=%zu",
         run, reinterpret_cast<void*>(edge.holder), reinterpret_cast<void*>(edge.slot),
         reinterpret_cast<void*>(edge.target), reinterpret_cast<void*>(line),
         timeline == lineTimelines.end() ? 0 : timeline->second.startRun,
         timeline == lineTimelines.end() ? 0 : static_cast<unsigned>(timeline->second.startByte),
         timeline == lineTimelines.end() ? 0 : static_cast<unsigned>(timeline->second.startDirty),
         static_cast<unsigned long long>(firstLogSequence), static_cast<unsigned long long>(edge.writeSequence),
         HookSiteName(edge.site), StickyLogExitName(edge.stickyLogExit),
         static_cast<unsigned long long>(trace.consumeSequence), static_cast<unsigned>(trace.byteAtRoundStart),
         static_cast<unsigned>(trace.dirtyAtRoundStart), static_cast<unsigned>(trace.dirtyRegionConsidered),
         static_cast<unsigned>(visited), minorsSinceWrite, ClearedByName(clearedBy),
         static_cast<unsigned long long>(clearEvent == nullptr ? 0 : clearEvent->sequence),
         clearEvent == nullptr ? 0 : clearEvent->run);
    if (timeline == lineTimelines.end()) {
        return;
    }
    for (size_t i = firstEventIndex; i < timeline->second.events.size(); ++i) {
        const ClearWhenEvent& event = timeline->second.events[i];
        if (event.sequence > trace.consumeSequence && event.run > run) {
            break;
        }
        if (event.kind == ClearWhenEventKind::BARRIER_WRITE && event.sequence != edge.writeSequence) {
            continue;
        }
        VLOG(REPORT,
             "[CLEARWHEN-EVENT] run=%zu slot=%p line=%p sequence=%llu eventRun=%zu kind=%s byte=%u->%u "
             "dirty=%u->%u hookSite=%s stickyExit=%s logSource=%s eventSlot=%p",
             run, reinterpret_cast<void*>(edge.slot), reinterpret_cast<void*>(line),
             static_cast<unsigned long long>(event.sequence), event.run, ClearWhenEventKindName(event.kind),
             static_cast<unsigned>(event.byteBefore), static_cast<unsigned>(event.byteAfter),
             static_cast<unsigned>(event.dirtyBefore), static_cast<unsigned>(event.dirtyAfter),
             HookSiteName(event.hookSite), StickyLogExitName(event.stickyLogExit),
             LogLineSourceName(event.logLineSource), reinterpret_cast<void*>(event.slot));
    }
}

void RemsetCheck::CheckRound(size_t run, size_t young)
{
    if (LIKELY(!enabled)) {
        return;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "remset check requires stopped mutators");
    CHECK(!visitedRoundActive);
    clearWhenRun.store(run, std::memory_order_release);
    visitedRoundRun = run;
    visitedRoundOldMissing = 0;
    visitedRoundCandidates.clear();
    visitedLines.clear();
    retain2SkippedLines.clear();
    visitedRoundLineTraces.clear();
    visitedRoundRegionLines.clear();
    for (size_t i = 0; i < VISITOR_HOOK_SITE_COUNT; ++i) {
        visitedRoundLineHits[i] = 0;
    }
    retain2SkippedThisRound = 0;
    CHECK(!retain2SkipControlActive);
    CHECK(!clearWhenControlActive);
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
        for (auto& timelineEntry : lineTimelines) {
            LineTimeline& timeline = timelineEntry.second;
            std::sort(timeline.events.begin(), timeline.events.end(), [](const ClearWhenEvent& left,
                                                                         const ClearWhenEvent& right) {
                return left.sequence < right.sequence;
            });
            if (!timeline.events.empty()) {
                timeline.startRun = timeline.events.front().run;
                timeline.startByte = timeline.events.front().byteBefore;
                timeline.startDirty = timeline.events.front().dirtyBefore;
            }
        }
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
        std::unordered_set<MAddress> activeLines;
        for (const auto& entry : edges) {
            activeLines.insert(entry.second.holder & ~(StickyLog::LINE_SIZE - 1));
        }
        for (auto it = lineTimelines.begin(); it != lineTimelines.end();) {
            if (activeLines.find(it->first) == activeLines.end() && LoadLoggedByte(it->first) == 0 &&
                !LoadDirtyBit(it->first)) {
                it = lineTimelines.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const Edge& edge : candidates) {
        MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
        uint8_t byte = LoadLoggedByte(line);
        bool dirty = LoadDirtyBit(edge.holder);
        bool buffered = bufferedLines.find(line) != bufferedLines.end();
        auto retainedWrite = bufferRetainedWriteRuns.find(line);
        bool writtenByPreviousRound = byte == 2 && retainedWrite != bufferRetainedWriteRuns.end() &&
            retainedWrite->second + 1 == run;
        if (byte != 2 && retainedWrite != bufferRetainedWriteRuns.end()) {
            bufferRetainedWriteRuns.erase(retainedWrite);
        }
        auto inserted = visitedRoundLineTraces.emplace(
            line, LineTrace{ edge.holderRegionStart, byte, dirty, buffered, false, false,
                             writtenByPreviousRound, DirtyLinePath::NONE, 0 });
        if (inserted.second) {
            visitedRoundRegionLines[edge.holderRegionStart].push_back(line);
            std::lock_guard<std::mutex> lg(edgeMutex);
            inserted.first->second.consumeSequence = AppendClearWhenEventLocked(
                line, edge.holderRegionStart, ClearWhenEventKind::CONSUME_START, byte, byte, dirty, dirty);
        }
    }

    if (retain2SkipControlEnabled && !retain2SkipControlCaught) {
        for (const Edge& edge : candidates) {
            MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
            auto trace = visitedRoundLineTraces.find(line);
            CHECK(trace != visitedRoundLineTraces.end());
            if (trace->second.byteAtRoundStart != 1 || !trace->second.dirtyAtRoundStart ||
                trace->second.inBuffer) {
                continue;
            }
            retain2SkipControlPreviousByte = ExchangeLoggedByte(line, 2);
            CHECK(retain2SkipControlPreviousByte == 1);
            retain2SkipControlLine = line;
            retain2SkipControlSlot = edge.slot;
            retain2SkipControlActive = true;
            VLOG(REPORT,
                 "[RETAIN2SKIP-POSCTRL-SELECT] run=%zu line=%p slot=%p previousByte=%u dirty=1 inBuffer=0 "
                 "setByte=2",
                 run, reinterpret_cast<void*>(line), reinterpret_cast<void*>(edge.slot),
                 static_cast<unsigned>(retain2SkipControlPreviousByte));
            break;
        }
    }

    if (clearWhenControlEnabled && !clearWhenControlCaught) {
        for (const Edge& edge : candidates) {
            MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
            auto trace = visitedRoundLineTraces.find(line);
            CHECK(trace != visitedRoundLineTraces.end());
            if (trace->second.byteAtRoundStart == 0 || !trace->second.dirtyAtRoundStart ||
                trace->second.inBuffer) {
                continue;
            }
            bool oldDirty = ExchangeDirtyBit(edge.holder, false);
            CHECK(oldDirty);
            {
                std::lock_guard<std::mutex> lg(edgeMutex);
                (void)AppendClearWhenEventLocked(line, edge.holderRegionStart,
                                                 ClearWhenEventKind::POSITIVE_CONTROL_CLEAR,
                                                 trace->second.byteAtRoundStart,
                                                 trace->second.byteAtRoundStart, true, false,
                                                 HookSite::COUNT, StickyLogExit::COUNT,
                                                 LogLineSource::BARRIER, edge.slot);
            }
            clearWhenControlLine = line;
            clearWhenControlSlot = edge.slot;
            clearWhenControlActive = true;
            VLOG(REPORT,
                 "[CLEARWHEN-POSCTRL-SELECT] run=%zu line=%p slot=%p byte=%u dirty=1 inBuffer=0 setDirty=0",
                 run, reinterpret_cast<void*>(line), reinterpret_cast<void*>(edge.slot),
                 static_cast<unsigned>(trace->second.byteAtRoundStart));
            break;
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

void RemsetCheck::RecordBufferLineResult(MAddress lineStart, bool retained)
{
    if (LIKELY(!enabled) || !visitedRoundActive) {
        return;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "buffer line result requires stopped mutators");
    CHECK((lineStart & (StickyLog::LINE_SIZE - 1)) == 0);
    if (retained) {
        bufferRetainedWriteRuns[lineStart] = visitedRoundRun;
    } else {
        bufferRetainedWriteRuns.erase(lineStart);
    }
}

void RemsetCheck::RecordDirtyRegionEntry(MAddress regionStart, bool accepted)
{
    if (LIKELY(!enabled) || !visitedRoundActive) {
        return;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "dirty region recording requires stopped mutators");
    auto regionLines = visitedRoundRegionLines.find(regionStart);
    if (regionLines == visitedRoundRegionLines.end()) {
        return;
    }
    for (MAddress line : regionLines->second) {
        auto trace = visitedRoundLineTraces.find(line);
        CHECK(trace != visitedRoundLineTraces.end());
        trace->second.dirtyRegionConsidered = true;
        trace->second.regionFiltered = !accepted;
    }
}

void RemsetCheck::RecordDirtyLinePath(MAddress lineStart, DirtyLinePath path)
{
    if (LIKELY(!enabled) || !visitedRoundActive) {
        return;
    }
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "dirty line path recording requires stopped mutators");
    CHECK((lineStart & (StickyLog::LINE_SIZE - 1)) == 0);
    CHECK(path != DirtyLinePath::NONE);
    bufferRetainedWriteRuns.erase(lineStart);
    auto trace = visitedRoundLineTraces.find(lineStart);
    if (trace != visitedRoundLineTraces.end()) {
        trace->second.dirtyLinePath = path;
    }
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
            if (visited(edge)) {
                continue;
            }
            if (!checkerVisited(edge, 0)) {
                trueUnvisitedControlCaught = true;
                bool skippedByRetain2 = retain2SkippedLines.find(line) != retain2SkippedLines.end();
                VLOG(REPORT,
                     "[VISITORHOOK-MISS] run=%zu reason=true-unvisited-control holder=%p slot=%p target=%p line=%p "
                     "skippedByRetain2=%u",
                     run, reinterpret_cast<void*>(edge.holder), reinterpret_cast<void*>(edge.slot),
                     reinterpret_cast<void*>(edge.target), reinterpret_cast<void*>(line),
                     static_cast<unsigned>(skippedByRetain2));
                VLOG(REPORT,
                     "[VISITORHOOK-POSCTRL-TRUE-UNVISITED] run=%zu caught=1 slot=%p line=%p skippedByRetain2=%u",
                     run, reinterpret_cast<void*>(edge.slot), reinterpret_cast<void*>(line),
                     static_cast<unsigned>(skippedByRetain2));
            }
            break;
        }
    }

    size_t missingNewThisRound = 0;
    size_t retain2SkipMissingThisRound = 0;
    size_t notInDirtyLoopMissingThisRound = 0;
    size_t regionFilteredMissingThisRound = 0;
    size_t otherPathMissingThisRound = 0;
    size_t missingByteAtRoundStartThisRound[3]{};
    size_t missingDirtyAtRoundStartThisRound[2]{};
    size_t missingInBufferThisRound = 0;
    size_t missingByte2WrittenByPreviousRoundThisRound = 0;
    size_t clearWhenMissingThisRound = 0;
    for (const Edge& edge : visitedRoundCandidates) {
        bool edgeVisited = checkerVisited(edge, 0);
        const ClearWhenEvent* allEdgeClear = nullptr;
        size_t allEdgeMinors = 0;
        auto trace = visitedRoundLineTraces.find(edge.holder & ~(StickyLog::LINE_SIZE - 1));
        CHECK(trace != visitedRoundLineTraces.end());
        (void)ClassifyClearedBy(edge, trace->second, allEdgeClear, allEdgeMinors);
        if (allEdgeMinors <= 1) {
            ++clearWhenAllEdgesOneMinor;
        } else {
            ++clearWhenAllEdgesMultipleMinors;
        }
        if (allEdgeClear != nullptr) {
            ++clearWhenAllEdgesClearedAfterWrite;
        }
        if (edgeVisited) {
            continue;
        }
        ++missingNewThisRound;
        ++clearWhenMissingThisRound;
        MAddress line = edge.holder & ~(StickyLog::LINE_SIZE - 1);
        CHECK(trace->second.byteAtRoundStart <= 2);
        ++missingByteAtRoundStartThisRound[trace->second.byteAtRoundStart];
        ++missingByteAtRoundStart[trace->second.byteAtRoundStart];
        size_t dirtyIndex = trace->second.dirtyAtRoundStart ? 1 : 0;
        ++missingDirtyAtRoundStartThisRound[dirtyIndex];
        ++missingDirtyAtRoundStart[dirtyIndex];
        if (trace->second.inBuffer) {
            ++missingInBufferThisRound;
            ++missingInBuffer;
        }
        if (trace->second.byte2WrittenByPreviousRound) {
            ++missingByte2WrittenByPreviousRoundThisRound;
            ++missingByte2WrittenByPreviousRound;
        }
        const char* pathBucket = "other";
        if (trace->second.dirtyLinePath == DirtyLinePath::RETAIN2_SKIP) {
            pathBucket = "retain2-skip";
            ++retain2SkipMissingThisRound;
            ++retain2SkipMissing;
        } else if (!trace->second.dirtyRegionConsidered) {
            pathBucket = "not-in-dirty-loop";
            ++notInDirtyLoopMissingThisRound;
            ++notInDirtyLoopMissing;
        } else if (trace->second.regionFiltered) {
            pathBucket = "region-filtered";
            ++regionFilteredMissingThisRound;
            ++regionFilteredMissing;
        } else {
            ++otherPathMissingThisRound;
            ++otherPathMissing;
        }
        bool skippedByRetain2 = retain2SkippedLines.find(line) != retain2SkippedLines.end();
        {
            std::lock_guard<std::mutex> lg(edgeMutex);
            EmitClearWhenTimeline(edge, trace->second, run, false);
        }
        VLOG(REPORT,
             "[VISITORHOOK-MISS] run=%zu reason=unvisited holder=%p slot=%p target=%p line=%p "
             "skippedByRetain2=%u site=%s byteAtRoundStart=%u dirtyAtRoundStart=%u "
             "dirtyRegionConsidered=%u regionFiltered=%u dirtyLinePath=%s pathBucket=%s inBuffer=%u "
             "byte2WrittenByPreviousRound=%u",
             run, reinterpret_cast<void*>(edge.holder), reinterpret_cast<void*>(edge.slot),
             reinterpret_cast<void*>(edge.target), reinterpret_cast<void*>(line),
             static_cast<unsigned>(skippedByRetain2), HookSiteName(edge.site),
             static_cast<unsigned>(trace->second.byteAtRoundStart),
             static_cast<unsigned>(trace->second.dirtyAtRoundStart),
             static_cast<unsigned>(trace->second.dirtyRegionConsidered),
             static_cast<unsigned>(trace->second.regionFiltered), DirtyLinePathName(trace->second.dirtyLinePath),
             pathBucket, static_cast<unsigned>(trace->second.inBuffer),
             static_cast<unsigned>(trace->second.byte2WrittenByPreviousRound));
    }
    if (retain2SkipControlActive) {
        bool skipped = retain2SkippedLines.find(retain2SkipControlLine) != retain2SkippedLines.end();
        bool lineVisited = visitedLines.find(retain2SkipControlLine) != visitedLines.end();
        bool edgeMissing = false;
        for (const Edge& edge : visitedRoundCandidates) {
            if (edge.slot == retain2SkipControlSlot && !visited(edge)) {
                edgeMissing = true;
                break;
            }
        }
        uint8_t byteAfterScan = ExchangeLoggedByte(retain2SkipControlLine, retain2SkipControlPreviousByte);
        retain2SkipControlCaught = skipped && !lineVisited && edgeMissing;
        VLOG(REPORT,
             "[RETAIN2SKIP-POSCTRL] run=%zu fires=%u line=%p slot=%p retain2Skip=%u unvisited=%u missing=%u "
             "byteAfterScan=%u restoredByte=%u",
             run, static_cast<unsigned>(retain2SkipControlCaught),
             reinterpret_cast<void*>(retain2SkipControlLine), reinterpret_cast<void*>(retain2SkipControlSlot),
             static_cast<unsigned>(skipped), static_cast<unsigned>(!lineVisited),
             static_cast<unsigned>(edgeMissing), static_cast<unsigned>(byteAfterScan),
             static_cast<unsigned>(retain2SkipControlPreviousByte));
        retain2SkipControlLine = 0;
        retain2SkipControlSlot = 0;
        retain2SkipControlPreviousByte = 0;
        retain2SkipControlActive = false;
    }
    if (clearWhenControlActive) {
        bool lineVisited = visitedLines.find(clearWhenControlLine) != visitedLines.end();
        bool edgeMissing = false;
        for (const Edge& edge : visitedRoundCandidates) {
            if (edge.slot == clearWhenControlSlot && !visited(edge)) {
                edgeMissing = true;
                break;
            }
        }
        uint8_t byteAfterScan = LoadLoggedByte(clearWhenControlLine);
        bool dirtyAfterScan = LoadDirtyBit(clearWhenControlLine);
        bool oldDirty = ExchangeDirtyBit(clearWhenControlLine, true);
        CHECK(!oldDirty);
        {
            std::lock_guard<std::mutex> lg(edgeMutex);
            auto timeline = lineTimelines.find(clearWhenControlLine);
            CHECK(timeline != lineTimelines.end());
            (void)AppendClearWhenEventLocked(clearWhenControlLine, timeline->second.regionStart,
                                             ClearWhenEventKind::POSITIVE_CONTROL_RESTORE,
                                             byteAfterScan, byteAfterScan, false, true,
                                             HookSite::COUNT, StickyLogExit::COUNT,
                                             LogLineSource::BARRIER, clearWhenControlSlot);
        }
        clearWhenControlCaught = !lineVisited && edgeMissing && byteAfterScan != 0 && !dirtyAfterScan;
        VLOG(REPORT,
             "[CLEARWHEN-POSCTRL] run=%zu fires=%u line=%p slot=%p byte=%u dirty=%u enteredDirtyLoop=0 "
             "visitorVisited=%u missing=%u restoredDirty=1",
             run, static_cast<unsigned>(clearWhenControlCaught), reinterpret_cast<void*>(clearWhenControlLine),
             reinterpret_cast<void*>(clearWhenControlSlot), static_cast<unsigned>(byteAfterScan),
             static_cast<unsigned>(dirtyAfterScan), static_cast<unsigned>(lineVisited),
             static_cast<unsigned>(edgeMissing));
        clearWhenControlLine = 0;
        clearWhenControlSlot = 0;
        clearWhenControlActive = false;
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
    VLOG(REPORT,
         "[RETAIN2SKIP] run=%zu missing=%zu retain2Skip=%zu notInDirtyLoop=%zu regionFiltered=%zu other=%zu "
         "byteAtRoundStart0=%zu byteAtRoundStart1=%zu byteAtRoundStart2=%zu dirtyBit0=%zu dirtyBit1=%zu "
         "inBuffer=%zu byte2WrittenByPreviousRound=%zu",
         run, missingNewThisRound, retain2SkipMissingThisRound, notInDirtyLoopMissingThisRound,
         regionFilteredMissingThisRound, otherPathMissingThisRound, missingByteAtRoundStartThisRound[0],
         missingByteAtRoundStartThisRound[1], missingByteAtRoundStartThisRound[2],
         missingDirtyAtRoundStartThisRound[0], missingDirtyAtRoundStartThisRound[1], missingInBufferThisRound,
         missingByte2WrittenByPreviousRoundThisRound);
    VLOG(REPORT, "[CLEARWHEN] run=%zu edges=%zu missing=%zu", run, visitedRoundCandidates.size(),
         clearWhenMissingThisRound);

    visitedRoundActive = false;
    {
        std::lock_guard<std::mutex> lg(edgeMutex);
        clearWhenRun.store(run + 1, std::memory_order_release);
    }
    visitedRoundCandidates.clear();
    visitedLines.clear();
    retain2SkippedLines.clear();
    visitedRoundLineTraces.clear();
    visitedRoundRegionLines.clear();
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
        long long visitorDelta = static_cast<long long>(missing) - static_cast<long long>(missingNewPredicate);
        VLOG(REPORT,
             "[VISITORHOOK-FINAL] runs=%zu young0Excluded=%zu edgesFromBarrier=%zu missingOldPredicate=%zu "
             "missingNewPredicate=%zu deltaOldMinusNew=%lld bufferVisitorCalls=%zu dirtyRegionVisitorCalls=%zu "
             "retain2Skipped=%zu falseUnvisitedControlCaught=%u trueUnvisitedControlCaught=%u",
             includedRuns, youngZeroExcluded, edgesFromBarrier, missing, missingNewPredicate, visitorDelta,
             visitedLineHits[static_cast<size_t>(VisitorHookSite::BUFFER)],
             visitedLineHits[static_cast<size_t>(VisitorHookSite::DIRTY_REGION)], retain2Skipped,
             static_cast<unsigned>(falseUnvisitedControlCaught),
             static_cast<unsigned>(trueUnvisitedControlCaught));
        VLOG(REPORT,
             "[RETAIN2SKIP-FINAL] missing=%zu retain2Skip=%zu notInDirtyLoop=%zu regionFiltered=%zu other=%zu "
             "byteAtRoundStart0=%zu byteAtRoundStart1=%zu byteAtRoundStart2=%zu dirtyBit0=%zu dirtyBit1=%zu "
             "inBuffer=%zu byte2WrittenByPreviousRound=%zu posctrlFires=%u",
             missingNewPredicate, retain2SkipMissing, notInDirtyLoopMissing, regionFilteredMissing,
             otherPathMissing, missingByteAtRoundStart[0], missingByteAtRoundStart[1],
             missingByteAtRoundStart[2], missingDirtyAtRoundStart[0], missingDirtyAtRoundStart[1],
             missingInBuffer, missingByte2WrittenByPreviousRound,
             static_cast<unsigned>(retain2SkipControlCaught));
        VLOG(REPORT,
             "[CLEARWHEN-FINAL] edges=%zu RescanLoggedLines-buffer=%zu RescanLoggedLines-dirty-region=%zu "
             "ClearUnavailableRegion=%zu BeginEpoch=%zu positiveControl=%zu notObserved=%zu "
             "clearAfterMiddleMinor=%zu allEdgeOneMinor=%zu allEdgeMultipleMinors=%zu "
             "allEdgeClearedAfterWrite=%zu posctrlFires=%u eventRingCapacity=%zu eventRingOverflows=%zu "
             "eventsDropped=%zu",
             clearWhenEdges, clearedByCounts[static_cast<size_t>(ClearedBy::RESCAN_BUFFER)],
             clearedByCounts[static_cast<size_t>(ClearedBy::RESCAN_DIRTY_REGION)],
             clearedByCounts[static_cast<size_t>(ClearedBy::CLEAR_UNAVAILABLE_REGION)],
             clearedByCounts[static_cast<size_t>(ClearedBy::BEGIN_EPOCH)],
             clearedByCounts[static_cast<size_t>(ClearedBy::POSITIVE_CONTROL)],
             clearedByCounts[static_cast<size_t>(ClearedBy::NOT_OBSERVED)], missingClearAfterMiddleMinor,
             clearWhenAllEdgesOneMinor, clearWhenAllEdgesMultipleMinors, clearWhenAllEdgesClearedAfterWrite,
             static_cast<unsigned>(clearWhenControlCaught), CLEAR_WHEN_EVENT_RING_CAPACITY,
             clearWhenEventRingOverflows.load(std::memory_order_relaxed),
             clearWhenEventsDropped.load(std::memory_order_relaxed));
        for (const auto& entry : missingMinorsSinceWrite) {
            VLOG(REPORT, "[CLEARWHEN-MINORS] minorsSinceWrite=%zu edges=%zu", entry.first, entry.second);
        }
    }
    std::lock_guard<std::mutex> lg(edgeMutex);
    edges.clear();
    lineTimelines.clear();
    replayEvents.clear();
}
} // namespace MapleRuntime
