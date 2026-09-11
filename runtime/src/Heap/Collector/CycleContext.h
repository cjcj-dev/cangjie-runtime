// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_CYCLE_CONTEXT_H
#define MRT_CYCLE_CONTEXT_H

#include <atomic>
#include "GcStats.h"

namespace MapleRuntime {
struct GCDriverRequest;
// GCPhase describes phases for stw/concurrent gc.
enum GCPhase : uint8_t {
    GC_PHASE_UNDEF = 0,
    GC_PHASE_IDLE = 1,
    GC_PHASE_FINISH = 2,
    GC_PHASE_RECLAIM_SATB_NODE = 3,
    GC_PHASE_INIT = 8,

    // only gc phase after GC_PHASE_INIT ( enum value > GC_PHASE_INIT) needs barrier.
    GC_PHASE_ENUM = 9,
    GC_PHASE_TRACE = 10,
    GC_PHASE_CLEAR_SATB_BUFFER = 11,
    GC_PHASE_POST_TRACE = 12,
    GC_PHASE_PREFORWARD = 13,
    GC_PHASE_FORWARD = 14,
};

// Execution generation is independent of the request cause and page lifetime.
// The old executor still performs the existing full-heap closure until I11/I12.
enum class CycleGeneration : uint8_t { Young, Old };
constexpr size_t CycleSlot(CycleGeneration generation) { return static_cast<size_t>(generation); }

// A slot address is stable across collections and therefore is not a cycle
// identity.  Callers that can publish or complete a cycle retain this value
// token so a delayed operation cannot act on a later use of the same slot.
struct CycleToken {
    CycleGeneration generation;
    uint64_t sequence;
};

// zGeneration.hpp:65-87: the generation owns its phase, mark work and statistics.
// Stable storage belongs to CollectorResources. Consumers carrying work must
// carry a CycleToken; the next use of a slot is a new cycle.
struct CycleContext {
    explicit CycleContext(CycleGeneration owner) : generation(owner) {}
    const CycleGeneration generation;
    std::atomic<uint64_t> sequence { 0 };
    GCReason reason = GC_REASON_USER;
    const GCDriverRequest* request = nullptr; // Borrowed only during the executing driver call.
    uint64_t taskIndex = 0; // Observation only; DriverPort owns the request receipt.
    GCStats stats;

    bool IsYoung() const { return generation == CycleGeneration::Young; }
};

// One acquire load describes all published phase obligations. Updating one
// generation preserves the other generation's bits, including at cycle end.
class CycleSnapshot {
public:
    explicit CycleSnapshot(uint32_t value = 0) : bits(value) {}
    bool Active(CycleGeneration generation) const { return (bits & ActiveBit(generation)) != 0; }
    GCPhase Phase(CycleGeneration generation) const
    {
        const uint32_t phase = (bits >> (CycleSlot(generation) * 8)) & 0x7f;
        return phase == 0 ? GC_PHASE_IDLE : static_cast<GCPhase>(phase);
    }
    bool Marking(CycleGeneration generation) const
    {
        const GCPhase phase = Phase(generation);
        return Active(generation) && (phase == GC_PHASE_ENUM || phase == GC_PHASE_TRACE ||
                                      phase == GC_PHASE_CLEAR_SATB_BUFFER);
    }
    bool AnyActive() const { return Active(CycleGeneration::Young) || Active(CycleGeneration::Old); }
    bool AnyMarking() const { return Marking(CycleGeneration::Young) || Marking(CycleGeneration::Old); }
    static constexpr uint32_t ActiveBit(CycleGeneration generation) { return 0x80u << (CycleSlot(generation) * 8); }
    static constexpr uint32_t SlotMask(CycleGeneration generation) { return 0xffu << (CycleSlot(generation) * 8); }
private:
    uint32_t bits;
};
} // namespace MapleRuntime
#endif
