// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_OBJECT_ALLOCATOR_INLINE_H
#define MRT_OBJECT_ALLOCATOR_INLINE_H

#include "Heap/z/zObjectAllocator.hpp"

namespace MapleRuntime {
// zObjectAllocator.cpp:219-221
inline uintptr_t RegionManager::AllocPinnedLocked(size_t size)
{
    ZPage* page = Heap::GetHeap().object_allocator().allocator(PageAge::old)->pinnedPage.load(std::memory_order_acquire);
    return page == nullptr ? 0 : page->alloc_object(size);
}

inline uintptr_t RegionManager::AllocPinned(size_t size)
{
    std::mutex& regionListMutex = pinnedAllocationMutex;

    LockPageMutexInSaferegion(regionListMutex);
    uintptr_t addr = AllocPinnedLocked(size);
    regionListMutex.unlock();
    if (addr != 0) {
        DLOG(ALLOC, "alloc pinned obj 0x%zx(%zu)", addr, size);
        return addr;
    }

    // TakeRegion() may enter a saferegion: never hold the region-list mutex
    // while a mutator leaves one and could park behind the collector.
    ZPage* region = Heap::alloc_page(ZPageSizeSmall, ZPageType::small);
    if (region == nullptr) {
        return 0;
    }
    DLOG(REGION, "alloc pinned region @[0x%zx+%zu, 0x%zx) unit idx %zu type %u", region->GetRegionStart(),
         region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->granule_index(), 0u);

#if defined(MRT_TESTABLE_INTERNALS)
    if (testPinnedPageAcquired != nullptr) {
        testPinnedPageAcquired(region);
    }
#endif
    LockPageMutexInSaferegion(regionListMutex);
    addr = AllocPinnedLocked(size);
    if (addr == 0) {
        // Acquisition may handshake; refresh this empty page under the same
        // mutex that spans old retirement and sequence advancement.
        region->ResetPageSequence();
        // zObjectAllocator.hpp: the pinned allocation page is a per-allocator
        // pointer, not a list member; the role word carries the lifecycle.
        region->SetRegionRole(ZPageRole::RecentPinned);
        Heap::GetHeap().object_allocator().allocator(PageAge::old)->pinnedPage.store(
            region, std::memory_order_release);
        addr = region->alloc_object(size);
        region = nullptr;
    }
    regionListMutex.unlock();
    if (region != nullptr) {
        (void)CollectRegion<Generation::Old>(region);
    }

    DLOG(ALLOC, "alloc pinned obj 0x%zx(%zu)", addr, size);
    return addr;
}

} // namespace MapleRuntime
#endif
