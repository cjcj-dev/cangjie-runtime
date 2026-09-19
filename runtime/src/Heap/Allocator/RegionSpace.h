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
namespace MapleRuntime {
extern const ZStatSubPhase OldForwardFromRegions;
extern const ZStatSubPhase PExemptFromRegions;
extern const ZStatCriticalPhase PReclaimGarbageRegions;
extern const ZStatSubPhase YoungForwardFromRegions;
}

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

    }

    void Init(const HeapParam&) override;

    MAddress Allocate(size_t size, AllocType allocType) override;

    RegionManager& GetRegionManager() const noexcept;

    MAddress GetSpaceStartAddress() const override { return GetRegionManager().GetSpaceStartAddress(); }

    MAddress GetSpaceEndAddress() const override { return GetRegionManager().GetSpaceEndAddress(); }

    size_t GetCurrentCapacity() const override { return GetRegionManager().GetCommittedBytes(); }
    size_t GetMaxCapacity() const override { return GetRegionManager().GetHeapCapacity(); }

    ZMemoryUsageInfo GetMemoryUsage() const
    {
        // ZHeap::used_generation -> ZPageAllocator::used_generation. Use
        // page occupancy for both generations, never object bytes minus pages.
        const size_t young = GetRegionManager().used_generation(ZGenerationId::young);
        const size_t old = GetRegionManager().used_generation(ZGenerationId::old);
        return ComputeMemoryUsageInfo(GetRegionManager().GetCommittedCapacity(), GetMaxCapacity(), young, old);
    }


    inline size_t GetRecentAllocatedSize() const { return GetRegionManager().GetRecentAllocatedSize(); }

    // size of objects survived in previous gc.
    inline size_t GetSurvivedSize() const { return GetRegionManager().GetSurvivedSize(); }

    size_t GetUsedPageSize() const override { return GetRegionManager().GetUsedRegionSize(); }

    inline size_t GetTargetSize() const
    {
        double heapUtilization = CangjieRuntime::GetHeapParam().heapUtilization;
        return static_cast<size_t>(GetUsedPageSize() / heapUtilization);
    }

    size_t AllocatedBytes() const override { return GetRegionManager().GetAllocatedSize(); }

    size_t LargeObjectBytes() const override { return GetRegionManager().GetLargeObjectSize(); }

    size_t FromSpaceSize() const { return GetRegionManager().GetFromSpaceSize(); }

    size_t PinnedSpaceSize() const { return GetRegionManager().GetPinnedSpaceSize(); }

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool IsHeapObject(MAddress addr) const override;
#endif

    // info dump
    void GetInstances(const TypeInfo*, bool, size_t, std::vector<MObject*>&) const {}
    void ClassInstanceNum(std::map<CString, long>&) const {}

    size_t ReclaimGarbageMemory(bool /* releaseAll */) override
    {
        const size_t cachedBefore = GetRegionManager().GetCachedBytes();
        ZStatTimerWorker zstatTimer(PReclaimGarbageRegions);
        // zPageAllocator.cpp: free pages return to the mapped cache. Physical
        // uncommit belongs to zUncommitter.cpp:367-421, including OOM reclaim.
        GetRegionManager().ReclaimGarbageRegions();
        const size_t cachedAfter = GetRegionManager().GetCachedBytes();
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
            GetRegionManager().ForEachObjSafe(visitor);
        } else {
            GetRegionManager().ForEachObjUnsafe(visitor);
        }
        return true;
    }

    // Return the garbage size of from space.
    size_t RefineFromSpace()
    {
        ZStatTimerWorker zstatTimer(PExemptFromRegions);
        return GetRegionManager().ExemptFromRegions();
    }




    template<Generation G>
    void ForwardFromSpace(ZWorkers& workers)
    {
        ZStatTimerWorker zstatTimer(G == Generation::Young ? YoungForwardFromRegions :
                                    OldForwardFromRegions);
        GetRegionManager().ForwardFromRegions<G>(workers);
    }

    size_t CollectLargeGarbage() { return GetRegionManager().CollectLargeGarbage(); }

    size_t CollectPinnedGarbage() { return GetRegionManager().CollectPinnedGarbage(); }

    void CollectFromSpaceGarbage()
    {
        GetRegionManager().CollectFromSpaceGarbage();
    }

    void AssembleGarbageCandidates(bool collectAll = false)
    {
        GetRegionManager().AssembleSmallGarbageCandidates();
        GetRegionManager().AssemblePinnedGarbageCandidates(collectAll);
        GetRegionManager().AssembleLargeGarbageCandidates();
    }

    void DumpRegionStats(const char* msg) const
    {
        GetRegionManager().DumpRegionStats(msg);
    }

    void CountLiveObject(const BaseObject* obj) { GetRegionManager().CountLiveObject(obj); }

    void PrepareTrace() { GetRegionManager().PrepareTrace(); }


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

    void AddRawPointerObject(BaseObject* obj) { GetRegionManager().AddRawPointerObject(obj); }

    void RemoveRawPointerObject(BaseObject* obj) { GetRegionManager().RemoveRawPointerObject(obj); }

    friend class Allocator;

private:
    MAddress TryAllocateOnce(size_t allocSize, AllocType allocType);

};
} // namespace MapleRuntime
#endif // MRT_REGION_SPACE_H
