// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STICKY_LOG_H
#define MRT_STICKY_LOG_H

#include <atomic>
#include <cstdint>
#include <functional>

#include "Cangjie.h"
#include "Common/TypeDef.h"

namespace MapleRuntime {
extern "C" MRT_EXPORT uint8_t* __cj_sticky_logged_base;
extern "C" MRT_EXPORT uintptr_t __cj_sticky_heap_base;
extern "C" MRT_EXPORT uintptr_t __cj_sticky_heap_size;
extern "C" MRT_EXPORT const uint8_t __cj_sticky_line_shift;
extern "C" MRT_EXPORT void CJ_MCC_StickyLogLine(BaseObject* object);

class MemMap;

class StickyLog {
public:
    static constexpr uint8_t LINE_SHIFT = 8;
    static constexpr size_t LINE_SIZE = static_cast<size_t>(1) << LINE_SHIFT;
    static constexpr size_t DEFAULT_YOUNG_BYTES = 32 * 1024 * 1024;

    static StickyLog& Instance() noexcept;

    void Init(MAddress heapStart, size_t heapSize);
    void Fini() noexcept;
    void ConfigureMinorFromEnvironment();

    void Enable(bool value) { enabled = value; }
    bool IsEnabled() const { return enabled; }
    bool IsMinorEnabled() const { return minorEnabled; }
    bool IsMinorValidatorEnabled() const { return minorValidatorEnabled; }
    bool IsForceSlowPathEnabled() const { return forceSlowPathEnabled; }
    bool IsEdgeCompleteEnabled() const { return edgeCompleteEnabled; }
    bool IsEdgeCompleteFakeMissEnabled() const { return edgeCompleteFakeMissEnabled; }
    size_t GetYoungBytesThreshold() const { return youngBytesThreshold; }
    uint32_t GetMajorInterval() const { return majorInterval; }
    // Region promotion age: a young region with live bytes ages until
    // youngAge reaches this value, then the whole region is promoted to old
    // (RegionManager::CollectYoungGarbage). Default 1 = shipped behavior
    // (age once, promote on the second surviving minor).
    uint8_t GetPromoteAge() const { return promoteAge; }
    bool IsLoggedLine(MAddress address) const;
    bool TryLogLine(MAddress address, MAddress& lineStart) const;
    void ClearUnavailableRegion(MAddress regionStart, size_t regionSize);
    void BeginEpoch();

    void RecordEdgeCompleteStoreCandidate(BaseObject* holder, MAddress slot, BaseObject* target);
    bool TryDropEdgeCompleteStoreAtSTW(size_t run);
    bool QueryEdgeCompleteLine(MAddress line, MAddress slot, BaseObject* target);
    bool IsEdgeCompleteDroppedEdge(MAddress slot, BaseObject* target) const;
    bool IsEdgeCompleteFakeMissEdge(MAddress slot, BaseObject* target) const;
    void MarkEdgeCompleteDropCaught();
    void MarkEdgeCompleteFakeMissCaught();
    bool RepairEdgeCompleteDroppedLine();
    void RecordEdgeCompleteRun(size_t oldObjects, size_t refFields, size_t edgesToYoung, size_t inRemset,
                               size_t missing);
    size_t GetEdgeCompleteDropN() const { return edgeCompleteDropN; }
    size_t GetEdgeCompleteEligibleStoreCount() const
    {
        return edgeCompleteEligibleStores.load(std::memory_order_acquire);
    }
    bool HasEdgeCompleteDroppedStore() const
    {
        return edgeCompleteDropInjected.load(std::memory_order_acquire);
    }
    bool WasEdgeCompleteDropCaught() const
    {
        return edgeCompleteDropCaught.load(std::memory_order_acquire);
    }
    size_t GetEdgeCompleteCandidateAttempts() const
    {
        return edgeCompleteCandidateAttempts.load(std::memory_order_acquire);
    }
    size_t GetEdgeCompleteCandidateRejects() const { return edgeCompleteCandidateRejects; }
    size_t GetEdgeCompleteCandidateWaitRuns() const { return edgeCompleteCandidateWaitRuns; }
    bool IsEdgeCompleteDropExhausted() const
    {
        return !HasEdgeCompleteDroppedStore() &&
            (GetEdgeCompleteCandidateAttempts() >= MAX_DROP_CANDIDATES ||
             GetEdgeCompleteCandidateWaitRuns() >= MAX_DROP_WAIT_RUNS);
    }
    bool HasEdgeCompleteFakeMiss() const { return edgeCompleteFakeMissSlot != 0; }
    bool WasEdgeCompleteFakeMissCaught() const { return edgeCompleteFakeMissCaught; }

    using LoggedLineVisitor = std::function<bool(MAddress lineStart, MAddress lineEnd)>;
    // TODO: the minor collector will consume logged lines through this interface and rescan objects in each line.
    void RescanLoggedLines(const LoggedLineVisitor& visitor);

private:
    static constexpr size_t MAX_DROP_CANDIDATES = 50;
    static constexpr size_t MAX_DROP_WAIT_RUNS = 20;

    bool IsEdgeCompleteDroppedLine(MAddress address) const;
    void RejectEdgeCompleteStoreCandidate(size_t run, const char* reason);

    MemMap* loggedMap = nullptr;
    MemMap* dirtyRegionMap = nullptr;
    MAddress heapStart = 0;
    size_t heapSize = 0;
    size_t loggedByteCount = 0;
    size_t dirtyRegionByteCount = 0;
    bool enabled = false;
    bool minorEnabled = false;
    bool minorValidatorEnabled = false;
    bool forceSlowPathEnabled = false;
    bool edgeCompleteEnabled = false;
    bool edgeCompleteFakeMissEnabled = false;
    size_t youngBytesThreshold = DEFAULT_YOUNG_BYTES;
    uint32_t majorInterval = 8;
    uint8_t promoteAge = 1;
    size_t edgeCompleteDropN = 0;
    std::atomic<size_t> edgeCompleteEligibleStores{ 0 };
    std::atomic<bool> edgeCompleteCandidateClaimed{ false };
    std::atomic<MAddress> edgeCompleteCandidateHolder{ 0 };
    std::atomic<MAddress> edgeCompleteCandidateSlot{ 0 };
    std::atomic<MAddress> edgeCompleteCandidateTarget{ 0 };
    std::atomic<size_t> edgeCompleteCandidateAttempts{ 0 };
    size_t edgeCompleteCandidateRejects = 0;
    size_t edgeCompleteCandidateWaitRuns = 0;
    std::atomic<bool> edgeCompleteDropInjected{ false };
    std::atomic<MAddress> edgeCompleteDroppedLine{ 0 };
    uint8_t edgeCompleteDroppedValue = 0;
    std::atomic<bool> edgeCompleteDropCaught{ false };
    MAddress edgeCompleteFakeMissLine = 0;
    MAddress edgeCompleteFakeMissSlot = 0;
    MAddress edgeCompleteFakeMissTarget = 0;
    bool edgeCompleteFakeMissCaught = false;
    size_t edgeCompleteRuns = 0;
    size_t edgeCompleteOldObjects = 0;
    size_t edgeCompleteRefFields = 0;
    size_t edgeCompleteEdgesToYoung = 0;
    size_t edgeCompleteInRemset = 0;
    size_t edgeCompleteMissing = 0;
};
} // namespace MapleRuntime

#endif // MRT_STICKY_LOG_H
