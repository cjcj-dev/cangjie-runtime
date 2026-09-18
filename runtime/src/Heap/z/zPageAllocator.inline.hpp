// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGIONMANAGER_INLINE_H
#define MRT_REGIONMANAGER_INLINE_H

#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zGeneration.hpp"

namespace MapleRuntime {

inline __attribute__((visibility("hidden"))) size_t RegionManager::GetHeapMemorySize(size_t heapSize)
    {
        size_t unitNum = GetHeapUnitCount(heapSize);
        size_t metadataSize = GetMetadataSize(unitNum);
        size_t roundedHeapSize = 0;
        CHECK_DETAIL(CheckedRoundUpSize(heapSize, ZPage::UNIT_SIZE, roundedHeapSize),
                     "heap size round-up overflows: heapSize=%zu unitSize=%zu", heapSize,
                     ZPage::UNIT_SIZE);
        size_t totalSize = 0;
        CHECK_DETAIL(CheckedAddSize(metadataSize, roundedHeapSize, totalSize),
                     "heap reservation geometry overflows: metadataSize=%zu heapSize=%zu",
                     metadataSize, roundedHeapSize);
        return totalSize;
    }

inline __attribute__((visibility("hidden"))) size_t RegionManager::GetHeapUnitCount(size_t heapSize)
    {
        size_t roundedHeapSize = 0;
        CHECK_DETAIL(CheckedRoundUpSize(heapSize, ZPage::UNIT_SIZE, roundedHeapSize),
                     "heap unit geometry overflows: heapSize=%zu unitSize=%zu", heapSize,
                     ZPage::UNIT_SIZE);
        heapSize = roundedHeapSize;
        size_t unitNum = heapSize / ZPage::UNIT_SIZE;
        return unitNum;
    }

inline __attribute__((visibility("hidden"))) size_t RegionManager::GetMetadataSize(size_t num)
    {
        (void)num;
        return MapleRuntime::MRT_PAGE_SIZE;
    }

    template<Generation G>
inline size_t RegionManager::CollectRegion(ZPage* region)
    {
        DLOG(REGION, "collect region %p@[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
             region->is_marked() ? region->live_bytes() : 0, region->GetRegionEnd(), 0u);
        region->LockWriteRegion();
        garbageRegionList.PrependRegion(region);
        region->UnlockWriteRegion();

        if (region->IsLargeRegion()) {
            return region->GetRegionSize();
        } else {
            return region->GetRegionSize() - (region->is_marked() ? region->live_bytes() : 0);
        }
    }





inline void RegionManager::ReclaimGarbageRegions()
    {
        ZPage* garbage = TakeReclaimableGarbageRegion();
        while (garbage != nullptr) {
            ReclaimRegion(garbage);
            garbage = TakeReclaimableGarbageRegion();
        }
        // STEER3: scrub runs here (async reclaim), not inside young STW.
        SatisfyStalledAllocations();
    }

inline size_t RegionManager::GetRecentAllocatedSize() const
    {
        return recentFullRegionList.GetAllocatedSize() + recentLargeRegionList.GetAllocatedSize() +
            recentPinnedRegionList.GetAllocatedSize();
    }

inline size_t RegionManager::GetSurvivedSize() const
    {
        return fromRegionList.GetAllocatedSize() + oldPinnedRegionList.GetAllocatedSize() +
            oldLargeRegionList.GetAllocatedSize();
    }

inline size_t RegionManager::GetUsedUnitCount() const
    {
        return
            fromRegionList.GetUnitCount() + unmovableFromRegionList.GetUnitCount() +
            recentFullRegionList.GetUnitCount() + oldLargeRegionList.GetUnitCount() +
            recentLargeRegionList.GetUnitCount() + oldPinnedRegionList.GetUnitCount() +
            recentPinnedRegionList.GetUnitCount() + rawPointerPinnedRegionList.GetUnitCount() +
            largeTraceRegions.GetUnitCount() + fullTraceRegions.GetUnitCount() +
            tlRegionList.GetUnitCount();
    }



inline void RegionManager::MergeRawPointerRegions(RegionList& smallSizeRegionList, RegionList& largeSizeRegionList)
    {
        const size_t smallRegions = smallSizeRegionList.GetRegionCount();
        const size_t smallUnits = smallSizeRegionList.GetUnitCount();
        recentFullRegionList.MergeRegionList(smallSizeRegionList);
        RecentFullAccounting::Enqueue(smallRegions, smallUnits);
        recentLargeRegionList.MergeRegionList(largeSizeRegionList);
    }

inline void RegionManager::HandleTraceRegions()
    {
        fullTraceRegions.DeactivateRegionCache();
        const size_t traceRegions = fullTraceRegions.GetRegionCount();
        const size_t traceUnits = fullTraceRegions.GetUnitCount();
        recentFullRegionList.MergeRegionList(fullTraceRegions);
        RecentFullAccounting::Enqueue(traceRegions, traceUnits);

        largeTraceRegions.DeactivateRegionCache();
        recentLargeRegionList.MergeRegionList(largeTraceRegions);

        tlRegionList.ClearTraceRegionFlag();
        recentPinnedRegionList.ClearTraceRegionFlag();
        oldPinnedRegionList.ClearTraceRegionFlag();
    }

inline void RegionManager::PrepareTrace()
    {
        fullTraceRegions.ActivateRegionCache();
        largeTraceRegions.ActivateRegionCache();
        // twoflags: Assemble just filtered previous-cycle stamps; clear so this TRACE
        // re-stamps only regions that allocate after this mark start.
        ClearNotRelocatableThisCycleFlags();
    }

inline void RegionManager::ReleaseMarkQuarantine()
    {
        size_t heldBefore = freeRegionManager.GetMarkQuarantineUnitCount();
        size_t units = freeRegionManager.ReleaseMarkQuarantineToDirty();
        size_t bytes = units * ZPage::UNIT_SIZE;
        VLOG(REPORT,
             "[MarkQuarantine] released_units=%zu released_bytes=%zu held_before=%zu held_after=%u",
             units, bytes, heldBefore, freeRegionManager.GetMarkQuarantineUnitCount());
        // Cost metric same family as ghostorder: peak retained bytes under mark-epoch gate.
        VLOG(REPORT, "[GhostRetention] retained_regions=%zu retained_bytes=%zu", heldBefore,
             heldBefore * ZPage::UNIT_SIZE);
        SatisfyStalledAllocations();
    }


    template <typename F>
inline void RegionManager::VisitAllManagedRegionsForProbe(F&& visitor)
    {
        auto walk = [&visitor](const char* name, RegionList& list) {
            list.VisitAllRegions([&visitor, name](ZPage* region) { visitor(region, name); });
        };
        walk("tlRegionList", tlRegionList);
        walk("recentFullRegionList", recentFullRegionList);
        walk("fromRegionList", fromRegionList);
        ghostFromRegionList.VisitAllGhostRegions(
            [&visitor](ZPage* region) { visitor(region, "ghostFromRegionList"); });
        walk("unmovableFromRegionList", unmovableFromRegionList);
        walk("garbageRegionList", garbageRegionList);
        walk("recentPinnedRegionList", recentPinnedRegionList);
        walk("oldPinnedRegionList", oldPinnedRegionList);
        walk("rawPointerPinnedRegionList", rawPointerPinnedRegionList);
        walk("oldLargeRegionList", oldLargeRegionList);
        walk("recentLargeRegionList", recentLargeRegionList);
        walk("fullTraceRegions", fullTraceRegions);
        walk("largeTraceRegions", largeTraceRegions);
    }

inline ZPage* RegionManager::TakeReclaimableGarbageRegion(size_t* gatedBytes)
    {
        std::lock_guard<std::mutex> lock(garbageRegionList.GetListMutex());
        ZPage* candidate = nullptr;
        size_t bytes = 0;
        for (ZPage* region = garbageRegionList.GetHeadRegion(); region != nullptr;
             region = region->GetNextRegion()) {
            if (candidate == nullptr && region->GetRawPointerObjectCount() == 0) {
                // routedest: defence in depth. A held region should never have reached
                // garbageRegionList — the two Assemble gates and the two young gates refuse
                // it first — so a non-zero count at this site means one of those was
                // bypassed. This one chokepoint covers both reclaim schedules that are not
                // phase-driven at once: the mutator garbage fast path (TakeRegion) and the
                // finalizer, whose ReclaimGarbageRegions loops on this function.
                candidate = region;
            }
        }
        if (candidate != nullptr) {
            RemoveRegionLocked(&garbageRegionList, candidate);
        }
        if (gatedBytes != nullptr) {
            *gatedBytes = bytes;
        }
        return candidate;
    }

inline bool RegionManager::TryTakeGarbageRegionAfterDispel(ZPage* target)
    {
        std::lock_guard<std::mutex> lock(garbageRegionList.GetListMutex());
        for (ZPage* region = garbageRegionList.GetHeadRegion(); region != nullptr;
             region = region->GetNextRegion()) {
            if (region == target) {
                CHECK_DETAIL(region->IsGarbageRegion(),
                             "TryTakeGarbageRegionAfterDispel region=%p type=%u "
                             "(garbage list still names a non-GARBAGE region)",
                             region, static_cast<unsigned>(0u));
                // routedest: refuse a held region here too, so it is neither quarantined nor
                // reclaimed. Same defence-in-depth role as TakeReclaimableGarbageRegion.
                if (region->GetRawPointerObjectCount() > 0) {
                    return false;
                }
                RemoveRegionLocked(&garbageRegionList, region);
                return true;
            }
        }
        return false;
    }

inline size_t RegionManager::GetGatedGarbageBytes()
    {
        std::lock_guard<std::mutex> lock(garbageRegionList.GetListMutex());
        (void)garbageRegionList;
        return 0;
    }




} // namespace MapleRuntime
#endif

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RELOCATE_H
#define MRT_RELOCATE_H

#include "Heap/z/zPageAllocator.hpp"

namespace MapleRuntime {
namespace detail {

// A single algorithm body serves both compile-time shapes below.  The default
// product inlines it through ForwardTask::Execute; the testable shape calls it
// from the exported out-of-line Execute instantiated in RegionManager.cpp.
template<Generation G>
inline void ExecuteForwardTask(RegionManager& regionManager, RegionList& fromRegionList)
{
    while (true) {
        // zRelocate.cpp:1193-1203: serve a mutator's requested receipt
        // before advancing the ordinary relocation iterator.
        ZRelocateQueue::Selection selected =
            regionManager.GetZRelocateQueue().SelectBeforeOrdinary([&fromRegionList]() -> void* {
                return fromRegionList.TakeHeadRegion();
            });
        if (!selected) {
            selected = regionManager.GetZRelocateQueue().SynchronizePoll();
            if (selected.workersDone) {
                break;
            }
            if (!selected) {
                continue;
            }
        }
        if (!selected.is_request()) {
            ZPage* region = static_cast<ZPage*>(selected.ordinary);
            regionManager.ForwardClaimedPage<G>(region, forwarding_for_page(region));
            continue;
        }

        ZPage* region = static_cast<ZPage*>(selected.owner());
        // If an ordinary iterator already removed the page, its worker will
        // lose the forwarding claim. This claimant still owns the page task.
        (void)fromRegionList.TryDeleteRegion(region);
        regionManager.ForwardClaimedPage<G>(region,
            forwarding_for_page(region), true);
    }
}

} // namespace detail

// The relocation worker task submitted by DrainForwardFromRegions. Test builds
// export Work so the unit runner binds the product SO; default builds retain
// the implicit inline virtual with no MRT_EXPORT and no dynamic export.
template<Generation G>
class ForwardTask : public ZTask {
public:
    ForwardTask(RegionManager& manager, RegionList& fromSpace)
        : ZTask("ZRelocateTask"), regionManager(manager), fromRegionList(fromSpace) {}

    ~ForwardTask() override = default;
#if defined(MRT_TESTABLE_INTERNALS)
    MRT_EXPORT void work() override;
#else
    __attribute__((visibility("hidden"))) void work() override
    {
        detail::ExecuteForwardTask<G>(regionManager, fromRegionList);
    }
#endif

private:
    RegionManager& regionManager;
    RegionList& fromRegionList;
};












} // namespace MapleRuntime
#endif
