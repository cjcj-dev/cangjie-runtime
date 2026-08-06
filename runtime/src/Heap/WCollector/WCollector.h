// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_WCOLLECTOR_H
#define MRT_WCOLLECTOR_H
#include "Common/ColourMask.h"
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

    void FlipRemapColour()
    {
        ZPointerRemappedYoungMask ^= REMAP_COLOUR_MASK;
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
        return (ref.GetFieldValue() & ZPointerRemappedYoungMask) != 0;
    }

    bool is_old_load_good(RefField<>& ref) const override
    {
        return (ref.GetFieldValue() & ZPointerRemappedOldMask) != 0;
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
    void VisitMinorRootSlots(RootVisitor& rawRootVisitor, const RefFieldVisitor& fieldVisitor);
    void VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor);
    void VisitMinorRoots(const std::function<void(BaseObject*)>& visitor);
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
