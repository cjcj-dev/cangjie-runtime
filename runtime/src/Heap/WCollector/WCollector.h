// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_WCOLLECTOR_H
#define MRT_WCOLLECTOR_H
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
    void FlipTagID() { currentTagID ^= 1; }
    uint16_t GetCurrentTagID() override { return currentTagID; }
    uint16_t GetPreviousTagID() const { return currentTagID ^ 1; }

    // note this api is not atomic, caller should take care of this.
    bool IsOldPointer(RefField<>& ref) const override { return ref.IsTagged() && ref.GetTagID() == GetPreviousTagID(); }

    // note this api is not atomic, caller should take care of this.
    bool IsCurrentPointer(RefField<>& ref) const override { return ref.IsTagged() && ref.GetTagID() == currentTagID; }

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
        if (IsFromObject(target)) {
            return RefField<>(target, 1, currentTagID);
        } else {
            return RefField<>(target);
        }
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
    void TraceYoungClosure(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                           MinorSlotSet& reachableSlots, MinorSlotSet& weakSlots);
    void RescanRememberedSet(WorkStack& workStack, const MinorSlotSet& rememberedSlots,
                             const MinorSlotSet& reachableSlots, const MinorSlotSet& weakSlots, bool fullYoungScan,
                             MinorSlotSet* consumedOut = nullptr, DiffPathRemsetStats* statsOut = nullptr);
    bool FixMinorEvacuatedSlot(RefField<>& field) const;
    void FixMinorRootSlots();
    void FixMinorObjectSlots(BaseObject* object);
    void EvacuateYoungRegions(const MinorObjectSet& reachableObjects, const MinorSlotSet& rememberedSlots);
    void ValidateYoungMarking(const MinorObjectSet& reachableObjects, const MinorObjectSet& allocationRoots);
    // Report-only: find young objs full-reachable but unmarked; attribute via remset MISSING.
    // Gated by MRT_GCMARKGAP_PROBE=1 (default off).
    void ProbeUnmarkedLive(const MinorObjectSet& allocationRoots, const MinorSlotSet& rememberedSlots);
    void ValidateMinorReferences(const char* point, const MinorObjectSet* reachableObjects);
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
