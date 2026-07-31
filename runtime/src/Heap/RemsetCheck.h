// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REMSET_CHECK_H
#define MRT_REMSET_CHECK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class BaseObject;
class RegionInfo;

class RemsetCheck {
public:
    enum class HookSite : uint8_t {
        WRITE_REFERENCE,
        WRITE_STATIC_REF,
        WRITE_STRUCT,
        ATOMIC_WRITE_REFERENCE,
        ATOMIC_SWAP_REFERENCE,
        COMPARE_AND_SWAP_REFERENCE,
        COPY_STRUCT_ARRAY,
        WRITE_GENERIC,
        COUNT,
    };

    enum class StickyLogExit : uint8_t {
        NO_LOGOBJECT_CALL,
        EXIT_A_NULL_OBJECT,
        EXIT_B_ALREADY_LOGGED,
        EXIT_C_NO_MUTATOR,
        EXIT_D_OUT_OF_HEAP_RANGE,
        EXIT_D_BASE_NULL,
        EXIT_D_OTHER,
        LOGGED,
        COUNT,
    };

    enum class VisitorHookSite : uint8_t {
        BUFFER,
        DIRTY_REGION,
        COUNT,
    };

    enum class DirtyLinePath : uint8_t {
        NONE,
        RETAIN2_SKIP,
        VISITED,
        BYTE0_CONTINUE,
    };

    enum class LogLineSource : uint8_t {
        BARRIER,
        DEFERRED_LOG_RING,
        PROMOTION,
    };

    enum class RescanWritePath : uint8_t {
        BUFFER,
        DIRTY_REGION,
    };

    enum class ClearWhenEventKind : uint8_t {
        LOG_LINE,
        BARRIER_WRITE,
        CONSUME_START,
        VISIT_BUFFER,
        VISIT_DIRTY_REGION,
        RESCAN_BUFFER_WRITE,
        RESCAN_DIRTY_WRITE,
        RESCAN_DIRTY_CLEAR,
        CLEAR_UNAVAILABLE_REGION,
        BEGIN_EPOCH,
        POSITIVE_CONTROL_CLEAR,
        POSITIVE_CONTROL_RESTORE,
    };

    enum class ClearedBy : uint8_t {
        RESCAN_BUFFER,
        RESCAN_DIRTY_REGION,
        CLEAR_UNAVAILABLE_REGION,
        BEGIN_EPOCH,
        POSITIVE_CONTROL,
        NOT_OBSERVED,
        COUNT,
    };

    enum class ClearPath : uint8_t {
        BUFFER,
        DIRTY_BYTE,
        DIRTY_REGION,
        UNKNOWN,
        COUNT,
    };

    enum class ClearVisitClass : uint8_t {
        AFTER_VISIT,
        WITHOUT_VISIT,
        UNDETERMINED,
        COUNT,
    };

    struct ClearWhenPendingEvent {
        uint64_t sequence;
        size_t run;
        ClearWhenEventKind kind;
        MAddress line;
        MAddress regionStart;
        size_t regionSize;
        MAddress slot;
        uint8_t byteBefore;
        uint8_t byteAfter;
        bool dirtyBefore;
        bool dirtyAfter;
        HookSite hookSite;
        StickyLogExit stickyLogExit;
        LogLineSource logLineSource;
    };

    static constexpr size_t CLEAR_WHEN_EVENT_RING_CAPACITY = 2097152;

    static RemsetCheck& Instance() noexcept;

    void ConfigureFromEnvironment(bool forceSlowPath);
    bool IsEnabled() const { return enabled; }
    void RecordHookHit(HookSite site);
    void BeginLogObject();
    void RecordStickyLogExit(StickyLogExit exit, BaseObject* object, MAddress heapStart, size_t heapSize);
    void RecordNoLogObjectCall(HookSite site);
    size_t GetThreadLogObjectCallCount() const;
    void RunStickyLogExitPositiveControls(BaseObject* object);
    void RecordLoggedLineWrite(MAddress lineStart, LogLineSource source, bool dirtyBefore);
    void RecordBarrierEdge(BaseObject* holder, MAddress slot, BaseObject* target, HookSite site);
    void RecordMajor(size_t completedMinorRuns, size_t minorRunsSinceMajor);
    void RecordBeginEpoch(size_t completedMinorRuns);
    void CheckRound(size_t run, size_t young);
    void RecordVisitedLine(MAddress lineStart, VisitorHookSite site);
    void RecordRetain2SkippedLine(MAddress lineStart);
    void RecordBufferLineResult(MAddress lineStart, bool retained);
    void RecordDirtyRegionEntry(MAddress regionStart, bool accepted);
    void RecordDirtyLinePath(MAddress lineStart, DirtyLinePath path);
    void RecordRescanByteWrite(MAddress lineStart, RescanWritePath path, uint8_t before, uint8_t after);
    void RecordRescanDirtyClear(MAddress regionStart, size_t regionSize, bool dirtyBefore);
    void RecordClearUnavailableRegion(MAddress regionStart, size_t regionSize);
    void RecordBeginEpochClear();
    uint64_t ReserveClearWhenSequence();
    size_t GetClearWhenRun() const;
    void ReplayClearWhenEvents(const ClearWhenPendingEvent* events, size_t count);
    void FinishReplayClearWhenEvents();
    void RecordClearWhenEventDrop(bool firstOverflow);
    void CheckVisitedRound(size_t run);
    void Fini();

private:
    struct Edge {
        MAddress holder;
        MAddress slot;
        MAddress target;
        MAddress holderRegionStart;
        MAddress targetRegionStart;
        uint64_t holderRegionEpoch;
        uint64_t targetRegionEpoch;
        uint8_t holderRegionType;
        HookSite site;
        StickyLogExit stickyLogExit;
        MAddress logHeapStart;
        size_t logHeapSize;
        bool holderInHeapRangeAtLog;
        uint64_t writeSequence;
    };

    struct LineTrace {
        MAddress regionStart;
        uint8_t byteAtRoundStart;
        bool dirtyAtRoundStart;
        bool inBuffer;
        bool dirtyRegionConsidered;
        bool regionFiltered;
        bool byte2WrittenByPreviousRound;
        DirtyLinePath dirtyLinePath;
        uint64_t consumeSequence;
    };

    struct ClearWhenEvent {
        uint64_t sequence;
        size_t run;
        ClearWhenEventKind kind;
        uint8_t byteBefore;
        uint8_t byteAfter;
        bool dirtyBefore;
        bool dirtyAfter;
        HookSite hookSite;
        StickyLogExit stickyLogExit;
        LogLineSource logLineSource;
        MAddress slot;
        size_t regionNonzeroLines;
    };

    struct LineTimeline {
        MAddress regionStart;
        size_t startRun;
        uint8_t startByte;
        bool startDirty;
        std::vector<ClearWhenEvent> events;
    };

    RemsetCheck() = default;
    ~RemsetCheck() = default;
    RemsetCheck(const RemsetCheck&) = delete;
    RemsetCheck& operator=(const RemsetCheck&) = delete;

    bool Revalidate(const Edge& edge, RegionInfo*& holderRegion, RegionInfo*& targetRegion) const;
    uint8_t LoadLoggedByte(MAddress line) const;
    uint8_t ExchangeLoggedByte(MAddress line, uint8_t value) const;
    bool LoadDirtyBit(MAddress holder) const;
    bool ExchangeDirtyBit(MAddress holder, bool value) const;
    uint64_t AppendClearWhenEventLocked(MAddress line, MAddress regionStart, ClearWhenEventKind kind,
                                        uint8_t byteBefore, uint8_t byteAfter, bool dirtyBefore, bool dirtyAfter,
                                        HookSite hookSite = HookSite::COUNT,
                                        StickyLogExit stickyLogExit = StickyLogExit::COUNT,
                                        LogLineSource logLineSource = LogLineSource::BARRIER, MAddress slot = 0,
                                        uint64_t sequence = 0, size_t eventRun = 0,
                                        size_t regionNonzeroLines = 0);
    uint64_t RecordPendingClearWhenEvent(ClearWhenPendingEvent& event);
    void ReplayClearWhenEventLocked(const ClearWhenPendingEvent& event);
    void RecordClearWhenRangeLocked(MAddress regionStart, size_t regionSize, ClearWhenEventKind kind,
                                    bool dirtyAfter, uint64_t sequence = 0, size_t eventRun = 0);
    ClearedBy ClassifyClearedBy(const Edge& edge, const LineTrace& trace, const ClearWhenEvent*& clearEvent,
                                size_t& minorsSinceWrite) const;
    void EmitClearWhenTimeline(const Edge& edge, const LineTrace& trace, size_t run, bool visited);

    static constexpr size_t HOOK_SITE_COUNT = static_cast<size_t>(HookSite::COUNT);
    static constexpr size_t STICKY_LOG_EXIT_COUNT = static_cast<size_t>(StickyLogExit::COUNT);
    static constexpr size_t VISITOR_HOOK_SITE_COUNT = static_cast<size_t>(VisitorHookSite::COUNT);
    static constexpr size_t CLEAR_PATH_COUNT = static_cast<size_t>(ClearPath::COUNT);
    static constexpr size_t CLEAR_VISIT_CLASS_COUNT = static_cast<size_t>(ClearVisitClass::COUNT);

    struct ClearedWithoutVisitEdge {
        MAddress line;
        MAddress slot;
        uint64_t writeSequence;
        uint64_t clearSequence;
        size_t clearRun;
        size_t recoveredRun;
    };

    bool configured = false;
    bool enabled = false;
    bool hitCountingEnabled = false;
    bool detectControlEnabled = false;
    bool produceControlEnabled = false;
    bool orphanControlEnabled = false;
    bool stickyLogExitControlEnabled = false;
    bool falseUnvisitedControlEnabled = false;
    bool trueUnvisitedControlEnabled = false;
    bool retain2SkipControlEnabled = false;
    bool clearWhenControlEnabled = false;
    bool detectControlCaught = false;
    bool produceControlCaught = false;
    bool orphanControlCaught = false;
    bool falseUnvisitedControlCaught = false;
    bool trueUnvisitedControlCaught = false;
    bool retain2SkipControlCaught = false;
    bool clearWhenControlCaught = false;
    std::atomic<size_t> barrierHits{ 0 };
    std::atomic<size_t> hookHits[HOOK_SITE_COUNT]{};
    std::atomic<size_t> stickyLogExitHits[STICKY_LOG_EXIT_COUNT]{};
    std::atomic<size_t> noLogObjectCallHits[HOOK_SITE_COUNT]{};
    std::atomic<bool> stickyLogExitControlStarted{ false };
    std::mutex edgeMutex;
    std::unordered_map<MAddress, Edge> edges;
    std::unordered_map<MAddress, LineTimeline> lineTimelines;
    std::vector<ClearWhenPendingEvent> replayEvents;
    std::atomic<uint64_t> nextClearWhenSequence{ 1 };
    std::atomic<size_t> clearWhenRun{ 1 };
    std::atomic<size_t> clearWhenEventRingOverflows{ 0 };
    std::atomic<size_t> clearWhenEventsDropped{ 0 };
    bool visitedRoundActive = false;
    size_t visitedRoundRun = 0;
    size_t visitedRoundOldMissing = 0;
    std::vector<Edge> visitedRoundCandidates;
    std::unordered_set<MAddress> visitedLines;
    std::unordered_set<MAddress> retain2SkippedLines;
    std::unordered_map<MAddress, LineTrace> visitedRoundLineTraces;
    std::unordered_map<MAddress, std::vector<MAddress>> visitedRoundRegionLines;
    std::unordered_map<MAddress, size_t> bufferRetainedWriteRuns;
    MAddress retain2SkipControlLine = 0;
    MAddress retain2SkipControlSlot = 0;
    uint8_t retain2SkipControlPreviousByte = 0;
    bool retain2SkipControlActive = false;
    MAddress clearWhenControlLine = 0;
    MAddress clearWhenControlSlot = 0;
    bool clearWhenControlActive = false;
    size_t visitedLineHits[VISITOR_HOOK_SITE_COUNT]{};
    size_t visitedRoundLineHits[VISITOR_HOOK_SITE_COUNT]{};
    size_t retain2Skipped = 0;
    size_t retain2SkippedThisRound = 0;
    size_t retain2SkipMissing = 0;
    size_t notInDirtyLoopMissing = 0;
    size_t regionFilteredMissing = 0;
    size_t otherPathMissing = 0;
    size_t missingByteAtRoundStart[3]{};
    size_t missingDirtyAtRoundStart[2]{};
    size_t missingInBuffer = 0;
    size_t missingByte2WrittenByPreviousRound = 0;
    size_t clearedByCounts[static_cast<size_t>(ClearedBy::COUNT)]{};
    std::map<size_t, size_t> missingMinorsSinceWrite;
    size_t missingClearAfterMiddleMinor = 0;
    size_t clearWhenEdges = 0;
    size_t clearWhenAllEdgesOneMinor = 0;
    size_t clearWhenAllEdgesMultipleMinors = 0;
    size_t clearWhenAllEdgesClearedAfterWrite = 0;
    size_t clearVisitCounts[CLEAR_PATH_COUNT][CLEAR_VISIT_CLASS_COUNT]{};
    std::vector<ClearedWithoutVisitEdge> clearedWithoutVisitEdges;
    std::unordered_map<MAddress, std::vector<size_t>> clearedWithoutVisitByLine;
    std::unordered_set<uint64_t> dirtyClearedNonzeroSequences;
    size_t laterRecovered = 0;
    size_t dirtyClearedNonzeroRegions = 0;
    size_t dirtyClearedNonzeroLines = 0;
    bool bufferAfterVisitControlCaught = false;
    bool dirtyRegionWithoutVisitControlCaught = false;
    size_t includedRuns = 0;
    size_t youngZeroExcluded = 0;
    size_t edgesFromBarrier = 0;
    size_t revalidateDropped = 0;
    size_t missing = 0;
    size_t missingNewPredicate = 0;
    size_t missingByteZero = 0;
    size_t missingOrphan = 0;
    size_t missingDirtyNotBuffered = 0;
    size_t missingOther = 0;
    size_t orphanByteNonzeroDirtyZero = 0;
    size_t majorCount = 0;
    size_t beginEpochCount = 0;
    size_t majorCountAtPreviousMinor = 0;
    size_t beginEpochCountAtPreviousMinor = 0;
    size_t missingStickyLogExits[STICKY_LOG_EXIT_COUNT]{};
    size_t missingHolderInHeapRange = 0;
    size_t missingHolderOutOfHeapRange = 0;
};
} // namespace MapleRuntime

#endif // MRT_REMSET_CHECK_H
