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
    return page == nullptr ? 0 : page->Alloc(size);
}

inline uintptr_t RegionManager::AllocPinned(size_t size)
{
    std::mutex& regionListMutex = recentPinnedRegionList.GetListMutex();

    LockRegionListInSaferegion(regionListMutex);
    uintptr_t addr = AllocPinnedLocked(size);
    regionListMutex.unlock();
    if (addr != 0) {
        DLOG(ALLOC, "alloc pinned obj 0x%zx(%zu)", addr, size);
        return addr;
    }

    // TakeRegion() may enter a saferegion: never hold the region-list mutex
    // while a mutator leaves one and could park behind the collector.
    size_t needUnitCount = maxUnitCountPerRegion;
#if defined(__EULER__)
    needUnitCount = maxUnitCountPerPinnedRegion;
#endif
    ZPage* region = Heap::alloc_page(needUnitCount, ZPageType::small);
    if (region == nullptr) {
        return 0;
    }
    DLOG(REGION, "alloc pinned region @[0x%zx+%zu, 0x%zx) unit idx %zu type %u", region->GetRegionStart(),
         region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetUnitIdx(), 0u);

#if defined(MRT_TESTABLE_INTERNALS)
    if (testPinnedPageAcquired != nullptr) {
        testPinnedPageAcquired(region);
    }
#endif
    LockRegionListInSaferegion(regionListMutex);
    addr = AllocPinnedLocked(size);
    if (addr == 0) {
        // Acquisition may handshake; refresh this empty page under the same
        // mutex that spans old retirement and sequence advancement.
        region->ResetPageSequence();
        recentPinnedRegionList.PrependRegionLocked(region);
        Heap::GetHeap().object_allocator().allocator(PageAge::old)->pinnedPage.store(
            region, std::memory_order_release);
        addr = region->Alloc(size);
        region = nullptr;
    }
    regionListMutex.unlock();
    if (region != nullptr) {
        (void)CollectRegion<Generation::Old>(region);
    }

    DLOG(ALLOC, "alloc pinned obj 0x%zx(%zu)", addr, size);
    return addr;
}

inline uintptr_t RegionManager::AllocLarge(size_t size, bool clearPayload)
{
    size_t regionCount = (size + ZPage::UNIT_SIZE - 1) / ZPage::UNIT_SIZE;
    ZPage* region = Heap::alloc_page(regionCount, ZPageType::large,
                                    false, true, clearPayload, PageAge::eden);
    if (region == nullptr) {
        return 0;
    }
    DLOG(REGION, "alloc large region @[0x%zx+%zu, 0x%zx) unit idx %zu type %u", region->GetRegionStart(),
         region->GetRegionSize(), region->GetRegionEnd(), region->GetUnitIdx(), 0u);
    uintptr_t addr = region->Alloc(size);

    if (largeTraceRegions.TryPrependRegion(region)) {
    } else {
        recentLargeRegionList.PrependRegion(region);
    }

    return addr;
}

inline void RegionManager::EnlistFullThreadLocalRegion(ZPage* region) noexcept
{
    MRT_ASSERT(region->IsThreadLocalRegion(), "unexpected region type");
    if (region->IsTraceRegion()) {
        if (!fullTraceRegions.TryPrependRegion(region)) {
            recentFullRegionList.PrependRegion(region);
            RecentFullAccounting::Enqueue(1, region->GetUnitCount());
            (void)region;
        }
        return;
    }
    recentFullRegionList.PrependRegion(region);
    RecentFullAccounting::Enqueue(1, region->GetUnitCount());
}

inline void RegionManager::RemoveThreadLocalRegion(ZPage* region) noexcept
{
    MRT_ASSERT(region->IsThreadLocalRegion(), "unexpected region type");
    tlRegionList.DeleteRegion(region);
}
} // namespace MapleRuntime
#endif
