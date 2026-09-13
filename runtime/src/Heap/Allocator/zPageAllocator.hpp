// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGION_MANAGER_H
#define MRT_REGION_MANAGER_H

#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <map>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

#include "AllocBuffer.h"
#include "AllocationStallQueue.h"
#include "Allocator.h"
#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/ColourEncoding.h"
#include "RoutePublish.h"
#include "Common/RunType.h"
#include "FreeRegionManager.h"
#include "RangeRegistry.h"
#include "PageAge.h"
#include "Heap/GcThreadPool.h"
#include "Heap/Collector/RelocationRequestQueue.h"
#include "RegionList.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/TraceClear.h"
#include "securec.h"
#include "SlotList.h"
#include "Sync/Sync.h"

namespace MapleRuntime {
class CopyCollector;
class CompactCollector;
class VerifyRegions;
class WCollector;
template<Generation G>
class ForwardTask;

struct YoungCollectionStats {
    size_t candidateRegions = 0;
    size_t candidateBytes = 0;
    size_t reclaimedRegions = 0;
    size_t reclaimedBytes = 0;

    // candfix: PrepareYoungGarbageCandidates selects regions and mutates region lists;
    // it does not visit heap objects or reference slots. Keep the input inventory and
    // skip reasons explicit so a long phase can be classified as "many entries" vs
    // "expensive per entry" without adding an object walk merely for measurement.
    size_t fromVisited = 0;
    size_t fromVisitedUnits = 0;
    size_t unmovableVisited = 0;
    size_t unmovableVisitedUnits = 0;
    size_t unmovableYoung = 0;
    size_t unmovableHeld = 0;
    size_t recentFullVisited = 0;
    size_t recentFullVisitedUnits = 0;
    size_t recentFullYoung = 0;
    size_t recentFullHeld = 0;
    size_t clearLiveRegions = 0;
    size_t clearLiveUnits = 0;
    size_t objectVisits = 0;
    size_t slotVisits = 0;
    uint64_t reparkNs = 0;
    uint64_t unmovableNs = 0;
    uint64_t recentFullNs = 0;
    uint64_t holdCheckNs = 0;
    uint64_t clearLiveNs = 0;
    uint64_t visitorNs = 0;
    uint64_t listMoveNs = 0;
};

// recent-full is a lifecycle queue, not a liveness root. Account at ownership
// transitions so retained inventory can be separated from ordinary heap growth.
namespace RecentFullAccounting {
void Enqueue(size_t regions, size_t units);
void Dequeue(size_t regions, size_t units);
void Report(size_t listRegions, size_t listBytes);
}

struct FreePinnedSlotLists {
    static constexpr size_t ATOMIC_OBJECT_SIZE = 16;
    static constexpr size_t SYNC_OBJECT_SIZE = CJFuture::SYNC_OBJECT_SIZE;
    SlotList freeAtomicSlotList;
    SlotList freeSyncSlotList;

private:
    friend class RegionManager;
    uintptr_t PopFront(size_t size)
    {
        switch (size) {
            case ATOMIC_OBJECT_SIZE:
                return freeAtomicSlotList.PopFront(size);
            case SYNC_OBJECT_SIZE:
                return freeSyncSlotList.PopFront(size);
            default:
                return 0;
        }
    }

public:
    void PushFront(BaseObject* slot)
    {
        // getsizetrace: GetSize before the SlotList gate. CollectFreePinnedSlots
        // already gated, but this public entry must not SEGV on a coloured /
        // dead-region slot (same #GP family as SlotList::PopFront).
        if (!PlausibleManagedObjectGate("FreePinnedSlotLists::PushFront", slot)) {
            return;
        }
        size_t size = slot->GetSize();
        switch (size) {
            case ATOMIC_OBJECT_SIZE:
                freeAtomicSlotList.PushFront(slot);
                break;
            case SYNC_OBJECT_SIZE:
                freeSyncSlotList.PushFront(slot);
                break;
            default:
                return;
        }
    }

    void Clear()
    {
        freeAtomicSlotList.Clear();
        freeSyncSlotList.Clear();
    }
};

// RegionManager needs to know header size and alignment in order to iterate objects linearly
// and thus its Alloc should be rewrite with AllocObj(objSize)
class RegionManager {
    friend class VerifyRegions;
    friend struct PinRootTestAccess;
    friend struct IkeKeepTestAccess;
    friend struct IsFromRegTestAccess;

public:
    /* region memory layout:
        1. region info for each region, part of heap metadata
        2. region space for allocation, i.e., the heap
    */
    __attribute__((visibility("hidden"))) static size_t GetHeapMemorySize(size_t heapSize);

    __attribute__((visibility("hidden"))) static size_t GetHeapUnitCount(size_t heapSize);

    // get metadataSize by regionNum or unitNumber
    // RegionInfo and UnitInfo have the same sizeof
    __attribute__((visibility("hidden"))) static size_t GetMetadataSize(size_t num);
#if defined(__EULER__)
    void SetCacheRatio(double minSize, double maxSize, double defaultParam);
#endif
    void Initialize(size_t regionNum, uintptr_t regionInfoStart, MemMap& memoryOwner,
                    const HeapParam& heapParam, double garbageThreshold);
    void InitializeSegments(uintptr_t metadataStart, const std::vector<MemoryRange>& reservations,
                            MemMap& memoryOwner, const HeapParam& heapParam, double garbageThreshold);

    void VisitPageOwners(const std::function<void(RegionInfo*)>& visitor) const
    {
        RegionInfo::VisitPageOwners(visitor);
    }

    size_t GetCommittedCapacity() const { return RegionInfo::GetCommittedCapacity(); }

    size_t GetHeapCapacity() const { return heapUnitCount * RegionInfo::UNIT_SIZE; }


    RegionManager();

    RegionManager(const RegionManager&) = delete;

    RegionManager& operator=(const RegionManager&) = delete;

    // allowSaferegion=false: no ScopedEnterSaferegion under ROUTING (routefix / REPORT-routespin).
    RegionInfo* AllocateThreadLocalRegion(size_t size, bool expectPhysicalMem = false, bool youngRegion = true,
                                          bool allowSaferegion = true);

    // ZObjectAllocator::alloc / alloc_for_relocation. These pages never belong
    // to an AllocBuffer: a thread's TLAB and a CPU's shared page are distinct.
    uintptr_t AllocSharedObject(size_t size, PageAge age, bool nonBlocking = false);
    void RetireSharedPages(PageAgeRange ages);

    // ZHeap::account_alloc_page/account_undo_alloc_page: backing extents,
    // independent of the thread-local requested bytes and retirement waste.
    void UndoThreadLocalRegionAllocation(RegionInfo* region);
    // Stable cycle history: read under the statistics lock, at a safepoint,
    // or with managed access preventing the next young pause.
    size_t GetTLABUsed() const { return lastTLABUsed; }
    size_t GetTLABCapacity() const { return static_cast<size_t>(tlabCapacity); }
    void InitializeTLAB(AllocBuffer& buffer);
    void ResetTLABUsage();
    void PublishTLABStatistics();
    void RetireTLABStatistics(AllocBuffer& buffer);

    template<Generation G>
    void ForwardFromRegions(GCWorkers& workers);
    template<Generation G>
    void ForwardFromRegions();
    template<Generation G>
    void ForwardRegion(RegionInfo* region);
    RelocationRequestQueue& GetRelocationRequestQueue() { return relocationRequestQueue; }
    bool StallAllocation(AllocationStallRequest& request, bool requestGc);
    bool ClaimAllocationLocked(AllocationStallRequest& request);
    void ReturnPageMemory(const PageMemory& memory);
    void SatisfyStalledAllocations();
    bool IsAllocationStalling() const { return allocationStallQueue.IsStalling(); }
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    using AllocationStallTestHook = std::function<void(RegionManager&)>;
    MRT_EXPORT void SetAllocationStallTestHooks(AllocationStallTestHook beforeWave,
                                                AllocationStallTestHook requestGc,
                                                AllocationStallTestHook beforeWait);
    MRT_EXPORT size_t PendingStalledAllocations() const;
    MRT_EXPORT size_t EnqueuedStalledAllocations() const;
    MRT_EXPORT size_t DequeuedStalledAllocations() const;
    MRT_EXPORT size_t SatisfiedStalledAllocations() const;
    MRT_EXPORT size_t FailedStalledAllocations() const;
#endif
    template<Generation G>
    void ForwardClaimedPage(RegionInfo* region, ForwardingTable::Owner owner, bool claimed = false);
    template<Generation G>
    void StartForwardFromRegions(GCWorkers& workers);
    template<Generation G>
    void DrainForwardFromRegions();
    bool RelocationStarted() const { return relocationStarted; }
    // ZRelocateWork::update_remset_promoted, called by the relocating page worker.
    static void RememberPromotedObject(BaseObject* object);
    // ZRelocationSet::flip_promoted_pages: page pointers only; liveness belongs to the page.
    void AddFlipPromotedPage(RegionInfo* region);
    void RememberFlipPromotedPages(GCWorkers& workers);
    void StampCensusBoundaries();
    void PromoteAllRegions();
    // CompactRegion's list-ownership tail. A concurrent stay-young path may
    // already have moved the region to recent-full; never steal its links.
    void EnlistCompactedRegionForAllocator(RegionInfo* region);
    // Put a region the forward path finished with in place back where a collection-set builder
    // will find it; CompactRegion leaves it on tlRegionList, which no builder walks.
    void RehomeCompactedInPlaceRegion(RegionInfo* region);
    void CompactRegion(RegionInfo* region);
    void CompactRegion(RegionInfo* region, RegionInfo* toRegion1);

    void ExemptFromRegion(RegionInfo* region);
    // Rehome onto unmovableFrom without publishing kept. PrepareYoung parks
    // leftover from-pages here; they were expired at cycle start and must not
    // be re-published as this cycle's done (zRelocationSetSelector.cpp:114-196).
    void ParkUnmovableFromRegion(RegionInfo* region);
    // ZGC zRelocationSetSelector.cpp:114-196 / zGeneration.cpp:205-213: a page
    // not in this cycle's relocation set is an ordinary candidate next cycle.
    // Kept (IsForwardingDone via Exempt) is in-cycle only.
    // zRelocate.cpp:1041-1047: relocate() returns only after every page in the
    // relocation set is done. Finish every ROUTED page or publish it kept.
    void FinishIncompleteFromRegions();
    // zRelocate.cpp:1346-1352 flip_survived: keep the page, reset age, leave young.
    // Must not remain LONE_FROM / FROM after TakeHead — barriers treat those as from-space.
    void EnlistStayYoungSurvivor(RegionInfo* region, bool advanceAge = true);
    static void BumpYoungSurvivorAge(RegionInfo* region);
    static void FinishStayYoungInPlace(RegionInfo* region, bool advanceAge = true);

    // ZGeneration::select_relocation_set iterates only pages owned by that
    // generation (zGeneration.cpp:195-221).  An old relocation pass may
    // observe a young page in our shared list, but it must not relocate or
    // promote it using the old mark view.
    static constexpr bool GenerationMayRelocateYoung(Generation generation)
    {
        return generation == Generation::Young;
    }

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    void DumpRegionInfo() const;
#endif

    void DumpRegionStats(const char* msg) const;

    uintptr_t GetInactiveZone() const { return inactiveZone; }

#if defined(__EULER__)
    double GetCacheRatio() const { return cacheRatio; }
#endif

    uintptr_t GetRegionHeapStart() const { return regionHeapStart; }

    ~RegionManager() = default;

    // take a region with *num* units for allocation
    // allowSaferegion=false: best-effort, never enter saferegion (ROUTING critical section).
    RegionInfo* TakeRegion(size_t num, RegionInfo::UnitRole, bool expectPhysicalMem = false,
                           bool allowSaferegion = true, bool clearPayload = true);

    uintptr_t AllocPinnedFromFreeList(size_t size);

    uintptr_t AllocPinned(size_t size);

    // caller assures size is truely large (> region size)
    uintptr_t AllocLarge(size_t size, bool clearPayload = true);

    void EnlistFullThreadLocalRegion(RegionInfo* region) noexcept;

    void RemoveThreadLocalRegion(RegionInfo* region) noexcept;

    void RestoreToSpaceStateWords();

    void CountLiveObject(const BaseObject* obj);

    void AssembleSmallGarbageCandidates();
    void AssembleLargeGarbageCandidates();
    void AssemblePinnedGarbageCandidates(bool collectAll);
    YoungCollectionStats PrepareYoungGarbageCandidates(const std::function<void(RegionInfo*)>& visitor);

    void MergeRawPointerPinnedRegions()
    {
        oldPinnedRegionList.MergeRegionList(rawPointerPinnedRegionList, RegionInfo::RegionType::FULL_PINNED_REGION);
    }

    void CollectFromSpaceGarbage();

    size_t GetThreadLocalRegionSize() const
    {
        return maxUnitCountPerRegion * RegionInfo::UNIT_SIZE;
    }

    size_t GetYoungAllocatedSize() const;

    static bool IsKnownEmptyForView(RegionInfo* region, MarkView<Generation::Young> view)
    {
        return region->IsKnownYoungEmpty(view);
    }

    static bool IsKnownEmptyForView(RegionInfo* region, MarkView<Generation::Old> view)
    {
        return region->IsKnownEmpty(view);
    }

    template<Generation G>
    size_t CollectRegion(RegionInfo* region);

    void AddRawPointerObject(BaseObject* obj);

    void RemoveRawPointerObject(BaseObject* obj);

    void ReclaimRegion(RegionInfo* region);
    // Like ReclaimRegion but units enter mark-quarantine tree, not dirty tree.
    void ReclaimRegionToMarkQuarantine(RegionInfo* region);
    size_t ReleaseRegion(RegionInfo* region);
    // Clear the two exact bitmap slices owned by [regionStart, regionEnd).
    // Called on both CollectRegion and the direct large-region release path.
    static void ScrubRememberedSetForRegion(RegionInfo* region);
    // Emit + reset process-local scrub cost counters (STEER3).
    static void DumpScrubCostAndReset(const char* point);

    void ReclaimGarbageRegions();

    size_t CollectLargeGarbage();

    size_t CollectPinnedGarbage();
    size_t CollectFreePinnedSlots(RegionInfo* region);

    // Ignore dynamic pinned regions and from regions whose garbage objects are quite few, return the garbage size that
    // can be reclaimed.
    size_t ExemptFromRegions();
    // ZGC zGeneration.cpp:211-213: drop is_allocating pages at CSet select (pre-flip).
    // HasMarkStartAllocGap pages never enter the route plan. Stay on unmovableFrom;
    // next cycle ClearLiveInfo re-snapshots the watermark.
    size_t ExemptMarkStartAllocatingFromCSet();
    void ReassembleFromSpace();

    void ForEachObjUnsafe(const std::function<void(BaseObject*)>& visitor,
                          bool skipKnownEmptyRegions = false) const;
    void ForEachObjSafe(const std::function<void(BaseObject*)>& visitor) const;

    size_t GetUsedRegionSize() const { return GetUsedUnitCount() * RegionInfo::UNIT_SIZE; }

    size_t GetRecentAllocatedSize() const;

    size_t GetSurvivedSize() const;

    size_t GetUsedUnitCount() const;

    size_t GetDirtyUnitCount() const { return freeRegionManager.GetDirtyUnitCount(); }
    size_t GetGarbageUnitCount() const { return garbageRegionList.GetUnitCount(); }
    size_t GetInactiveUnitCount() const { return freeRegionManager.GetVirtualUnitCount(); }

    size_t GetActiveUnitCount() const { return heapUnitCount - GetInactiveUnitCount(); }

    inline size_t GetLargeObjectSize() const
    {
        return oldLargeRegionList.GetAllocatedSize() + recentLargeRegionList.GetAllocatedSize();
    }

    size_t GetAllocatedSize() const;

    inline size_t GetFromSpaceSize() const { return fromRegionList.GetAllocatedSize(); }

    inline size_t GetPinnedSpaceSize() const
    {
        return oldPinnedRegionList.GetAllocatedSize() + recentPinnedRegionList.GetAllocatedSize();
    }

    size_t GetLargeObjectThreshold() const { return largeObjectThreshold; }

    void ClearFreePinnedSlots() { freePinnedSlotLists.Clear(); }

    // wait for a period of time to allocate region which will avoid harm to gc
    void RequestForRegion(size_t size);

    void MergeRawPointerRegions(RegionList& smallSizeRegionList, RegionList& largeSizeRegionList);

    void SetMaxUnitCountForRegion(size_t regionSize);
    void SetMaxUnitCountForPinnedRegion(size_t regionSize);
    void SetLargeObjectThreshold(size_t regionSize);
    void SetGarbageThreshold(double garbageThreshold);

    void HandleTraceRegions();

    void PrepareTrace();

    // twoflags: walk live region lists and clear notRelocatableThisCycle.
    void ClearNotRelocatableThisCycleFlags();

    // routedest: walk the same lists and drop routeDestHold for one route generation.
    void ClearRouteDestHoldFlags();

    bool RelocateClaimedPage(RegionInfo* region);

    static bool RouteIsPublished(BaseObject* fromObj, RegionInfo* fromRegionInfo);

    PublishedRoute FindPublishedRoute(BaseObject* fromObj, RegionInfo* fromRegionInfo);

    PublishedRoute FindPublishedRoute(BaseObject* fromObj);

    bool RouteRegion(RegionInfo* fromRegionInfo, bool mayWait = true);

    template<Generation G>
    void PrepareFromRegionList();

    // Release point for OPTION_2 mark-epoch gate: major PostTrace after PrepareForwardTable.
    // Concurrent mark (TRACE+CLEAR_SATB) has finished; plain strong refs into quarantined
    // ranges are no longer traced. Safe to publish units to dirty tree for ClearUnits reuse.
    // Note: this major's just-installed quarantine (from PrepareForwardTable above) is also
    // released here — mark is already done, so no TRACE can race those units. Units held from
    // prior minor PrepareForwardTable are the ones that covered the TRACE window.
    void ReleaseMarkQuarantine();

    void ClearAllLiveInfo();

    // Probe-only: visit every region on managed lists with its list name (tag-reuse scan).
    template <typename F>
    void VisitAllManagedRegionsForProbe(F&& visitor);

private:
    // zPageAllocator.cpp:2248-2266: consumed by safe retirement after the
    // page table no longer publishes the old descriptor.
    void ReclaimRetiredRegion(RegionInfo* region);
    void ReclaimRetiredRegionToMarkQuarantine(RegionInfo* region);
    void ReleaseRetiredRegion(RegionInfo* region);
    void ReturnRetiredPageMemory(const PageMemory& memory, bool allowSaferegion = true);

    BaseObject* ComputeRoute(BaseObject* fromObj, RegionInfo* fromRegionInfo);

    BaseObject* ComputeRouteBorrowed(BaseObject* fromObj, RegionInfo* fromRegionInfo);

    RegionInfo* TakeReclaimableGarbageRegion(size_t* gatedBytes = nullptr);

    bool TryTakeGarbageRegionAfterDispel(RegionInfo* target);

    size_t GetGatedGarbageBytes();

    // Acquire a region list mutex which the collector also takes while the world is stopped.
    // Waiting for it in a saferegion is required so that a contended mutator cannot stall
    // StopTheWorld (MutatorManager.cpp:485-490), but the mutex must never be owned while the
    // saferegion guard is destroyed: LeaveSaferegion() parks the mutator in SuspendForSync()
    // (Mutator.h:172-186, Mutator.cpp:229-280) and the collector would then wait for that mutex
    // forever. Wait in try-lock rounds so every saferegion transition happens unlocked, exactly
    // as FreeRegionManager::TakeRegion() does for the free unit trees (FreeRegionManager.h:45-92).
    static void LockRegionListInSaferegion(std::mutex& listMutex);

    // caller must own recentPinnedRegionList's list mutex, and must not release it in between.
    uintptr_t AllocPinnedLocked(size_t size);

    static const size_t MAX_UNIT_COUNT_PER_REGION;
    static const size_t HUGE_PAGE;
    inline void CheckRegionWhetherCreatedInFixPhase(RegionInfo* region);
    inline void TagHugePage(RegionInfo* region, size_t num) const;
    inline void UntagHugePage(RegionInfo* region, size_t num) const;

    template<Generation G>
    void ClearLiveInfo(RegionList& list);

    // ZObjectAllocator::PerAge and ZPerCPU<ZPage*>. Contended slots have
    // independent cache lines; CPU migration selects a fresh index per call.
    struct SharedSmallPage;
    struct PerAgeObjectAllocator;
    static size_t SharedPageCPUCount();
    static size_t CurrentSharedPageCPU();
    RegionInfo* AllocateSharedPage(size_t units, RegionInfo::UnitRole role, PageAge age, bool nonBlocking);
    void UndoSharedPage(RegionInfo* page);
    std::unique_ptr<PerAgeObjectAllocator> objectAllocators[kPageAgeCount];

    FreeRegionManager freeRegionManager;

    // region lists actually represent life cycle of regions.
    // each region must belong to only one list at any time.

    // regions for movable (small-sized) objects.
    // regions for thread-local allocation.
    // regions in this list are already used for allocation but not full yet, i.e. local regions.
    RegionList tlRegionList;

    // recentFullRegionList is a list of regions which is already full, thus escape current gc.
    RegionList recentFullRegionList;

    // if region is allocated during gc trace phase, it is called a trace-region, it is recorded here when it is full.
    RegionCache fullTraceRegions;

    // fromRegionList is a list of full regions waiting to be collected (i.e. for forwarding).
    // region type must be FROM_REGION.
    RegionList fromRegionList;
    RelocationRequestQueue relocationRequestQueue;
    GCWorkers* relocationWorkers{ nullptr };
    bool relocationStarted{ false };
    bool relocationDrained{ false };
    // zPageAllocator.cpp:1518: ordinary allocation and stall share one owner.
    friend class Uncommitter;
    std::mutex flipPromotedMutex;
    std::vector<RegionInfo*> flipPromotedPages;
    std::mutex pageAllocatorMutex;
    AllocationStallQueue allocationStallQueue{ pageAllocatorMutex };
    size_t pageAllocatorUsed{ 0 };
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    AllocationStallTestHook allocationStallBeforeWaveTestHook;
    AllocationStallTestHook allocationStallGcTestHook;
    AllocationStallTestHook allocationStallBeforeWaitTestHook;
#endif
    RegionList ghostFromRegionList;

    // regions exempted by ExemptFromRegions, which will not be moved during current GC.
    RegionList unmovableFromRegionList;

    // cache for fromRegionList after forwarding.
    RegionList garbageRegionList;

    // regions for pinned (small-sized) objects.
    // region lists for small-sized pinned objects which are not be moved during concurrent gc, but
    // may be moved during compaction.
    RegionList recentPinnedRegionList;
    RegionList oldPinnedRegionList;

    // region lists for small-sized raw-pointer objects (i.e. future, monitor)
    // which can not be moved ever (even during compaction).
    RegionList rawPointerPinnedRegionList;

    // regions for large-sized objects.
    // large region is recorded here after large object is allocated.
    RegionList oldLargeRegionList;

    // if large region is allocated when gc is not running, it is recorded here.
    RegionList recentLargeRegionList;

    // if large region is allocated during gc trace phase, it is called a trace-region,
    // it is recorded here when it is full.
    RegionCache largeTraceRegions;

    uintptr_t regionInfoStart = 0; // the address of first RegionInfo

    uintptr_t regionHeapStart = 0; // the address of first region to allocate object
    uintptr_t regionHeapEnd = 0;

    // the time when previous region was allocated, which is assigned with returned value by timeutil::NanoSeconds().
    std::atomic<uint64_t> prevRegionAllocTime = { 0 };

    // heap space not allocated yet for even once. this value should not be decreased.
    std::atomic<uintptr_t> inactiveZone = { 0 }; // highest handed-out address, diagnostic envelope only
    size_t heapUnitCount = 0;
    std::atomic<size_t> tlabUsed{ 0 };
    size_t lastTLABUsed = 0;
    double tlabCapacity = 0;
    TLABAllocationAverage tlabAllocatingThreads;
    TLABAllocationAverage tlabRequestedFraction;
    std::mutex tlabStatisticsLock;
    TLABStatistics retiredTLABStatistics;

    size_t maxUnitCountPerRegion = MAX_UNIT_COUNT_PER_REGION;   // max units count for threadLocal buffer.
    size_t maxUnitCountPerPinnedRegion = maxUnitCountPerRegion; // max units count for pinned region.
    size_t largeObjectThreshold;
    double fromSpaceGarbageThreshold = 0.5; // 0.5: default garbage ratio.
    double exemptedRegionThreshold;
#if defined(__EULER__)
    double cacheRatio;
#endif
    std::mutex freePinnedSlotListMutex;
    FreePinnedSlotLists freePinnedSlotLists;
};

} // namespace MapleRuntime

#include "zObjectAllocator.hpp"
#include "zPageAllocator.inline.hpp"
#include "Heap/Collector/zRelocate.hpp"
#include "Heap/Collector/zRelocationSet.inline.hpp"
#endif // MRT_REGION_MANAGER_H
