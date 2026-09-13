// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_OBJECT_ALLOCATOR_H
#define MRT_OBJECT_ALLOCATOR_H

#include "Heap/z/zPageAllocator.hpp"

namespace MapleRuntime {
    struct RegionManager::SharedSmallPage {
        std::atomic<RegionInfo*> page{nullptr};
        char padding[64 - sizeof(std::atomic<RegionInfo*>)];
    };
    struct RegionManager::PerAgeObjectAllocator {
        explicit PerAgeObjectAllocator(PageAge pageAge);
        const PageAge age;
        std::unique_ptr<SharedSmallPage[]> smallPages;
    };



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
        RegionInfo* region = TakeRegion(needUnitCount, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        if (region == nullptr) {
            return 0;
        }
        DLOG(REGION, "alloc pinned region @[0x%zx+%zu, 0x%zx) unit idx %zu type %u", region->GetRegionStart(),
             region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetUnitIdx(),
             region->GetRegionType());

        LockRegionListInSaferegion(regionListMutex);
        // another mutator may have installed a pinned region while the mutex was released.
        addr = AllocPinnedLocked(size);
        if (addr == 0) {
            // If allocate pinned obj during tracing, set region to traced new region.
            GCPhase phase = Heap::GetHeap().GetGCPhase(region->IsYoungRegion() ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD);
            if (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER) {
                region->SetTraceRegionFlag(1);
            }
            // twoflags: POST_TRACE+ only (TRACE uses isTraceRegion).
            if (phase == GC_PHASE_POST_TRACE || phase == GC_PHASE_PREFORWARD ||
                phase == GC_PHASE_FORWARD) {
                region->SetNotRelocatableThisCycle(1);
            }
            // To make sure the allocedSize are consistent, it must prepend region first then alloc object.
            recentPinnedRegionList.PrependRegionLocked(region, RegionInfo::RegionType::RECENT_PINNED_REGION);
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
        size_t regionCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;
        RegionInfo* region = TakeRegion(regionCount, RegionInfo::UnitRole::LARGE_SIZED_UNITS,
                                        false, true, clearPayload);
        if (region == nullptr) {
            return 0;
        }
        DLOG(REGION, "alloc large region @[0x%zx+%zu, 0x%zx) unit idx %zu type %u", region->GetRegionStart(),
             region->GetRegionSize(), region->GetRegionEnd(), region->GetUnitIdx(), region->GetRegionType());
        uintptr_t addr = region->Alloc(size);

        GCPhase phase = Heap::GetHeap().GetGCPhase(region->IsYoungRegion() ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD);
        bool shouldSetTraceRegion = (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER);
        if (largeTraceRegions.TryPrependRegion(region, RegionInfo::RegionType::RECENT_LARGE_REGION)) {
            if (shouldSetTraceRegion) {
                region->SetTraceRegionFlag(1);
            }
        } else {
            recentLargeRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_LARGE_REGION);
            region->SetTraceRegionFlag(0);
        }
        // twoflags: POST_TRACE+ only (independent of isTraceRegion).
        if (phase == GC_PHASE_POST_TRACE || phase == GC_PHASE_PREFORWARD ||
            phase == GC_PHASE_FORWARD) {
            region->SetNotRelocatableThisCycle(1);
        }

        return addr;
    }

inline void RegionManager::EnlistFullThreadLocalRegion(RegionInfo* region) noexcept
    {
        MRT_ASSERT(region->IsThreadLocalRegion(), "unexpected region type");

        if (region->IsTraceRegion()) {
            if (!fullTraceRegions.TryPrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION)) {
                recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
                RecentFullAccounting::Enqueue(1, region->GetUnitCount());
                region->SetTraceRegionFlag(0);
            }
            return;
        }
        recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
        RecentFullAccounting::Enqueue(1, region->GetUnitCount());
    }

inline void RegionManager::RemoveThreadLocalRegion(RegionInfo* region) noexcept
    {
        MRT_ASSERT(region->IsThreadLocalRegion(), "unexpected region type");
        tlRegionList.DeleteRegion(region);
    }

} // namespace MapleRuntime
#endif
