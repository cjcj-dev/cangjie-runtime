// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <atomic>
#include <mutex>
#include <memory>
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/Collector/GcStats.h"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zGenerationId.hpp"
#include "Heap/Collector/GcRequest.h"
namespace MapleRuntime {
class RememberedSet;
struct TenuringInputs;
// Per-generation execution state. The snapshot lock publishes cycle identity
// and phase together; the phase atomic serves existing barrier readers.
// ZGC: zGeneration.hpp:65-78 (generation-owned phase and sequence).
enum class GCCycleGeneration : uint8_t { YOUNG, OLD };
struct GCCycleSnapshot {
    GCCycleGeneration generation;
    uint64_t sequence;
    uint64_t requestIndex;
    GCReason reason;
    GCPhase phase;
    bool active;
};
class GenerationCycle {
public:
    explicit GenerationCycle(GCCycleGeneration generation) : generation(generation) {}
    GCCycleSnapshot Snapshot() const;
    void InitializeWorkers(uint32_t capacity);
    void StopWorkers();
    GCWorkers* Workers() const { return workers.get(); }
    GCStats& Stats() { return stats; }
    ZStatCycle& CycleStats() { return cycleStats; }
    GCPhase Phase() const { return phase.load(std::memory_order_acquire); }
    uint64_t Sequence() const { return Snapshot().sequence; }
    GCReason Reason() const { return reason.load(std::memory_order_acquire); }
    void SelectReason(GCReason value, uint64_t index = 0);
    ZYoungType YoungType() const { return youngType.load(std::memory_order_acquire); }
    void SetYoungType(ZYoungType type);
    void SelectTenuringThreshold(const TenuringInputs& inputs);
    bool IsMajorRoots() const
    {
        return YoungType() == ZYoungType::major_full_roots || YoungType() == ZYoungType::major_partial_roots;
    }
    void Begin(uint64_t index);
    void StartYoungMark(RememberedSet& rememberedSet);
    void PublishPhase(GCPhase value);
    void RecordYoungSequenceAtRelocateStart(uint64_t youngSequence);
    bool ActiveRemsetIsCurrent(uint64_t youngSequence) const;
    void End();
private:
    const GCCycleGeneration generation;
    std::unique_ptr<GCWorkers> workers;
    GCStats stats;
    ZStatCycle cycleStats;
    mutable std::mutex mutex;
    uint64_t sequence = 0;
    uint64_t requestIndex = 0;
    // ZGenerationOld::_young_seqnum_at_reloc_start (zGeneration.hpp:278).
    std::atomic<uint64_t> youngSequenceAtRelocateStart{ 0 };
    std::atomic<ZYoungType> youngType { ZYoungType::none };
    std::atomic<GCReason> reason { GC_REASON_USER };
    std::atomic<GCPhase> phase { GC_PHASE_IDLE };
    bool active = false;
};

// zGeneration.cpp:489-497: type is scoped to one young collection.
class YoungTypeSetter {
public:
    YoungTypeSetter(GenerationCycle& cycle, ZYoungType type);
    ~YoungTypeSetter();
    YoungTypeSetter(const YoungTypeSetter&) = delete;
    YoungTypeSetter& operator=(const YoungTypeSetter&) = delete;
private:
    GenerationCycle& cycle;
};

}
