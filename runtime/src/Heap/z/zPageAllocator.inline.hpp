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
        return SumAllocatedByRoles({ ZPageRole::RecentFull, ZPageRole::RecentLarge });
    }

inline size_t RegionManager::GetSurvivedSize() const
    {
        return SumAllocatedByRoles({ ZPageRole::From, ZPageRole::OldLarge });
    }

inline size_t RegionManager::GetFromSpaceSize() const
    {
        return SumAllocatedByRoles({ ZPageRole::From });
    }

inline size_t RegionManager::GetUsedBytes() const
    {
        // zPageAllocator.cpp:1311: used is the allocator's page-granular
        // counter, not a list sum.
        return pageAllocatorUsed;
    }



inline void RegionManager::HandleTraceRegions()
    {
        // #710: trace-stamped pages become ordinary full/large pages; the
        // stamp is a role word, so the merge is a page-table walk
        // (zPageTable.hpp:57-77), not a list splice.
        fullTraceCacheActive = false;
        largeTraceCacheActive = false;
        ZPage::SafeDestroyScope scope;
        ZPageTableIterator iter(&ZPageTable::heap_table());
        for (ZPage* region; iter.next(&region);) {
            const ZPageRole role = region->GetRegionRole();
            if (role == ZPageRole::FullTrace) {
                region->SetRegionRole(ZPageRole::RecentFull);
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


} // namespace detail

// The relocation worker task submitted by ZRelocate::relocate. Test builds
// export Work so the unit runner binds the product SO; default builds retain
// the implicit inline virtual with no MRT_EXPORT and no dynamic export.
template<Generation G>
class ForwardTask : public ZTask {
public:
    ForwardTask(RegionManager& manager, ZRelocationSet* relocationSet)
        : ZTask("ZRelocateTask"), regionManager(manager), relocationSet(relocationSet), iter(relocationSet),
          smallAllocator(relocationSet->generation()),
          mediumAllocator(relocationSet->generation(),
                          relocationSet->generation()->relocate().shared_medium_targets()) {}
    ~ForwardTask() override { relocationSet->generation()->relocate().queue()->deactivate(); }
#if defined(MRT_TESTABLE_INTERNALS)
    MRT_EXPORT void work() override;
#else
    __attribute__((visibility("hidden"))) void work() override;
#endif
private:
    RegionManager& regionManager;
    ZRelocationSet* relocationSet;
    ZRelocationSetParallelIterator iter;
    ZRelocateSmallAllocator smallAllocator;
    ZRelocateMediumAllocator mediumAllocator;

};












} // namespace MapleRuntime
#endif
