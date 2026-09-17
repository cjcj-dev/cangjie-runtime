// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_H
#define MRT_COLLECTOR_H

#include "Heap/z/zAddress.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_set>
#include <vector>

#include "Base/Macros.h"
#include "Heap/Collector/GcRequest.h"
#include "Heap/Collector/GcStats.h"

#include "Heap/z/zGeneration.hpp"
#include "Heap/Collector/Collector.h"
namespace MapleRuntime {
enum class Generation : uint8_t;
enum CollectorType {
    NO_COLLECTOR = 0, // No Collector
    PROXY_COLLECTOR,  // Proxy of Collector
    COPY_COLLECTOR,   // Regional-Copying GC
    SMOOTH_COLLECTOR, // wgc
    COLLECTOR_TYPE_COUNT,
};

class Collector {
public:
    Collector();
    virtual ~Collector();

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

    virtual GCPhase GetGCPhase(GCCycleGeneration generation) const { return GetCycleSnapshot(generation).phase; }

    virtual void SetGCPhase(GCCycleGeneration generation, const GCPhase phase)
    {
        PublishGenerationPhase(generation, phase);
    }

    virtual ZGeneration& GetZGeneration(GCCycleGeneration generation)
    {
        if (generation == GCCycleGeneration::YOUNG) {
            return youngCycle;
        }
        return oldCycle;
    }

    virtual const ZGeneration& GetZGeneration(GCCycleGeneration generation) const
    {
        if (generation == GCCycleGeneration::YOUNG) {
            return youngCycle;
        }
        return oldCycle;
    }
    ZGeneration& GetZGeneration(Generation generation)
    {
        return GetZGeneration(generation == Generation::Young ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD);
    }
    const ZGeneration& GetZGeneration(Generation generation) const
    {
        return GetZGeneration(generation == Generation::Young ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD);
    }

    virtual GCCycleSnapshot GetCycleSnapshot(GCCycleGeneration generation) const
    {
        return GetZGeneration(generation).Snapshot();
    }
    virtual void PublishGenerationPhase(GCCycleGeneration generation, GCPhase value);
    bool OldActiveRemsetIsCurrent() const
    {
        return GetZGeneration(GCCycleGeneration::OLD).ActiveRemsetIsCurrent(
            GetZGeneration(GCCycleGeneration::YOUNG).Sequence());
    }
    Generation ObjectGeneration(BaseObject* object) const;

    // determine how we treat new object during gc.
    virtual void MarkNewObject(BaseObject*) {}
    void MarkObjectIfActive(BaseObject* object) const;
    virtual void MarkYoungObjectIfActive(BaseObject*) const
    {
        AbortUnimplemented("Collector::MarkYoungObjectIfActive");
    }
    virtual void MarkYoungRootObject(BaseObject*) const
    {
        AbortUnimplemented("Collector::MarkYoungRootObject");
    }
    virtual void MarkOldObjectIfActive(BaseObject*, bool = false) const
    {
        AbortUnimplemented("Collector::MarkOldObjectIfActive");
    }

    virtual void FixObject(BaseObject&) const {}

    virtual void RunGarbageCollection(uint64_t, GCReason) = 0;

    // Named aborts: virtual defaults for collectors that do not implement this method.
    // Bodies live in Collector.cpp so headers stay free of FormatLog / string literals.
    [[noreturn]] static void AbortUnimplemented(const char* method);

    // loadfc: shared hand-out verdict (header word only, one relaxed load). Out-of-line because
    // Heap::IsHeapAddress lives behind Barrier.h which must not be included from here.
    static HandVerdict JudgeHandOutTarget(BaseObject* target);
    // Emit the gated NeverInstalled identity record from one relaxed raw-header
    // read and one ownership-protected carrier snapshot. Returns its event id.
    static uint64_t EmitNeverInstalledDiagnostic(BaseObject* target, uintptr_t rawSlotBits,
                                                 MAddress witnessStart, uint64_t witnessEpoch,
                                                 uint64_t witnessLife, bool witnessValid);
    // loadfc: the loud failure for "resolution failed and the from-address is not Usable"
    // (0825 用户令: no silent fold-back to the original address).
    [[noreturn]] static void FailClosedLoad(const char* site, BaseObject* target, uintptr_t slotBits,
                                            const ForwardingProvenance& provenance);

    virtual GCStats& GetGCStats(GCCycleGeneration generation = GCCycleGeneration::OLD)
    {
        return GetZGeneration(generation).Stats();
    }

    virtual BaseObject* ForwardObject(BaseObject*, Generation) { AbortUnimplemented("Collector::ForwardObject"); }

    virtual bool ShouldIgnoreRequest(GCRequest& quest) = 0;
    virtual bool IsFromObject(BaseObject*) const { AbortUnimplemented("Collector::IsFromObject"); }
    virtual bool IsGhostFromObject(BaseObject*) const { AbortUnimplemented("Collector::IsGhostFromObject"); }
    virtual bool IsUnmovableFromObject(BaseObject*) const
    {
        AbortUnimplemented("Collector::IsUnmovableFromObject");
    }
    // Every miss has a public state. Consumers may treat NotManaged and
    // NotForwarded as their existing soft misses; Unavailable must never fall
    // through to object-field access.
    virtual FindToVersionResult FindToVersion(BaseObject* obj, Generation generation) const = 0;

    // OpenJDK zBarrier.inline.hpp:695-716 store_barrier / color_store_good:
    // a stored reference must already be the current version (remap included).
    // New raw values are already current; validate without selecting a forwarding map.
    BaseObject* ValidateCurrentValue(BaseObject* ref, const ForwardingProvenance& provenance) const;

    virtual BaseObject* ResolveStoreValue(BaseObject* ref, const ForwardingProvenance& provenance,
                                          Generation generation) const
    {
        return relocate_or_remap_object(ref, static_cast<ZGenerationId>(generation), provenance);
    }

    virtual bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const
    {
        AbortUnimplemented("Collector::TryUpdateRefField");
    }
    virtual bool TryUpdateRefFieldWithProvenance(BaseObject* obj, RefField<>& field, BaseObject*& to,
                                                 const ForwardingProvenance&) const
    {
        return TryUpdateRefField(obj, field, to);
    }
    virtual bool TryForwardRefField(BaseObject*, RefField<>&, BaseObject*&) const
    {
        AbortUnimplemented("Collector::TryForwardRefField");
    }
    virtual bool TryUntagRefField(BaseObject*, RefField<>&, BaseObject*&) const
    {
        AbortUnimplemented("Collector::TryUntagRefField");
    }
    virtual bool TryTagRefField(BaseObject*, RefField<>&, BaseObject*) const
    {
        AbortUnimplemented("Collector::TryTagRefField");
    }
    virtual Uptr CurrentRemapColourForProbe() const { return 0; }

    virtual bool IsMarkedObjectForProbe(BaseObject*) const { return false; }

    // healfp: lossy fingerprint of "this slot address was healed by the mark walk this process".
    // Volume alone cannot answer whether a *particular* stale slot was ever visited -- TraceRefField
    // heals >=524k slots per run, so a big number proves breadth, not coverage of the one slot that
    // went stale.  A bit that is *clear* is strong evidence the slot was never healed; a bit that is
    // set is weak (hash collisions), which is why the same query is also run on ordinary hand-outs
    // to get the collision baseline.
    static constexpr size_t kHealFpBits = 1u << 22; // 4 Mbit = 512 KiB
    static uint64_t* HealFpTable()
    {
        static uint64_t table[kHealFpBits / 64] = {};
        return table;
    }
    static size_t HealFpIndex(uintptr_t slot)
    {
        // slots are 8-byte aligned; mix the high bits down so regions do not alias wholesale
        const uintptr_t h = (slot >> 3) ^ (slot >> 23) ^ (slot >> 41);
        return static_cast<size_t>(h) & (kHealFpBits - 1);
    }
    static void HealFpMark(uintptr_t slot)
    {
        const size_t i = HealFpIndex(slot);
        __atomic_fetch_or(&HealFpTable()[i / 64], uint64_t(1) << (i % 64), __ATOMIC_RELAXED);
    }
    static bool HealFpTest(uintptr_t slot)
    {
        const size_t i = HealFpIndex(slot);
        return (__atomic_load_n(&HealFpTable()[i / 64], __ATOMIC_RELAXED) & (uint64_t(1) << (i % 64))) != 0;
    }

    virtual RefField<> GetAndTryTagRefField(BaseObject*) const
    {
        AbortUnimplemented("Collector::GetAndTryTagRefField");
    }
    virtual RefField<> GetAndTryTagRefFieldWithProvenance(BaseObject* obj,
                                                          const ForwardingProvenance&) const
    {
        return GetAndTryTagRefField(obj);
    }

    // "Does this reference need the barrier before use?" -- the question every consumer of the
    // two predicates below is actually asking. Today a reference carries no colour unless it is
    // being evacuated, so the answer was a pointer tag bit; phase C of the colouring work
    // (ops/design/G1_WRITE_BARRIER_DESIGN.md §3.6) makes it a mask test. Non-virtual and phase
    // independent: the encoding is a property of RefField, not of the collector's phase.
    // Phase C: the value now says whether it may be stale. A reference is good when it carries
    // the colour the collector is currently handing out and is not mid-evacuation; anything else
    // -- an older colour, or a tagged reference -- has to go through the barrier. One AND, matching
    // what the compiler emits (CJBarrierLowering.cpp:653) and what ZGC does
    // (jdk zBarrier.inline.hpp:626-628).
    //
    // A zero field passes, as it does in ZGC: null carries no colour, and every stored reference
    // is coloured on the way in, so the only uncoloured values are the ones that were never
    // written (jdk zAddress.inline.hpp:635-643 makes the same trade deliberately).
    bool IsLoadBad(RefField<>& ref) const
    {
        // 凭什么 raw: 掩码测的是槽位位型，不是解引用。
        return (raw(ref.GetFieldValue()) & ::g_cjLoadBadMask) != 0;
    }


    // ZPointer::is_load_good (zAddress.inline.hpp:631-633). Keep the product
    // predicate in the collector domain; diagnostic mode selection must not own it.

    virtual ZGenerationId remap_generation(RefField<>&) const
    {
        AbortUnimplemented("Collector::remap_generation");
    }
    virtual BaseObject* relocate_or_remap_object(BaseObject*, ZGenerationId) const
    {
        AbortUnimplemented("Collector::relocate_or_remap_object");
    }
    virtual BaseObject* relocate_or_remap_object(
        BaseObject* object, ZGenerationId generation, const ForwardingProvenance&) const
    {
        return relocate_or_remap_object(object, generation);
    }

    // make_load_good: 带色槽 → 可解引用对象。内部仍返 BaseObject* 以兼容现有调用面；
    // 新代码应经 to_object(zaddress) 出口。
    // tipnull barriernull: live non-null ref must never become nullptr for mutator
    // (cjpm+0x31061a test [rax+0xc] after CJ_MCC_ReadRefField with rax=0).
    //
    // loadfc scope note (0826 measured, kkk2 gate nwdet e_256MB.txt): make_load_good is NOT
    // exclusively a mutator hand-out funnel -- the GC mark walk calls it too (Mark.cpp) and
    // already owns a tolerance policy for cleared from-addresses ([MARKSTALE]; dozens per cycle
    // are routine). Failing loudly here killed a green nwdet wave from inside mark, at no
    // hand-out. Slow/runtime exits close with Barrier::FinalizeLoadForMutator; the finalizer
    // consumer now uses the public ReadStaticRef runtime path instead of this helper. Compiler
    // colour-good fast paths retain ZGC's direct-uncolour form and depend on the producer-side
    // colour/lifetime invariant.
    BaseObject* make_load_good(RefField<>& ref, const ForwardingProvenance& provenance) const
    {
        // 凭什么 to_object: GetTargetObject 已剥色；null 或 load-good 可直接用。
        BaseObject* target = to_object(ref.GetTargetObject());
        if (target == nullptr || ZPointer::is_load_good(ref.GetFieldValue())) {
            return target;
        }

        // ZGC's relocate_or_remap_object has no "return the from oop" exit
        // (zRelocate.cpp:382-416).  A failed lookup is therefore represented
        // as null for internal walkers; callers that hand values to mutators
        // must use the load-barrier slow path, which fails closed rather than
        // laundering the unresolved address into a load-good value.
        return relocate_or_remap_object(target, remap_generation(ref), provenance);
    }

    // OpenJDK ZPointer::is_mark_good (zAddress.inline.hpp:658-664): mark-good includes load-good,
    // the current young mark epoch, and the current old mark epoch; raw null is not mark-good.
    //
    // Encoding completeness is enforced at HeapSlot publication. As in ZGC,
    // this phase predicate is only the single not-bad-mask test.

    // OpenJDK ZPointer::is_store_good (zAddress.inline.hpp:679-684): store-good includes
    // mark-good plus the current Remembered epoch bit. Fast path for write barrier.


    virtual bool IsOldPointer(RefField<>&) const { AbortUnimplemented("Collector::IsOldPointer"); }
    virtual bool IsCurrentPointer(RefField<>&) const { AbortUnimplemented("Collector::IsCurrentPointer"); }
    virtual void AddRawPointerObject(BaseObject*) { AbortUnimplemented("Collector::AddRawPointerObject"); }
    // Pin for callers that hand the pinned payload out (MCC_AcquireRawData): the pin may
    // resolve a movable from-copy to its to-version first, and the caller MUST adopt the
    // returned pointer — Inc lands on the resolved object's region, so releasing through
    // the original from payload would Dec a region that was never Inc'd (underflow) and
    // hand C a payload the collector is about to relocate.
    virtual BaseObject* PinRawPointerObject(BaseObject* obj)
    {
        AddRawPointerObject(obj);
        return obj;
    }
    virtual void RemoveRawPointerObject(BaseObject*)
    {
        AbortUnimplemented("Collector::RemoveRawPointerObject");
    }
    virtual void ResolveCycleRef() { AbortUnimplemented("Collector::ResolveCycleRef"); }

    // F5: to==nullptr must not silently return a dead/zeroed from (REPORT-tagaba F5).
    // Implementation in Collector.cpp — needs complete BaseObject + CHECK_DETAIL.
    // Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94.
    BaseObject* FindLatestVersion(BaseObject* obj, const ForwardingProvenance& provenance, Generation generation) const;

protected:
    virtual void RequestGCInternal(GCReason, bool) { AbortUnimplemented("Collector::RequestGCInternal"); }

    CollectorType collectorType = CollectorType::NO_COLLECTOR;
    ZGenerationYoung youngCycle;
    ZGenerationOld oldCycle;
};
} // namespace MapleRuntime

#endif // MRT_COLLECTOR_H
