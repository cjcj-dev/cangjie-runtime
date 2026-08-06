// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_H
#define MRT_COLLECTOR_H

#include "Common/ColourMask.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <set>
#include <vector>

#include "GcRequest.h"
#include "GcStats.h"

namespace MapleRuntime {
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

enum CollectorType {
    NO_COLLECTOR = 0, // No Collector
    PROXY_COLLECTOR,  // Proxy of Collector
    COPY_COLLECTOR,   // Regional-Copying GC
    SMOOTH_COLLECTOR, // wgc
    COLLECTOR_TYPE_COUNT,
};

// Central garbage identification algorithm.
class Collector {
public:
    Collector();
    virtual ~Collector() = default;

    static const char* GetGCPhaseName(GCPhase phase);

    // Initializer and finalizer.
    virtual void Init() = 0;
    virtual void Fini() {}
    const char* GetCollectorName() const;

    // This pure virtual function implements the trigger of GC.
    // reason: Reason for GC.
    // async:  Trigger from unsafe context, e.g., holding a lock, in the middle of an allocation.
    //         In order to prevent deadlocks, async trigger only add one async gc task and will not block.
    void RequestGC(GCReason reason, bool async);

    virtual GCPhase GetGCPhase() const { return gcPhase.load(std::memory_order_acquire); }

    virtual void SetGCPhase(const GCPhase phase) { gcPhase.store(phase, std::memory_order_release); }

    // determine how we treat new object during gc.
    virtual void MarkNewObject(BaseObject*) {}

    virtual void FixObject(BaseObject&) const {}

    virtual void RunGarbageCollection(uint64_t, GCReason) = 0;

    virtual GCStats& GetGCStats() { std::abort(); }

    virtual BaseObject* ForwardObject(BaseObject*) { std::abort(); }

    virtual bool ShouldIgnoreRequest(GCRequest& quest) = 0;
    virtual bool IsFromObject(BaseObject*) const { std::abort(); }
    virtual bool IsGhostFromObject(BaseObject*) const { std::abort(); }
    virtual bool IsUnmovableFromObject(BaseObject*) const { std::abort(); }
    virtual BaseObject* FindToVersion(BaseObject* obj) const = 0;

    virtual bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const { std::abort(); }
    virtual bool TryForwardRefField(BaseObject*, RefField<>&, BaseObject*&) const { std::abort(); }
    virtual bool TryUntagRefField(BaseObject*, RefField<>&, BaseObject*&) const { std::abort(); }
    virtual bool TryTagRefField(BaseObject*, RefField<>&, BaseObject*) const { std::abort(); }
    virtual RefField<> GetAndTryTagRefField(BaseObject*) const { std::abort(); }

    // "Does this reference need the barrier before use?" -- the question every consumer of the
    // two predicates below is actually asking. Today a reference carries no colour unless it is
    // being evacuated, so the answer is exactly IsTagged(); phase C of the colouring work
    // (ops/design/G1_WRITE_BARRIER_DESIGN.md §3.6) makes it a mask test. Non-virtual and phase
    // independent: the encoding is a property of RefField, not of the collector's phase.
    // Phase C: the value now says whether it may be stale. A reference is good when it carries
    // the colour the collector is currently handing out and is not mid-evacuation; anything else
    // -- an older colour, or a tagged reference -- has to go through the barrier. One AND, matching
    // what the compiler emits (CJBarrierLowering.cpp:641) and what ZGC does
    // (jdk zBarrier.inline.hpp:626-628).
    //
    // A zero field passes, as it does in ZGC: null carries no colour, and every stored reference
    // is coloured on the way in, so the only uncoloured values are the ones that were never
    // written (jdk zAddress.inline.hpp:635-643 makes the same trade deliberately).
    bool IsLoadBad(RefField<>& ref) const
    {
        return (ref.GetFieldValue() & ::g_cjLoadBadMask) != 0;
    }

    // OpenJDK ZPointer::is_mark_good (zAddress.inline.hpp:658-664): mark-good includes load-good,
    // the current young mark epoch, and the current old mark epoch; raw null is not mark-good.
    bool is_mark_good(RefField<>& ref) const
    {
        return (ref.GetFieldValue() & ::g_cjMarkBadMask) == 0 && ref.GetFieldValue() != 0;
    }

    virtual bool IsOldPointer(RefField<>&) const { std::abort(); }
    virtual bool IsCurrentPointer(RefField<>&) const { std::abort(); }
    virtual void AddRawPointerObject(BaseObject*) { std::abort(); }
    virtual void RemoveRawPointerObject(BaseObject*) { std::abort(); }
    virtual void ResolveCycleRef() { std::abort(); }

    // F5: to==nullptr must not silently return a dead/zeroed from (REPORT-tagaba F5).
    // Implementation in Collector.cpp — needs complete BaseObject + CHECK_DETAIL.
    // Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94.
    BaseObject* FindLatestVersion(BaseObject* obj) const;

protected:
    virtual void RequestGCInternal(GCReason, bool) { std::abort(); }

    CollectorType collectorType = CollectorType::NO_COLLECTOR;
    std::atomic<GCPhase> gcPhase = { GCPhase::GC_PHASE_IDLE };
};
} // namespace MapleRuntime

#endif // MRT_COLLECTOR_H
