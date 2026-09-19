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
        region->SetRegionRole(ZPageRole::Garbage);
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

inline size_t RegionManager::SumAllocatedByRoles(std::initializer_list<ZPageRole> roles) const
    {
        size_t bytes = 0;
        ZPage::SafeDestroyScope scope;
        ZPageTableIterator iter(&ZPageTable::heap_table());
        for (ZPage* region; iter.next(&region);) {
            for (ZPageRole role : roles) {
                if (region->GetRegionRole() == role) {
                    bytes += region->GetUnitCount() * ZPage::UNIT_SIZE;
                    break;
                }
            }
        }
        return bytes;
    }

inline size_t RegionManager::GetRecentAllocatedSize() const
    {
        return SumAllocatedByRoles({ ZPageRole::RecentFull, ZPageRole::RecentLarge, ZPageRole::RecentPinned });
    }

inline size_t RegionManager::GetSurvivedSize() const
    {
        return SumAllocatedByRoles({ ZPageRole::From, ZPageRole::OldPinned, ZPageRole::OldLarge });
    }

inline size_t RegionManager::GetFromSpaceSize() const
    {
        return SumAllocatedByRoles({ ZPageRole::From });
    }

inline size_t RegionManager::GetPinnedSpaceSize() const
    {
        return SumAllocatedByRoles({ ZPageRole::OldPinned, ZPageRole::RecentPinned, ZPageRole::RawPointerPinned });
    }

inline size_t RegionManager::GetUsedUnitCount() const
    {
        // zPageAllocator.cpp:1311: used is the allocator's page-granular
        // counter, not a list sum.
        return pageAllocatorUsed / ZPage::UNIT_SIZE;
    }



inline void RegionManager::MergeRawPointerRegions(std::vector<ZPage*>& smallSizeRegions,
                                                      std::vector<ZPage*>& largeSizeRegions)
    {
        size_t smallUnits = 0;
        for (ZPage* region : smallSizeRegions) {
            region->SetRegionRole(ZPageRole::RecentFull);
            smallUnits += region->GetUnitCount();
        }

        smallSizeRegions.clear();
        for (ZPage* region : largeSizeRegions) {
            region->SetRegionRole(ZPageRole::RecentLarge);
        }
        largeSizeRegions.clear();
    }

inline void RegionManager::HandleTraceRegions()
    {
        // #710: trace-stamped pages become ordinary full/large pages; the
        // stamp is a role word, so the merge is a page-table walk
        // (zPageTable.hpp:57-77), not a list splice.
        fullTraceCacheActive = false;
        largeTraceCacheActive = false;
        size_t traceRegions = 0;
        size_t traceUnits = 0;
        ZPage::SafeDestroyScope scope;
        ZPageTableIterator iter(&ZPageTable::heap_table());
        for (ZPage* region; iter.next(&region);) {
            const ZPageRole role = region->GetRegionRole();
            if (role == ZPageRole::FullTrace) {
                region->SetRegionRole(ZPageRole::RecentFull);
                ++traceRegions;
                traceUnits += region->GetUnitCount();
            } else if (role == ZPageRole::LargeTrace) {
                region->SetRegionRole(ZPageRole::RecentLarge);
            }
        }

    }

inline void RegionManager::PrepareTrace()
    {
        fullTraceCacheActive = true;
        largeTraceCacheActive = true;
        // twoflags: notRelocatableThisCycle stamps do not exist as list state;
        // is_allocating (zPage.inline.hpp:180-186) is the only filter.
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
        // #710: managed pages are the page table's non-free pages; the probe
        // names the role word instead of the deleted list.
        ZPage::SafeDestroyScope scope;
        ZPageTableIterator iter(&ZPageTable::heap_table());
        for (ZPage* region; iter.next(&region);) {
            visitor(region, RegionRoleName(region->GetRegionRole()));
        }
    }

inline ZPage* RegionManager::TakeReclaimableGarbageRegion(size_t* gatedBytes)
    {
        // #710: garbage pages are page-table entries with the Garbage role.
        // The claim is a role CAS (zPageTable.hpp:57-77 walk); the routedest
        // defence-in-depth raw-pointer check is unchanged.
        ZPage* candidate = nullptr;
        ZPage::SafeDestroyScope scope;
        ZPageTableIterator iter(&ZPageTable::heap_table());
        for (ZPage* region; iter.next(&region);) {
            if (region->GetRegionRole() != ZPageRole::Garbage || region->GetRawPointerObjectCount() != 0) {
                continue;
            }
            ZPageRole expect = ZPageRole::Garbage;
            if (region->CASRegionRole(expect, ZPageRole::None)) {
                candidate = region;
                break;
            }
        }
        if (gatedBytes != nullptr) {
            *gatedBytes = 0;
        }
        return candidate;
    }

inline bool RegionManager::TryTakeGarbageRegionAfterDispel(ZPage* target)
    {
        CHECK_DETAIL(target == nullptr || target->IsGarbageRegion(),
                     "TryTakeGarbageRegionAfterDispel region=%p type=%u "
                     "(garbage role still names a non-GARBAGE region)",
                     target, static_cast<unsigned>(0u));
        if (target == nullptr || target->GetRawPointerObjectCount() > 0) {
            return false;
        }
        ZPageRole expect = ZPageRole::Garbage;
        return target->CASRegionRole(expect, ZPageRole::None);
    }

inline size_t RegionManager::GetGatedGarbageBytes()
    {
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
// #710: ordinary work comes from the relocation set's parallel iterator
// (zRelocate.cpp:1088-1153 ZRelocate::relocate shape), not a page list.
template<Generation G>
inline void ExecuteForwardTask(RegionManager& regionManager, ZRelocationSet* relocationSet)
{
    ZRelocationSetParallelIterator iter(relocationSet);
    while (true) {
        // zRelocate.cpp:1193-1203: serve a mutator's requested receipt
        // before advancing the ordinary relocation iterator.
        ZRelocateQueue::Selection selected =
            regionManager.GetZRelocateQueue().SelectBeforeOrdinary([&iter]() -> void* {
                ZForwarding* forwarding = nullptr;
                return iter.next(&forwarding) ? static_cast<void*>(forwarding) : nullptr;
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
            ZForwarding* forwarding = static_cast<ZForwarding*>(selected.ordinary);
            regionManager.ForwardClaimedPage<G>(forwarding->page(), forwarding);
            continue;
        }

        ZPage* region = static_cast<ZPage*>(selected.owner());
        // The request claimant owns the page task even when an ordinary
        // iterator already claimed the forwarding (the claim fails there).
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
    ForwardTask(RegionManager& manager, ZRelocationSet* relocationSet)
        : ZTask("ZRelocateTask"), regionManager(manager), relocationSet(relocationSet) {}

    ~ForwardTask() override = default;
    __attribute__((visibility("hidden"))) void work() override
    {
        detail::ExecuteForwardTask<G>(regionManager, relocationSet);
    }

private:
    RegionManager& regionManager;
    ZRelocationSet* relocationSet;
};












} // namespace MapleRuntime
#endif
