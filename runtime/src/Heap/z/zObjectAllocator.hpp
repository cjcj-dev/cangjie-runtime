// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_OBJECT_ALLOCATOR_H
#define MRT_OBJECT_ALLOCATOR_H

#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zDeferredConstructed.inline.hpp"
#include "Heap/z/zValue.inline.hpp"

namespace MapleRuntime {
// zObjectAllocator.cpp:48-54 (per-CPU shared small pages are always in use;
// ZHeuristics::use_per_cpu_shared_small_pages belongs to the allocator package).
inline ZPage** RegionManager::PerAgeObjectAllocator::shared_small_page_addr()
{
    return sharedSmallPage.addr();
}

inline ZPage* const* RegionManager::PerAgeObjectAllocator::shared_small_page_addr() const
{
    return sharedSmallPage.addr();
}

inline ZPage** RegionManager::PerAgeObjectAllocator::shared_medium_page_addr()
{
    return sharedMediumPage.addr();
}

inline ZPage* const* RegionManager::PerAgeObjectAllocator::shared_medium_page_addr() const
{
    return sharedMediumPage.addr();
}

// zObjectAllocator.cpp:219-221
inline RegionManager::PerAgeObjectAllocator* RegionManager::allocator(PageAge age)
{
    return objectAllocators[untype(age)].get();
}



inline uintptr_t RegionManager::AllocPinnedLocked(size_t size)
{
    ZPage* page = allocator(PageAge::old)->pinnedPage.load(std::memory_order_acquire);
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

        // TakeRegion() must not run while this mutator owns the pinned region list mutex: it
        // enters a saferegion between try-lock rounds (FreeRegionManager.h:91) and again in
        // ReclaimRegion() -> FreeRegionManager::AddGarbageUnits() (RegionManager.cpp:420,
        // FreeRegionManager.h:100). Being in a saferegion lets StopTheWorld complete while the
        // mutex is held, and the minor collection then blocks on that very mutex inside the
        // stopped world (RegionManager.cpp:500 -> RegionList.h:117).
        size_t needUnitCount = maxUnitCountPerRegion;
#if defined(__EULER__)
        needUnitCount = maxUnitCountPerPinnedRegion;
#endif
        ZPage* region = Heap::alloc_page(needUnitCount, ZPageType::small, false, true, true, PageAge::old);
        if (region == nullptr) {
            return 0;
        }
        DLOG(REGION, "alloc pinned region @[0x%zx+%zu, 0x%zx) unit idx %zu type %u", region->GetRegionStart(),
             region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetUnitIdx(),
             0u);

#if defined(MRT_TESTABLE_INTERNALS)
        if (testPinnedPageAcquired != nullptr) {
            testPinnedPageAcquired(region);
        }
#endif
        LockRegionListInSaferegion(regionListMutex);
        // another mutator may have installed a pinned region while the mutex was released.
        addr = AllocPinnedLocked(size);
        if (addr == 0) {
            // ZPage::reset_seqnum (zPage.cpp:90) precedes publication without
            // an intervening mark-start pause. Our acquisition may handshake;
            // refresh this still-empty page under the same mutex that spans
            // old retirement and seqnum advancement (P14 pause-model adapter).
            region->ResetPageSequence();
            // To make sure the allocedSize are consistent, it must prepend region first then alloc object.
            recentPinnedRegionList.PrependRegionLocked(region);
            allocator(PageAge::old)->pinnedPage.store(region, std::memory_order_release);
            addr = region->Alloc(size);
            region = nullptr;
        }
        regionListMutex.unlock();
        if (region != nullptr) {
            // the region was not needed after all, hand it back the same way
            // RegionSpace::FeedHungryBuffers() does (RegionSpace.cpp:302-306).

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
