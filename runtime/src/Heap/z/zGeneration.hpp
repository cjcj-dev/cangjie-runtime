#pragma once
#include <atomic>
#include <mutex>
#include "Heap/z/zGlobals.hpp"
#include "Heap/Collector/GcRequest.h"
namespace MapleRuntime {
class RememberedSet;
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
    GCPhase Phase() const { return phase.load(std::memory_order_acquire); }
    uint64_t Sequence() const { return Snapshot().sequence; }
    GCReason Reason() const { return reason.load(std::memory_order_acquire); }
    void SelectReason(GCReason value);
    void Begin(uint64_t index);
    void StartYoungMark(RememberedSet& rememberedSet);
    void PublishPhase(GCPhase value);
    void RecordYoungSequenceAtRelocateStart(uint64_t youngSequence);
    bool ActiveRemsetIsCurrent(uint64_t youngSequence) const;
    void End();
private:
    const GCCycleGeneration generation;
    mutable std::mutex mutex;
    uint64_t sequence = 0;
    uint64_t requestIndex = 0;
    // ZGenerationOld::_young_seqnum_at_reloc_start (zGeneration.hpp:278).
    std::atomic<uint64_t> youngSequenceAtRelocateStart{ 0 };
    std::atomic<GCReason> reason { GC_REASON_USER };
    std::atomic<GCPhase> phase { GC_PHASE_IDLE };
    bool active = false;
};

}
