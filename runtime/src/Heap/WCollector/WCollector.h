// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_WCOLLECTOR_H
#define MRT_WCOLLECTOR_H
#include "Common/ColourMask.h"
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "Allocator/RegionSpace.h"
#include "Collector/CopyCollector.h"
#include "Heap/Verify/DiffPathExplainer.h"
namespace MapleRuntime {

class ForwardTable {
public:
    explicit ForwardTable(RegionSpace& space) : theSpace(space) {}

    // if object is not relocated (forwarded or compacted), return nullptr.
    BaseObject* RouteObject(BaseObject* old)
    {
        BaseObject* toAddress = theSpace.RouteObject(old);
        return toAddress;
    }

    // if region is compacted, return false.
    bool RouteRegion(RegionInfo* region) { return theSpace.GetRegionManager().RouteRegion(region); }

    void PrepareForwardTable()
    {
        DLOG(FORWARD, "reset fwd table");
        theSpace.PrepareFromSpace();

        ForwardDataManager::GetForwardDataManager().ClearPreviousForwardData();
    }

    RegionSpace& theSpace;
};

using CrossRefHandler = void(*)(BaseObject*, BaseObject*);

class WCollector : public CopyCollector {
public:
    explicit WCollector(Allocator& allocator, CollectorResources& resources)
        : CopyCollector(allocator, resources), fwdTable(reinterpret_cast<RegionSpace&>(allocator))
    {
        collectorType = CollectorType::SMOOTH_COLLECTOR;
    }

    ~WCollector() override = default;

    void Init() override { ForwardDataManager::GetForwardDataManager().InitializeForwardData(); }

    void MarkNewObject(BaseObject* obj) override;

    bool ShouldIgnoreRequest(GCRequest& request) override;
    bool MarkObject(BaseObject* obj) const override;
    bool ResurrectObject(BaseObject* obj, size_t offset, RegionInfo* regionInfo) override;

    void EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const override;
    void TraceRefField(BaseObject* obj, RefField<>& ref, WorkStack& workStack) const;
    void TraceObjectRefFields(BaseObject* obj, WorkStack& workStack) override;
    BaseObject* GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field) override;
    BaseObject* ForwardObject(BaseObject* fromVersion) override;
    void PostResolveCycleTask();
    void PrepareCycleRef()
    {
        std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
        cycleRefWorkStack.insert(discoveredExternObjects.begin(), discoveredExternObjects.end());
        discoveredExternObjects.clear();
    }
    void MergeResurrectExportObjects()
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        resurrectedExportObjectes.insert(resurrectedExportObjectesForwardPhase.begin(),
            resurrectedExportObjectesForwardPhase.end());
        resurrectedExportObjectesForwardPhase.clear();
    }
    void FlipTagID() { currentTagID = static_cast<uint16_t>((currentTagID + 1) % TAG_ID_COUNT); }
    uint16_t GetCurrentTagID() override { return currentTagID; }
    uint16_t GetPreviousTagID() const
    {
        return static_cast<uint16_t>((currentTagID + TAG_ID_COUNT - 1) % TAG_ID_COUNT);
    }

    // Phase A of the ZGC-style colouring work (ops/design/G1_WRITE_BARRIER_DESIGN.md §3.6).
    //
    // Today a reference carries no colour unless it is being evacuated, so "needs the barrier"
    // is exactly "tagged", and every consumer spells that as one of the two predicates below.
    // Those two share a blind spot: an untagged value satisfies neither, so an if/else-if chain
    // over them lets it through unexamined. That is correct while good == 0 and wrong the moment
    // a good colour is non-zero, which is what phase C does.
    //
    // IsLoadBad is declared on Collector (Collector.h) so the six phase barriers, which hold a
    // Collector&, can spell it. Phase C changes that one body -- as in ZGC's
    // ZPointer::is_load_bad, zAddress.inline.hpp:626-628 -- instead of ~90 call sites.

    // The colour currently handed out. Flipping a phase swaps it and updates the mask the
    // compiler tests, which is what replaces walking the heap to strip stale colours.
    Uptr ZPointerRemappedYoungMask = ZPointerRemapped10 | ZPointerRemapped00;
    Uptr ZPointerRemappedOldMask = ZPointerRemapped01 | ZPointerRemapped00;
    Uptr currentRemapColour = ZPointerRemapped00;
    Uptr currentMarkedYoung = MARKED_YOUNG_0;
    Uptr currentMarkedOld = MARKED_OLD_0;
    size_t youngMarkFlipCount = 0;
    size_t oldMarkFlipCount = 0;

    // Mirrors ZGlobalsPointers::set_good_masks (OpenJDK zAddress.cpp:78-94): mark-good is
    // load-good plus the current epoch from each independent mark family.
    void set_good_masks()
    {
        currentRemapColour = ZPointerRemappedYoungMask & ZPointerRemappedOldMask;
        ::g_cjLoadBadMask = TAGGED_BITS_MASK | (REMAP_COLOUR_MASK ^ currentRemapColour);
        ::g_cjMarkBadMask = ::g_cjLoadBadMask | (MARKED_YOUNG_MASK & ~currentMarkedYoung) |
            (MARKED_OLD_MASK & ~currentMarkedOld);
    }

    // OpenJDK ZGlobalsPointers::flip_young_relocate_start/flip_old_relocate_start
    // (zAddress.cpp:138-151): each generation independently alternates the two accepted pairs.
    void flip_young_relocate_start()
    {
        ZPointerRemappedYoungMask ^= REMAP_COLOUR_MASK;
        set_good_masks();
    }

    void flip_old_relocate_start()
    {
        ZPointerRemappedOldMask ^= REMAP_COLOUR_MASK;
        set_good_masks();
    }

    // OpenJDK zAddress.cpp:132-146 flips each mark family only at that generation's mark start.
    void flip_young_mark_start()
    {
        currentMarkedYoung ^= MARKED_YOUNG_MASK;
        set_good_masks();
        if (++youngMarkFlipCount == 1) {
            LOG(RTLOG_ERROR, "[ZCOLOR2][mark-mask-flip] generation=young count=%zu g_cjMarkBadMask=%#lx",
                youngMarkFlipCount, ::g_cjMarkBadMask);
        }
    }

    void flip_old_mark_start()
    {
        currentMarkedOld ^= MARKED_OLD_MASK;
        set_good_masks();
        if (++oldMarkFlipCount == 1) {
            LOG(RTLOG_ERROR, "[ZCOLOR2][mark-mask-flip] generation=old count=%zu g_cjMarkBadMask=%#lx",
                oldMarkFlipCount, ::g_cjMarkBadMask);
        }
    }

    // note this api is not atomic, caller should take care of this.
    // N>2: any non-current tagged ref is "old" (not only previous); else older tags are dropped.
    bool IsOldPointer(RefField<>& ref) const override
    {
        return IsLoadBad(ref) && ref.GetTagID() != currentTagID;
    }

    // note this api is not atomic, caller should take care of this.
    bool IsCurrentPointer(RefField<>& ref) const override { return IsLoadBad(ref) && ref.GetTagID() == currentTagID; }

    // OpenJDK ZPointer::is_young_load_good/is_old_load_good
    // (zAddress.inline.hpp:648-655): the conceptual generation epoch is represented by the two
    // accepted bits in that generation's mask.
    bool is_young_load_good(RefField<>& ref) const override
    {
        // 凭什么 raw: 掩码测位型，不解引用。
        return (raw(ref.GetFieldValue()) & ZPointerRemappedYoungMask) != 0;
    }

    bool is_old_load_good(RefField<>& ref) const override
    {
        return (raw(ref.GetFieldValue()) & ZPointerRemappedOldMask) != 0;
    }

    // OpenJDK ZBarrier::remap_generation (zBarrier.inline.hpp:110-137): one generation-good
    // bit identifies the other generation; a double-bad colour consults the forwarding side table.
    ZGenerationId remap_generation(RefField<>& ref) const override
    {
        CHECK_DETAIL(!is_load_good(ref), "load-good reference does not need remap");
        bool youngLoadGood = is_young_load_good(ref);
        bool oldLoadGood = is_old_load_good(ref);
        if (oldLoadGood && !youngLoadGood) {
            return ZGenerationId::young;
        }
        if (youngLoadGood && !oldLoadGood) {
            return ZGenerationId::old;
        }

        BaseObject* target = to_object(ref.GetTargetObject());
        if (!Heap::IsHeapAddress(target)) {
            return ZGenerationId::old;
        }
        RegionInfo* forwarding = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(target));
        if (forwarding != nullptr && forwarding->generation_id() == ZGenerationId::young) {
            return ZGenerationId::young;
        }
        return ZGenerationId::old;
    }

    // OpenJDK ZGeneration::relocate_or_remap_object (zGeneration.inline.hpp:131-140): an address
    // outside the selected generation's forwarding table is already safe; a matching entry routes
    // to the current object. The generation check prevents an address-reuse alias from selecting a
    // route installed by the other generation.
    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId generation) const override
    {
        if (!Heap::IsHeapAddress(obj)) {
            return obj;
        }
        RegionInfo* forwarding = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
        if (forwarding == nullptr || forwarding->generation_id() != generation) {
            return obj;
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        BaseObject* to = space.GetRegionManager().RouteObject(obj, forwarding);
        return to == nullptr ? obj : to;
    }

    void AddRawPointerObject(BaseObject* obj) override
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        space.AddRawPointerObject(obj);
    }

    void RemoveRawPointerObject(BaseObject* obj) override
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        space.RemoveRawPointerObject(obj);
    }

    void ResolveCycleRef() override;

    // BaseObject* ForwardFixRefField(RefField<>& field) const;
    BaseObject* ForwardUpdateRawRef(ObjectRef& ref);

    bool IsFromObject(BaseObject* obj) const override
    {
        // filter const string object.
        if (Heap::IsHeapAddress(obj)) {
            auto regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
            return regionInfo->IsFromRegion();
        }

        return false;
    }

    bool IsGhostFromObject(BaseObject* obj) const override
    {
        // filter const string object.
        if (Heap::IsHeapAddress(obj)) {
            return RegionInfo::InGhostFromRegion(obj);
        }

        return false;
    }

    bool IsUnmovableFromObject(BaseObject* obj) const override;

    // this is called when caller assures from-object/from-region still exists.
    BaseObject* GetForwardPointer(BaseObject* fromObj, RegionInfo* region)
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        return space.GetRegionManager().RouteObject(fromObj, region);
    }

    BaseObject* FindToVersion(BaseObject* obj) const override
    {
        // Mirror IsGhostFromObject: GetGhostFromRegionAt → GetUnitIdxAt has no heap range
        // check, so null / non-heap (incl. colour-only null after flip) aborts as
        // "GetUnitIdxAt OOB addr=0". FixOldTaggedRefField then nulls the slot.
        // nullptr here is dual: non-heap/null gate OR no to-version for a heap from.
        // Soft-resolve paths must not CAS-null on the non-heap reading (RO static).
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
            return nullptr;
        }
        RegionInfo* fromRegionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
        if (fromRegionInfo == nullptr) {
            return nullptr;
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        return space.GetRegionManager().RouteObject(obj);
    }

protected:
    BaseObject* ForwardObjectImpl(BaseObject* obj, RegionInfo* ghostFromRegion);
    BaseObject* ForwardObjectExclusive(BaseObject* obj) override;

    bool TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const override;

    BaseObject* TryForwardObject(BaseObject* fromVersion);

    bool TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const override;
    bool TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const override;

    RefField<> GetAndTryTagRefField(BaseObject* target) const override
    {
        // Null carries no colour (ZGC zAddress: null is never load-bad). Colouring a null
        // yields a colour-only word; after a phase flip IsLoadBad becomes true while
        // tagID stays 0, so IsOldPointer can fire and ForwardUpdateRawRef's E-class
        // CHECK (oldInv) aborts on a value that is not an object at all.
        if (target == nullptr) {
            return RefField<>(static_cast<BaseObject*>(nullptr));
        }
        // Phase C: colour every reference, not only the ones being evacuated. The else branch used
        // to hand back a bare pointer, and that bare pointer was the trust state -- a value that
        // neither IsOldPointer nor IsCurrentPointer recognised, so readers dereferenced it without
        // asking anything. Handing out the current colour instead means a later phase flip turns
        // this reference bad on its own, and the reader finds out by testing the value it holds.
        if (IsFromObject(target)) {
            return RefField<>(target, 1, currentTagID,
                currentRemapColour | currentMarkedYoung | currentMarkedOld);
        }
        return RefField<>(target, 0, 0, currentRemapColour | currentMarkedYoung | currentMarkedOld);
    }

    // plainroots: root-slot write-back is plain (ZGC uncolored root); heap-slot write-back
    // keeps Phase C colour. Gate MRT_GCV2_PLAIN_ROOTS, on by default.
    //
    // It shipped off for two hours and that was wrong. The stated reason was that a
    // hang carries no si_code, no registers and no core and so is a worse base to
    // debug from -- but hangfloor then diagnosed the hang twice with the gate on,
    // reading per-cycle reclaim out of MRT_GC_LOG. The premise was false, and what
    // the default actually did was let the ledger record a known-wrong behaviour as
    // not-a-blocker.
    //
    // The rule is what ops/design/STACK_ROOTS_STAY_PLAIN.md rules and what ZGC does
    // (zUncoloredRoot.inline.hpp heals a root by writing back zaddress, with the
    // colour passed alongside as an argument). Colour in a register can only have
    // come from the collector's root write-back, which is why the same mechanism
    // surfaced as four separate-looking floor layers. Off means main keeps emitting
    // the defect those four fixes were about.
    //
    // Nothing that works today stops working: hello is 10/10 in all three arms. The
    // arm that hangs was already failing 10/10 as a SEGV.
    static bool PlainRootsEnabled()
    {
        static const bool on = []() {
            const char* v = std::getenv("MRT_GCV2_PLAIN_ROOTS");
            if (v == nullptr) {
                return true;
            }
            return !(v[0] == '0' && v[1] == '\0');
        }();
        return on;
    }

    RefField<> RootSlotWriteback(BaseObject* target, const void* slot) const
    {
        if (PlainRootsEnabled() && !Heap::IsHeapAddress(slot)) {
            return RefField<>(target);
        }
        return GetAndTryTagRefField(target);
    }

    void CollectLargeGarbage()
    {
        MRT_PHASE_TIMER("Collect large garbage");
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        GCStats& stats = GetGCStats();
        stats.largeSpaceSize = space.LargeObjectBytes();
        stats.largeGarbageSize = space.CollectLargeGarbage();
        stats.collectedBytes += stats.largeGarbageSize;
    }

    void CollectPinnedGarbage()
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        GCStats& stats = GetGCStats();
        stats.pinnedSpaceSize = space.PinnedSpaceSize();
        stats.pinnedGarbageSize = space.CollectPinnedGarbage();
        stats.collectedBytes += stats.pinnedGarbageSize;
    }

    void CollectSmallSpace();

    void DoGarbageCollection() override;
    void ProcessFinalizers() override;
    void EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const override;

private:
    using MinorObjectSet = std::unordered_set<BaseObject*>;
    using MinorRegionSet = std::unordered_set<RegionInfo*>;
    using MinorSlotSet = std::unordered_set<MAddress>;

    BaseObject* ResolveMinorReference(RefField<>& field) const;
    BaseObject* ResolveMinorReference(RootSlot& root) const;
    void VisitMinorRootSlots(RootVisitor& rawRootVisitor, uint64_t stackScanEpoch = 0);
    void VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor);
    void VisitMinorRoots(const std::function<void(BaseObject*)>& visitor, uint64_t stackScanEpoch = 0);
    // origin tags root source for invalid-minor-root diagnosis (gcbadroot).
    void PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin = "unknown") const;
    // setbitmap O1③: claim young via MarkObject (region mark bitmap) + collect vector;
    // FYS=0 skips reachableSlots inserts (slots never looked up). MRT_GCV2_SETBITMAP=0 → legacy set.
    void TraceYoungClosure(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                           std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                           MinorSlotSet& weakSlots, bool useBitmapLedger);
    void RescanRememberedSet(WorkStack& workStack, const MinorSlotSet& rememberedSlots,
                             const MinorSlotSet& reachableSlots, const MinorSlotSet& weakSlots, bool fullYoungScan,
                             MinorSlotSet* consumedOut = nullptr, DiffPathRemsetStats* statsOut = nullptr);
    bool FixMinorEvacuatedSlot(RefField<>& field) const;
    bool FixMinorEvacuatedSlot(RootSlot& root) const;
    void FixMinorRootSlots();
    void FixMinorRootSlotsParallel(GCThreadPool* threadPool);
    void FixMinorObjectSlots(BaseObject* object);
    void EvacuateYoungRegions(const std::vector<BaseObject*>& reachableVec, const MinorSlotSet& rememberedSlots);
    void ValidateYoungMarking(const std::vector<BaseObject*>& reachableVec, const MinorObjectSet& allocationRoots);
    // Report-only: find young objs full-reachable but unmarked; attribute via remset MISSING.
    // Gated by MRT_GCMARKGAP_PROBE=1 (default off).
    void ProbeUnmarkedLive(const MinorObjectSet& allocationRoots, const MinorSlotSet& rememberedSlots);
    void ValidateMinorReferences(const char* point, const std::vector<BaseObject*>* reachableVec);
    // Region-set structural verifier (Verify/VerifyRegions); gated by MRT_GCV2_VERIFY_REGIONS.
    void VerifyRegionSets(const char* point);
    void DoYoungGarbageCollection();
    void FlushAllocationRegions();
    template<bool forward>
    bool TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& ref, BaseObject*& oldRef, BaseObject*& newRef) const;
    void TraceHeap();
    // F3: rewrite IsOldPointer slots to plain/to so one-gen-stale tags cannot
    // outlive the route table (REPORT-tagaba F3).
    // requireSurvivedMark=true  → pre-dispel (PostTrace; marks still on from).
    // requireSurvivedMark=false → post-Flip after Forward (to-space has no marks).
    // Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94 + bfb5e8b24fa7c462321709c0c5af8290dccb38a6.
    void InvalidateOldTaggedRefsBeforeDispel();
    void InvalidateOldTaggedRefs(bool requireSurvivedMark);
    void FixOldTaggedRefField(BaseObject* holder, RefField<>& field);
    void PreforwardConcurrencyModelRoots();
    void PostTrace();
    void Preforward();
    void PreforwardAllExportFromRoots();
    void PreforwardStaticRoots();
    void PreforwardFinalizerProcessorRoots();
    void PreforwardDiscoveredExternObjects();
    void PreforwardAllResurrectExportFromObjects();
    CrossRefHandler GetCrossRefHandler(BaseObject* foreignProxy);

    ForwardTable fwdTable;
    // gc index 0 or 1 is used to distinguish previous gc and current gc.
    uint16_t currentTagID = 0;
    uint64_t minorTotalRuns = 0;
    MinorRegionSet minorCandidateRegions;
};
} // namespace MapleRuntime
#endif // ~MRT_WCOLLECTOR_H
