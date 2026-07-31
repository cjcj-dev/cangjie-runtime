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
#include <mutex>
#include <unordered_map>

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

    static RemsetCheck& Instance() noexcept;

    void ConfigureFromEnvironment(bool forceSlowPath);
    bool IsEnabled() const { return enabled; }
    void RecordHookHit(HookSite site);
    void BeginLogObject();
    void RecordStickyLogExit(StickyLogExit exit, BaseObject* object, MAddress heapStart, size_t heapSize);
    void RecordNoLogObjectCall(HookSite site);
    size_t GetThreadLogObjectCallCount() const;
    void RunStickyLogExitPositiveControls(BaseObject* object);
    void RecordBarrierEdge(BaseObject* holder, MAddress slot, BaseObject* target, HookSite site);
    void RecordMajor(size_t completedMinorRuns, size_t minorRunsSinceMajor);
    void RecordBeginEpoch(size_t completedMinorRuns);
    void CheckRound(size_t run, size_t young);
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

    static constexpr size_t HOOK_SITE_COUNT = static_cast<size_t>(HookSite::COUNT);
    static constexpr size_t STICKY_LOG_EXIT_COUNT = static_cast<size_t>(StickyLogExit::COUNT);

    bool configured = false;
    bool enabled = false;
    bool hitCountingEnabled = false;
    bool detectControlEnabled = false;
    bool produceControlEnabled = false;
    bool orphanControlEnabled = false;
    bool stickyLogExitControlEnabled = false;
    bool detectControlCaught = false;
    bool produceControlCaught = false;
    bool orphanControlCaught = false;
    std::atomic<size_t> barrierHits{ 0 };
    std::atomic<size_t> hookHits[HOOK_SITE_COUNT]{};
    std::atomic<size_t> stickyLogExitHits[STICKY_LOG_EXIT_COUNT]{};
    std::atomic<size_t> noLogObjectCallHits[HOOK_SITE_COUNT]{};
    std::atomic<bool> stickyLogExitControlStarted{ false };
    std::mutex edgeMutex;
    std::unordered_map<MAddress, Edge> edges;
    size_t includedRuns = 0;
    size_t youngZeroExcluded = 0;
    size_t edgesFromBarrier = 0;
    size_t revalidateDropped = 0;
    size_t missing = 0;
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
