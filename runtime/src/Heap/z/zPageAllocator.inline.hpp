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
        size_t alignedHeapSize = GetAlignedHeapSize(heapSize);
        size_t metadataSize = GetMetadataSize();
        size_t roundedHeapSize = 0;
        CHECK_DETAIL(CheckedRoundUpSize(heapSize, ZGranuleSize, roundedHeapSize),
                     "heap size round-up overflows: heapSize=%zu unitSize=%zu", heapSize,
                     ZGranuleSize);
        size_t totalSize = 0;
        CHECK_DETAIL(CheckedAddSize(metadataSize, roundedHeapSize, totalSize),
                     "heap reservation geometry overflows: metadataSize=%zu heapSize=%zu",
                     metadataSize, roundedHeapSize);
        return totalSize;
    }

inline __attribute__((visibility("hidden"))) size_t RegionManager::GetAlignedHeapSize(size_t heapSize)
    {
        size_t roundedHeapSize = 0;
        CHECK_DETAIL(CheckedRoundUpSize(heapSize, ZGranuleSize, roundedHeapSize),
                     "heap unit geometry overflows: heapSize=%zu unitSize=%zu", heapSize,
                     ZGranuleSize);
        heapSize = roundedHeapSize;
        size_t alignedHeapSize = heapSize;
        return alignedHeapSize;
    }

inline __attribute__((visibility("hidden"))) size_t RegionManager::GetMetadataSize()
    {
        return ZGranuleSize;
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
                    bytes += region->GetRegionSize();
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

inline size_t RegionManager::GetUsedBytes() const
    {
        // zPageAllocator.cpp:1311: used is the allocator's page-granular
        // counter, not a list sum.
        return pageAllocatorUsed;
    }



inline void RegionManager::MergeRawPointerRegions(std::vector<ZPage*>& smallSizeRegions,
                                                      std::vector<ZPage*>& largeSizeRegions)
    {
        size_t smallBytes = 0;
        for (ZPage* region : smallSizeRegions) {
            region->SetRegionRole(ZPageRole::RecentFull);
            smallBytes += region->GetRegionSize();
        }
        RecentFullAccounting::Enqueue(smallSizeRegions.size(), smallBytes);
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
        size_t traceBytes = 0;
        ZPage::SafeDestroyScope scope;
        ZPageTableIterator iter(&ZPageTable::heap_table());
        for (ZPage* region; iter.next(&region);) {
            const ZPageRole role = region->GetRegionRole();
            if (role == ZPageRole::FullTrace) {
                region->SetRegionRole(ZPageRole::RecentFull);
                ++traceRegions;
                traceBytes += region->GetRegionSize();
            } else if (role == ZPageRole::LargeTrace) {
                region->SetRegionRole(ZPageRole::RecentLarge);
            }
        }
        RecentFullAccounting::Enqueue(traceRegions, traceBytes);
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
        size_t heldBefore = freeRegionManager.GetMarkQuarantineBytes();
        size_t bytes = freeRegionManager.ReleaseMarkQuarantineToDirty();
        VLOG(REPORT,
             "[MarkQuarantine] released_bytes=%zu held_before=%zu held_after=%zu",
             bytes, heldBefore, freeRegionManager.GetMarkQuarantineBytes());
        // Cost metric same family as ghostorder: peak retained bytes under mark-epoch gate.
        VLOG(REPORT, "[GhostRetention] retained_regions=%zu retained_bytes=%zu", heldBefore,
             heldBefore);
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
inline void ExecuteForwardTask(RegionManager& regionManager, ZRelocationSet* relocationSet,
                               ZRelocationSetParallelIterator& iter)
{
    ZRelocateQueue& queue = *relocationSet->generation()->relocate().queue();
    for (;;) {
        // ZGC zRelocate.cpp:1193-1211: service waiting mutators before
        // synchronizing for a safepoint, then take ordinary page work.
        for (ZForwarding* forwarding; (forwarding = queue.synchronize_poll()) != nullptr;) {
            regionManager.ForwardClaimedPage<G>(forwarding->page(), forwarding, true);
        }
        ZForwarding* forwarding = nullptr;
        if (!iter.next(&forwarding)) {
            break;
        }
        regionManager.ForwardClaimedPage<G>(forwarding->page(), forwarding);
    }
    queue.leave();
}

} // namespace detail

// The relocation worker task submitted by ZRelocate::relocate. Test builds
// export Work so the unit runner binds the product SO; default builds retain
// the implicit inline virtual with no MRT_EXPORT and no dynamic export.
template<Generation G>
class ForwardTask : public ZTask {
public:
    ForwardTask(RegionManager& manager, ZRelocationSet* relocationSet)
        : ZTask("ZRelocateTask"), regionManager(manager), relocationSet(relocationSet), iter(relocationSet) {}

    // ZGC zRelocate.cpp:1124: deactivate after all workers have left.
    ~ForwardTask() override { relocationSet->generation()->relocate().queue()->deactivate(); }
#if defined(MRT_TESTABLE_INTERNALS)
    MRT_EXPORT void work() override;
#else
    __attribute__((visibility("hidden"))) void work() override
    {
        detail::ExecuteForwardTask<G>(regionManager, relocationSet, iter);
    }
#endif

private:
    RegionManager& regionManager;
    ZRelocationSet* relocationSet;
    ZRelocationSetParallelIterator iter;
};












} // namespace MapleRuntime
#endif
