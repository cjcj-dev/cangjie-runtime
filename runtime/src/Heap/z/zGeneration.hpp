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
#include "Heap/z/zWeakRootsProcessor.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/Collector/GcStats.h"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zGenerationId.hpp"
#include "Heap/Collector/GcRequest.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSet.hpp"
#include "Heap/z/zRemembered.hpp"
namespace MapleRuntime {
class ZMark;
class ZRelocate;
class ZRelocationSetSelector;
enum class zaddress : Uptr;
struct TenuringInputs;
// Per-generation execution state. The snapshot lock publishes cycle identity
// and phase together; the phase atomic serves existing barrier readers.
// ZGC: zGeneration.hpp:65-78 (generation-owned phase and sequence).
enum class GCCycleGeneration : uint8_t { YOUNG, OLD };
enum class MarkStartPoint : uint8_t { Begin, BeforeRetire, BeforeSequence, BeforeDomain, BeforeRemembered, Complete };
struct GCCycleSnapshot {
    GCCycleGeneration generation;
    uint64_t sequence;
    uint64_t requestIndex;
    GCReason reason;
    GCPhase phase;
    bool active;
};
class WCollector;
struct YoungCollectionStats;
class ZGeneration;
class ZGenerationYoung;
class ZGenerationOld;

class ZGeneration {
protected:
    static ZGenerationYoung* _young;
    static ZGenerationOld* _old;

public:
    explicit ZGeneration(GCCycleGeneration generation);
    ~ZGeneration();
    ZGeneration(const ZGeneration&) = delete;
    ZGeneration& operator=(const ZGeneration&) = delete;
    ZGenerationId id() const;
    ZGenerationIdOptional id_optional() const;
    bool is_young() const;
    bool is_old() const;
    static ZGenerationYoung* young();
    static ZGenerationOld* old();
    static ZGeneration* generation(ZGenerationId id);
    uint32_t seqnum() const;
    GCCycleSnapshot Snapshot() const;
    ZMark& Mark() { return *mark; }
    const ZMark& Mark() const { return *mark; }
    ZMark* MarkPtr() { return mark.get(); }
    const ZMark* MarkPtr() const { return mark.get(); }
    enum class Phase { Mark, MarkComplete, Relocate };
    void set_phase(Phase new_phase);
    void log_phase_switch(Phase from, Phase to);
    bool is_phase_relocate() const { return _phase == Phase::Relocate; }
    bool is_phase_mark() const { return _phase == Phase::Mark; }
    bool is_phase_mark_complete() const { return _phase == Phase::MarkComplete; }
    const char* phase_to_string() const;
    bool IsPhaseMark() const;
    double FragmentationLimit() const;
    template<bool resurrect, bool gcThread, bool follow, bool finalizable>
    void MarkObject(zaddress address);
    template<bool resurrect, bool gcThread, bool follow, bool finalizable>
    void MarkObjectIfActive(zaddress address);
    // zGeneration.cpp:129 constructs _workers(id, &_stat_workers) by value with
    // the ZYoungGCThreads/ZOldGCThreads budget from zArguments. That budget is
    // computed after this object exists here, so the set is built when the
    // driver starts; StopWorkers is the matching end of that lifetime (see
    // ~WorkerThreads for why a destroy path exists at all).
    void InitializeWorkers(uint32_t capacity);
    void StopWorkers();
    ZWorkers* Workers() const { return workers.get(); }
    ZWeakRootsProcessor* WeakRootsProcessor() const { return weakRootsProcessor.get(); }
    GCStats& Stats() { return stats; }
    ZStatCycle& CycleStats() { return cycleStats; }
    ZStatWorkers* StatWorkers() { return &statWorkers; }
    GCPhase GcPhase() const { return phase.load(std::memory_order_acquire); }
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
    YoungCollectionStats StartYoungMark(WCollector& collector);
    void StartOldMark(WCollector& collector);
    void PublishPhase(GCPhase value);
    void RecordYoungSequenceAtRelocateStart(uint64_t youngSequence);
    bool ActiveRemsetIsCurrent(uint64_t youngSequence) const;
    ZRemembered* remembered() { return &_remembered; }
    const ZRemembered* remembered() const { return &_remembered; }
    void register_with_remset(ZPage* page) { _remembered.register_found_old(page); }
    void End();
    ZForwardingTable& forwarding_table() { return _forwarding_table; }
    const ZForwardingTable& forwarding_table() const { return _forwarding_table; }
    ZRelocationSet& relocation_set() { return _relocation_set; }
    ZRelocate& relocate() { return *_relocate; }
    ZForwarding* forwarding(MAddress addr) const { return addr == 0 ? nullptr : _forwarding_table.get(addr); }
    void reset_relocation_set();
    void free_empty_pages(ZRelocationSetSelector* selector, int bulk);
    void flip_age_pages(const ZRelocationSetSelector* selector);
    void select_relocation_set(bool promote_all);
    private:
#if defined(MRT_GENERATION_SEQUENCE_FIXTURE)
    friend struct GenerationSequenceFixture;
#endif
    std::unique_ptr<ZMark> mark;
    const GCCycleGeneration generation;
    std::unique_ptr<ZWorkers> workers;
    std::unique_ptr<ZWeakRootsProcessor> weakRootsProcessor;
    GCStats stats;
    ZStatCycle cycleStats;
    // zGeneration.hpp:_stat_workers, constructed before _workers points at it.
    ZStatWorkers statWorkers;
    mutable std::mutex mutex;
    // ZGeneration::ZGeneration (zGeneration.cpp:137): _seqnum(1). ZLiveMap uses
    // seqnum 0 as "never marked" (zLiveMap.cpp:40, zLiveMap.inline.hpp:37-43),
    // so no generation may ever report sequence 0.
    uint64_t sequence = 1;
    uint64_t requestIndex = 0;
    // ZGenerationOld::_young_seqnum_at_reloc_start (zGeneration.hpp:278).
    std::atomic<uint64_t> youngSequenceAtRelocateStart{ 0 };
    std::atomic<ZYoungType> youngType { ZYoungType::none };
    std::atomic<GCReason> reason { GC_REASON_USER };
    std::atomic<GCPhase> phase { GC_PHASE_IDLE };
    ZGeneration::Phase _phase { ZGeneration::Phase::Relocate };
    bool active = false;
    ZForwardingTable _forwarding_table;
    ZRelocationSet _relocation_set;
    std::unique_ptr<ZRelocate> _relocate;
    ZRemembered _remembered;
};

// zGeneration.cpp:489-497: type is scoped to one young collection.
class YoungTypeSetter {
public:
    YoungTypeSetter(ZGeneration& cycle, ZYoungType type);
    ~YoungTypeSetter();
    YoungTypeSetter(const YoungTypeSetter&) = delete;
    YoungTypeSetter& operator=(const YoungTypeSetter&) = delete;
private:
    ZGeneration& cycle;
};

class ZGenerationYoung : public ZGeneration {
public:
    ZGenerationYoung();
    void collect(WCollector& collector);
    void pause_mark_start(WCollector& collector);
    void concurrent_mark(WCollector& collector);
    bool pause_mark_end(WCollector& collector);
    void concurrent_mark_continue(WCollector& collector);
    void concurrent_mark_free();
    void concurrent_reset_relocation_set();
    void concurrent_select_relocation_set();
    void pause_relocate_start(WCollector& collector);
    void concurrent_relocate(WCollector& collector);
};

class ZGenerationOld : public ZGeneration {
public:
    ZGenerationOld();
    void collect(WCollector& collector);
    void concurrent_mark(WCollector& collector);
    bool pause_mark_end(WCollector& collector);
    void concurrent_mark_continue(WCollector& collector);
    void concurrent_mark_free();
    void concurrent_process_non_strong_references(WCollector& collector);
    void concurrent_reset_relocation_set();
    void pause_verify(WCollector& collector);
    void concurrent_select_relocation_set();
    void concurrent_remap_young_roots(WCollector& collector);
    void pause_relocate_start(WCollector& collector);
    void concurrent_relocate(WCollector& collector);
};

}
