// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REGION_SPACE_H
#define MRT_REGION_SPACE_H

#include <cassert>
#include <list>
#include <memory>
#include <sys/mman.h>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "Heap/z/zServiceability.hpp"
#include "Allocator.h"
#include "ExceptionManager.h"
#include "Mutator/Mutator.h"
#include "Heap/z/zPageAllocator.hpp"
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
// RegionSpace aims to be the API for other components of runtime
// the complication of implementation is delegated to RegionManager
// allocator should not depend on any assumptions on the details of RegionManager
class RegionSpace : public Allocator {
public:
    static size_t ToAllocSize(size_t objSize)
    {
        size_t size = objSize + HEADER_SIZE;
        return RoundUp<size_t>(size, ALLOC_ALIGN);
    }

    static size_t GetAllocSize(const BaseObject& obj)
    {
        size_t objSize = obj.GetSize();
        return ToAllocSize(objSize);
    }

    RegionSpace() = default;
    ATTR_NO_INLINE ~RegionSpace() override
    {
        if (allocBufferManager != nullptr) {
            delete allocBufferManager;
            allocBufferManager = nullptr;
        }
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
        if (reservedEnd > reservedStart) {
            Sanitizer::OnHeapDeallocated(reinterpret_cast<void*>(reservedStart), reservedEnd - reservedStart);
        }
#endif
        // Members are destroyed after this body in reverse declaration order:
        // regionManager (whose mapped caches keep entries in heap memory)
        // first, then the two managers release backing and address space.
    }

    void Init(const HeapParam&) override;

    MAddress Allocate(size_t size, AllocType allocType) override;

    RegionManager& GetRegionManager() noexcept { return regionManager; }

    MAddress GetSpaceStartAddress() const override { return reservedStart; }

    MAddress GetSpaceEndAddress() const override { return reservedEnd; }

    size_t GetCurrentCapacity() const override { return regionManager.GetActiveUnitCount() * ZPage::UNIT_SIZE; }
    size_t GetMaxCapacity() const override { return regionManager.GetHeapCapacity(); }

    ZMemoryUsageInfo GetMemoryUsage() const
    {
        // ZHeap::used_generation -> ZPageAllocator::used_generation. Use
        // page occupancy for both generations, never object bytes minus pages.
        const size_t young = regionManager.GetYoungAllocatedSize();
        const size_t used = regionManager.GetUsedRegionSize();
        const size_t old = used - std::min(used, young);
        return ComputeMemoryUsageInfo(regionManager.GetCommittedCapacity(), GetMaxCapacity(), young, old);
    }


    inline size_t GetRecentAllocatedSize() const { return regionManager.GetRecentAllocatedSize(); }

    // size of objects survived in previous gc.
    inline size_t GetSurvivedSize() const { return regionManager.GetSurvivedSize(); }

    size_t GetUsedPageSize() const override { return regionManager.GetUsedRegionSize(); }

    inline size_t GetTargetSize() const
    {
        double heapUtilization = CangjieRuntime::GetHeapParam().heapUtilization;
        return static_cast<size_t>(GetUsedPageSize() / heapUtilization);
    }

    size_t AllocatedBytes() const override { return regionManager.GetAllocatedSize(); }

    size_t LargeObjectBytes() const override { return regionManager.GetLargeObjectSize(); }

    size_t FromSpaceSize() const { return regionManager.GetFromSpaceSize(); }

    size_t PinnedSpaceSize() const { return regionManager.GetPinnedSpaceSize(); }

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool IsHeapObject(MAddress addr) const override;
#endif

    // info dump
    void GetInstances(const TypeInfo*, bool, size_t, std::vector<MObject*>&) const {}
    void ClassInstanceNum(std::map<CString, long>&) const {}

    size_t ReclaimGarbageMemory(bool /* releaseAll */) override
    {
        const size_t cachedBefore = regionManager.GetDirtyUnitCount() * ZPage::UNIT_SIZE;
        MRT_PHASE_TIMER(ZStatPhases::PReclaimGarbageRegions);
        // zPageAllocator.cpp: free pages return to the mapped cache. Physical
        // uncommit belongs to zUncommitter.cpp:367-421, including OOM reclaim.
        regionManager.ReclaimGarbageRegions();
        const size_t cachedAfter = regionManager.GetDirtyUnitCount() * ZPage::UNIT_SIZE;
        return cachedAfter > cachedBefore ? cachedAfter - cachedBefore : 0;
    }
#if defined(__EULER__)
    void TryReclaimGarbageMemory() override
    {
        ReclaimGarbageMemory(false);
    }
#endif
    bool ForEachObj(const std::function<void(BaseObject*)>& visitor, bool safe) const override
    {
        if (UNLIKELY(safe)) {
            regionManager.ForEachObjSafe(visitor);
        } else {
            regionManager.ForEachObjUnsafe(visitor);
        }
        return true;
    }

    // Return the garbage size of from space.
    size_t RefineFromSpace()
    {
        MRT_PHASE_TIMER(ZStatPhases::PExemptFromRegions);
        return regionManager.ExemptFromRegions();
    }



    template<Generation G>
    void PrepareFromSpace() { regionManager.PrepareFromRegionList<G>(); }


    template<Generation G>
    void ForwardFromSpace(ZWorkers& workers)
    {
        MRT_PHASE_TIMER(G == Generation::Young ? ZStatPhases::YoungForwardFromRegions :
                        ZStatPhases::OldForwardFromRegions);
        regionManager.ForwardFromRegions<G>(workers);
    }

    size_t CollectLargeGarbage() { return regionManager.CollectLargeGarbage(); }

    size_t CollectPinnedGarbage() { return regionManager.CollectPinnedGarbage(); }

    void CollectFromSpaceGarbage()
    {
        regionManager.CollectFromSpaceGarbage();
        regionManager.ReassembleFromSpace();
    }

    void AssembleGarbageCandidates(bool collectAll = false)
    {
        regionManager.AssembleSmallGarbageCandidates();
        regionManager.AssemblePinnedGarbageCandidates(collectAll);
        regionManager.AssembleLargeGarbageCandidates();
    }

    void DumpRegionStats(const char* msg) const
    {
        regionManager.DumpRegionStats(msg);
    }

    void CountLiveObject(const BaseObject* obj) { regionManager.CountLiveObject(obj); }

    void PrepareTrace() { regionManager.PrepareTrace(); }
    void FeedHungryBuffers() override;


    // ZPage::mark_object + inc_live (zMark.cpp:405-425) for a caller without a
    // ZMarkCache: the first live claim is accounted on the page directly.
    template<Generation G>
    static bool MarkObject(const BaseObject* obj)
    {
        ZPage* regionInfo = Heap::page(reinterpret_cast<MAddress>(obj));
        (void)G;
        bool incLive = false;
        const bool newlyMarked = regionInfo->mark_object(from_object(obj), false, incLive);
        if (incLive) {
            regionInfo->inc_live(1, obj->GetSize());
        }
        return !newlyMarked;
    }

    // ZPage::is_object_strongly_live (zPage.inline.hpp:258-260).
    template<Generation G>
    static bool IsMarkedObject(const BaseObject* obj)
    {
        (void)G;
        ZPage* regionInfo = Heap::page(reinterpret_cast<MAddress>(obj));
        return regionInfo->is_object_strongly_live(from_object(obj));
    }

    template<Generation G>
    static bool ShouldEnqueue(const BaseObject* obj)
    {
        ZPage* regionInfo = Heap::page(reinterpret_cast<MAddress>(obj));
        if (regionInfo == nullptr || regionInfo->IsFreeRegion() || regionInfo->IsGarbageRegion() ||
            regionInfo->IsFreeRegion()) {
            return false;
        }
        (void)G;
        // ZGC SATB entries are not suppressed by an independent enqueue
        // bitmap.  The mark pair is the sole epoch authority; until the strong
        // bit is visible, every observation remains eligible for publication.
        return !regionInfo->is_object_strongly_live(from_object(obj));
    }

    // Finalizable-only marked: the live bit without the strong bit
    // (zPage.inline.hpp:254-260 pair semantics).
    static bool IsResurrectedObject(const BaseObject* obj)
    {
        ZPage* regionInfo = Heap::page(reinterpret_cast<MAddress>(obj));
        const zaddress addr = from_object(obj);
        return regionInfo->is_object_live(addr) && !regionInfo->is_object_strongly_live(addr);
    }

    void AddRawPointerObject(BaseObject* obj) { regionManager.AddRawPointerObject(obj); }

    void RemoveRawPointerObject(BaseObject* obj) { regionManager.RemoveRawPointerObject(obj); }

    friend class Allocator;

private:
    MAddress TryAllocateOnce(size_t allocSize, AllocType allocType);
    MAddress reservedStart = 0;
    MAddress reservedEnd = 0;
    // Reverse per-unit metadata array (ABI adapter, outside the heap address
    // domain; ZPage descriptors live on the C heap in ZGC).
    struct MetadataMapping {
        void* base{ nullptr };
        size_t size{ 0 };
        ~MetadataMapping()
        {
            if (base != nullptr) {
                (void)munmap(base, size);
            }
        }
    } metadata;
    // ZPageAllocator::_virtual / _physical (zPageAllocator.hpp:196-197),
    // constructed for max_capacity once the heap parameters are known.
    // Declared before regionManager so they outlive its mapped caches.
    std::unique_ptr<ZVirtualMemoryManager> virtualMemory;
    std::unique_ptr<ZPhysicalMemoryManager> physicalMemory;
    RegionManager regionManager;
};
} // namespace MapleRuntime
#endif // MRT_REGION_SPACE_H
