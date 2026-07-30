// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGION_MANAGER_H
#define MRT_REGION_MANAGER_H

#include <atomic>
#include <list>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "AllocBuffer.h"
#include "Allocator.h"
#include "Common/RunType.h"
#include "FreeRegionManager.h"
#include "Heap/GcThreadPool.h"
#include "RegionList.h"
#include "securec.h"
#include "SlotList.h"
#include "Sync/Sync.h"

namespace MapleRuntime {
class CopyCollector;
class CompactCollector;
#ifdef MRT_REGION_EPOCH_TEST
void RouteRecordAfterAcquireForTest(RegionInfo* region);
#endif

struct YoungCollectionStats {
    size_t candidateRegions = 0;
    size_t candidateBytes = 0;
    size_t reclaimedRegions = 0;
    size_t reclaimedBytes = 0;
};

enum class YoungAccountingSource : uint8_t {
    NEW_REGION,
    REUSED_GARBAGE_REGION,
    REUSED_FREE_REGION,
};

struct YoungAccountingStats {
    size_t gcOrdinal = 0;
    size_t accountedBytes = 0;
    size_t measuredObjectBytes = 0;
    size_t objectAllocPointerBytes = 0;
    size_t pinnedSlotBytes = 0;
    size_t actualBytes = 0;
    size_t heapBaselineBytes = 0;
    size_t heapCurrentBytes = 0;
    size_t objectBaselineBytes = 0;
    size_t objectCurrentBytes = 0;
    size_t regionCapacityBaselineBytes = 0;
    size_t regionCapacityCurrentBytes = 0;
    size_t tailBaselineBytes = 0;
    size_t tailCurrentBytes = 0;
    size_t rawPrivateBaselineBytes = 0;
    size_t rawPrivateCurrentBytes = 0;
    int64_t conservationErrorBytes = 0;
    size_t validationObjectBytes = 0;
    size_t validationBaselineBytes = 0;
    size_t validationCurrentBytes = 0;
    int64_t validationErrorBytes = 0;
    uint64_t allocPointerScanNs = 0;
    uint64_t validationScanNs = 0;
    size_t newRegionEvents = 0;
    size_t newRegionBytes = 0;
    size_t reusedGarbageEvents = 0;
    size_t reusedGarbageBytes = 0;
    size_t reusedFreeEvents = 0;
    size_t reusedFreeBytes = 0;
};

struct FreePinnedSlotLists {
    static constexpr size_t ATOMIC_OBJECT_SIZE = 16;
    static constexpr size_t SYNC_OBJECT_SIZE = CJFuture::SYNC_OBJECT_SIZE;
    SlotList freeAtomicSlotList;
    SlotList freeSyncSlotList;

    uintptr_t PopFront(size_t size)
    {
        switch (size) {
            case ATOMIC_OBJECT_SIZE:
                return freeAtomicSlotList.PopFront(size);
            case SYNC_OBJECT_SIZE:
                return freeSyncSlotList.PopFront(size);
            default:
                return 0;
        }
    }

    void PushFront(BaseObject* slot)
    {
        size_t size = slot->GetSize();
        switch (size) {
            case ATOMIC_OBJECT_SIZE:
                freeAtomicSlotList.PushFront(slot);
                break;
            case SYNC_OBJECT_SIZE:
                freeSyncSlotList.PushFront(slot);
                break;
            default:
                return;
        }
    }

    void Clear()
    {
        freeAtomicSlotList.Clear();
        freeSyncSlotList.Clear();
    }
};

// RegionManager needs to know header size and alignment in order to iterate objects linearly
// and thus its Alloc should be rewrite with AllocObj(objSize)
class RegionManager {
public:
    /* region memory layout:
        1. region info for each region, part of heap metadata
        2. region space for allocation, i.e., the heap
    */
    static size_t GetHeapMemorySize(size_t heapSize)
    {
        size_t unitNum = GetHeapUnitCount(heapSize);
        size_t metadataSize = GetMetadataSize(unitNum);
        size_t totalSize = metadataSize + RoundUp<size_t>(heapSize, RegionInfo::UNIT_SIZE);
        return totalSize;
    }

    static size_t GetHeapUnitCount(size_t heapSize)
    {
        heapSize = RoundUp<size_t>(heapSize, RegionInfo::UNIT_SIZE);
        size_t unitNum = heapSize / RegionInfo::UNIT_SIZE;
        return unitNum;
    }

    // get metadataSize by regionNum or unitNumber
    // RegionInfo and UnitInfo have the same sizeof
    static size_t GetMetadataSize(size_t num)
    {
        size_t metadataSize = num * sizeof(RegionInfo);
        return RoundUp<size_t>(metadataSize, MapleRuntime::MRT_PAGE_SIZE);
    }
#if defined(__EULER__)
    void SetCacheRatio(double minSize, double maxSize, double defaultParam);
#endif
    void Initialize(size_t regionNum, uintptr_t regionInfoStart);

    RegionManager()
        : freeRegionManager(*this), tlRegionList("thread local regions"), recentFullRegionList("recent full regions"),
          fullTraceRegions("full trace regions"), fromRegionList("from regions"),
          ghostFromRegionList("ghost from regions"), unmovableFromRegionList("escaped from regions"),
          garbageRegionList("garbage regions"), recentPinnedRegionList("recent pinned regions"),
          oldPinnedRegionList("old pinned regions"), rawPointerPinnedRegionList("raw pointer pinned regions"),
          oldLargeRegionList("old large regions"), recentLargeRegionList("recent large regions"),
          largeTraceRegions("large trace regions")
    {}

    RegionManager(const RegionManager&) = delete;

    RegionManager& operator=(const RegionManager&) = delete;

    RegionInfo* AllocateThreadLocalRegion(bool expectPhysicalMem = false);

    void ForwardFromRegions(GCThreadPool* threadPool);
    void ForwardFromRegions();
    void ForwardRegion(RegionInfo* region);
    void CompactRegion(RegionInfo* region);
    void CompactRegion(RegionInfo* region, RegionInfo* toRegion1);

    void ExemptFromRegion(RegionInfo* region);

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    void DumpRegionInfo() const;
#endif

    void DumpRegionStats(const char* msg, bool dumpToError = false) const;

    uintptr_t GetInactiveZone() const { return inactiveZone; }

#if defined(__EULER__)
    double GetCacheRatio() const { return cacheRatio; }
#endif

    uintptr_t GetRegionHeapStart() const { return regionHeapStart; }

    ~RegionManager() = default;

    // take a region with *num* units for allocation
    RegionInfo* TakeRegion(size_t num, RegionInfo::UnitRole, bool expectPhysicalMem = false);

    uintptr_t AllocPinnedFromFreeList(size_t size);

    uintptr_t AllocPinned(size_t size)
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
            GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase();
            if (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER) {
                region->SetTraceRegionFlag(1);
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
            (void)CollectRegion(region);
        }

        DLOG(ALLOC, "alloc pinned obj 0x%zx(%zu)", addr, size);
        return addr;
    }

    // note: AllocSmall() is always performed by region owned by mutator thread
    // thus no need to do in RegionManager
    // caller assures size is truely large (> region size)
    uintptr_t AllocLarge(size_t size)
    {
        size_t regionCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;
        RegionInfo* region = TakeRegion(regionCount, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        if (region == nullptr) {
            return 0;
        }
        DLOG(REGION, "alloc large region @[0x%zx+%zu, 0x%zx) unit idx %zu type %u", region->GetRegionStart(),
             region->GetRegionSize(), region->GetRegionEnd(), region->GetUnitIdx(), region->GetRegionType());
        uintptr_t addr = region->Alloc(size);

        GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase();
        bool shouldSetTraceRegion = (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER);
        if (largeTraceRegions.TryPrependRegion(region, RegionInfo::RegionType::RECENT_LARGE_REGION)) {
            if (shouldSetTraceRegion) {
                region->SetTraceRegionFlag(1);
            }
        } else {
            recentLargeRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_LARGE_REGION);
            region->SetTraceRegionFlag(0);
        }

        return addr;
    }

    void EnlistFullThreadLocalRegion(RegionInfo* region) noexcept
    {
        MRT_ASSERT(region->IsThreadLocalRegion(), "unexpected region type");

        if (region->IsTraceRegion()) {
            if (!fullTraceRegions.TryPrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION)) {
                recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
                region->SetTraceRegionFlag(0);
            }
            return;
        }
        recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
    }

    void RemoveThreadLocalRegion(RegionInfo* region) noexcept
    {
        MRT_ASSERT(region->IsThreadLocalRegion(), "unexpected region type");
        tlRegionList.DeleteRegion(region);
    }

    void RestoreToSpaceStateWords();

    void CountLiveObject(const BaseObject* obj);

    void AssembleSmallGarbageCandidates();
    void AssembleLargeGarbageCandidates();
    void AssemblePinnedGarbageCandidates(bool collectAll);
    YoungCollectionStats PrepareYoungGarbageCandidates(const std::function<void(RegionInfo*)>& visitor);
    void CollectYoungGarbage(YoungCollectionStats& stats, const std::function<void(RegionInfo*)>& promoteVisitor);
    void PromoteAllRegions();

    void MergeRawPointerPinnedRegions()
    {
        oldPinnedRegionList.MergeRegionList(rawPointerPinnedRegionList, RegionInfo::RegionType::FULL_PINNED_REGION);
    }

    void CollectFromSpaceGarbage()
    {
#if defined(__OHOS__)
        // OHOS keeps the low-fragmentation path for ordinary regions. A ghost carrier
        // remains in the garbage list until PrepareFromRegionList dispels it.
        RegionInfo* region = fromRegionList.TakeHeadRegion();
        while (region != nullptr) {
            if (region->IsGhostFromRegion()) {
                region->BumpSnapshotEpoch();
                garbageRegionList.PrependRegion(region, RegionInfo::RegionType::GARBAGE_REGION);
            } else {
                ReclaimRegion(region);
            }
            region = fromRegionList.TakeHeadRegion();
        }
#else
        // GARBAGE ends the retained snapshot, but the ghost route remains in service
        // until route teardown or the region is actually returned to the allocator.
        // Plain loop (no lambda) so nm -D export surface stays free of std::function glue.
        for (RegionInfo* r = fromRegionList.GetHeadRegion(); r != nullptr; r = r->GetNextRegion()) {
            r->BumpSnapshotEpoch();
        }
        garbageRegionList.MergeRegionList(fromRegionList, RegionInfo::RegionType::GARBAGE_REGION);
#endif
    }

    size_t GetThreadLocalRegionSize() const
    {
        return maxUnitCountPerRegion * RegionInfo::UNIT_SIZE;
    }

    size_t CollectRegion(RegionInfo* region)
    {
        DLOG(REGION, "collect region %p@[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
             region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType());

        region->LockWriteRegion();
        // GARBAGE invalidates the snapshot domain only. OHOS advances identity later in
        // InitFreeUnits; non-OHOS keeps the installed route valid until actual reclaim.
        region->BumpSnapshotEpoch();
#if defined(__OHOS__)
        // Do not publish an installed ghost carrier to dirtyTree before its dispel point.
        if (region->IsGhostFromRegion()) {
            garbageRegionList.PrependRegion(region, RegionInfo::RegionType::GARBAGE_REGION);
        } else {
            ReclaimRegion(region);
        }
#else
        garbageRegionList.PrependRegion(region, RegionInfo::RegionType::GARBAGE_REGION);
#endif
        region->UnlockWriteRegion();

        if (region->IsLargeRegion()) {
            return region->GetRegionSize();
        } else {
            return region->GetRegionSize() - region->GetLiveByteCount();
        }
    }

    void AddRawPointerObject(BaseObject* obj)
    {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        region->IncRawPointerObjectCount();
        if (region->IsFromRegion() && fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                           RegionInfo::RegionType::RAW_POINTER_PINNED_REGION)) {
            GCPhase phase = Heap::GetHeap().GetGCPhase();
            CHECK(phase != GCPhase::GC_PHASE_FORWARD && phase != GCPhase::GC_PHASE_PREFORWARD);
            if (phase == GCPhase::GC_PHASE_POST_TRACE) {
                region->ClearGhostRegionBit();
            }
            rawPointerPinnedRegionList.PrependRegion(region, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
        } else {
            CHECK(region->GetRegionType() != RegionInfo::RegionType::LONE_FROM_REGION);
        }
    }

    void RemoveRawPointerObject(BaseObject* obj)
    {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        region->DecRawPointerObjectCount();
    }

    void ReclaimRegion(RegionInfo* region);
    size_t ReleaseRegion(RegionInfo* region);

    __attribute__((used)) void ReclaimGarbageRegions()
    {
        RegionInfo* garbage = TakeReclaimableGarbageRegion();
        while (garbage != nullptr) {
            ReclaimRegion(garbage);
            garbage = TakeReclaimableGarbageRegion();
        }
    }

    size_t CollectLargeGarbage();

    size_t CollectPinnedGarbage();
    size_t CollectFreePinnedSlots(RegionInfo* region);

    // targetSize: size of memory which we do not release and keep it as cache for future allocation.
    size_t ReleaseGarbageRegions(size_t targetSize) { return freeRegionManager.ReleaseGarbageRegions(targetSize); }

    // Ignore dynamic pinned regions and from regions whose garbage objects are quite few, return the garbage size that
    // can be reclaimed.
    size_t ExemptFromRegions();
    void ReassembleFromSpace();

    void ForEachObjUnsafe(const std::function<void(BaseObject*)>& visitor) const;
    void ForEachObjSafe(const std::function<void(BaseObject*)>& visitor) const;

    size_t GetUsedRegionSize() const { return GetUsedUnitCount() * RegionInfo::UNIT_SIZE; }

    size_t GetRecentAllocatedSize() const
    {
        return recentFullRegionList.GetAllocatedSize() + recentLargeRegionList.GetAllocatedSize() +
            recentPinnedRegionList.GetAllocatedSize();
    }

    size_t GetYoungAllocatedSize() const
    {
        return youngAllocatedBytes.load(std::memory_order_relaxed);
    }

    __attribute__((visibility("hidden"))) YoungAccountingStats SnapshotYoungAccounting();
    __attribute__((visibility("hidden")))
    void ReportYoungAccounting(const YoungAccountingStats& stats, const char* collectionKind) const;
    __attribute__((visibility("hidden"))) void SetYoungAccountingHeapBaseline();

    size_t GetSurvivedSize() const
    {
        return fromRegionList.GetAllocatedSize() + oldPinnedRegionList.GetAllocatedSize() +
            oldLargeRegionList.GetAllocatedSize();
    }

    size_t GetUsedUnitCount() const
    {
        return
            fromRegionList.GetUnitCount() + unmovableFromRegionList.GetUnitCount() +
            recentFullRegionList.GetUnitCount() + oldLargeRegionList.GetUnitCount() +
            recentLargeRegionList.GetUnitCount() + oldPinnedRegionList.GetUnitCount() +
            recentPinnedRegionList.GetUnitCount() + rawPointerPinnedRegionList.GetUnitCount() +
            largeTraceRegions.GetUnitCount() + fullTraceRegions.GetUnitCount() +
            tlRegionList.GetUnitCount();
    }

    size_t GetDirtyUnitCount() const { return freeRegionManager.GetDirtyUnitCount(); }

    size_t GetInactiveUnitCount() const { return (regionHeapEnd - inactiveZone) / RegionInfo::UNIT_SIZE; }

    size_t GetActiveUnitCount() const { return (inactiveZone - regionHeapStart) / RegionInfo::UNIT_SIZE; }

    inline size_t GetLargeObjectSize() const
    {
        return oldLargeRegionList.GetAllocatedSize() + recentLargeRegionList.GetAllocatedSize();
    }

    size_t GetAllocatedSize() const
    {
        size_t threadLocalSize = 0;
        AllocBufferVisitor visitor = [&threadLocalSize](AllocBuffer& regionBuffer) {
            RegionInfo* region = regionBuffer.GetRegion();
            if (UNLIKELY(region == RegionInfo::NullRegion())) {
                return;
            }
            threadLocalSize += region->GetRegionAllocatedSize();
        };
        Heap::GetHeap().GetAllocator().VisitAllocBuffers(visitor);
        // exclude garbageRegionList for live object set.
        return fromRegionList.GetAllocatedSize() + unmovableFromRegionList.GetAllocatedSize() +
            recentFullRegionList.GetAllocatedSize() + oldLargeRegionList.GetAllocatedSize() +
            recentLargeRegionList.GetAllocatedSize() + oldPinnedRegionList.GetAllocatedSize() +
            recentPinnedRegionList.GetAllocatedSize() + rawPointerPinnedRegionList.GetAllocatedSize() +
            largeTraceRegions.GetAllocatedSize() + fullTraceRegions.GetAllocatedSize() +
            threadLocalSize;
    }

    inline size_t GetFromSpaceSize() const { return fromRegionList.GetAllocatedSize(); }

    inline size_t GetPinnedSpaceSize() const
    {
        return oldPinnedRegionList.GetAllocatedSize() + recentPinnedRegionList.GetAllocatedSize();
    }

    size_t GetLargeObjectThreshold() const { return largeObjectThreshold; }

    void ClearFreePinnedSlots() { freePinnedSlotLists.Clear(); }

    // wait for a period of time to allocate region which will avoid harm to gc
    void RequestForRegion(size_t size);

    void MergeRawPointerRegions(RegionList& smallSizeRegionList, RegionList& largeSizeRegionList)
    {
        recentFullRegionList.MergeRegionList(smallSizeRegionList, RegionInfo::RegionType::RECENT_FULL_REGION);
        recentLargeRegionList.MergeRegionList(largeSizeRegionList, RegionInfo::RegionType::RECENT_LARGE_REGION);
    }

    void SetMaxUnitCountForRegion();
    void SetMaxUnitCountForPinnedRegion();
    void SetLargeObjectThreshold();
    void SetGarbageThreshold();

    void HandleTraceRegions()
    {
        fullTraceRegions.DeactivateRegionCache();
        recentFullRegionList.MergeRegionList(fullTraceRegions, RegionInfo::RegionType::RECENT_FULL_REGION);

        largeTraceRegions.DeactivateRegionCache();
        recentLargeRegionList.MergeRegionList(largeTraceRegions, RegionInfo::RegionType::RECENT_LARGE_REGION);

        tlRegionList.ClearTraceRegionFlag();
        recentPinnedRegionList.ClearTraceRegionFlag();
        oldPinnedRegionList.ClearTraceRegionFlag();
    }

    void PrepareTrace()
    {
        fullTraceRegions.ActivateRegionCache();
        largeTraceRegions.ActivateRegionCache();
    }

    bool RouteOrCompactRegionImpl(RegionInfo* region);

    ALWAYS_INLINE BaseObject* RouteObject(BaseObject* fromObj, RegionInfo* fromRegionInfo, uint64_t expectedEpoch)
    {
        // The caller captures expectedEpoch when it establishes its view of this region.
        // Identity turnover between that recognition and routing is genuine staleness — loud.
        if (UNLIKELY(expectedEpoch != fromRegionInfo->GetIdentityEpoch())) {
            size_t n = routeEpochMismatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[RouteObject] epoch_mismatch region=%p epoch_seen=%llu epoch_now=%llu "
                     "route_epoch=%llu state=%u n=%zu",
                     fromRegionInfo, static_cast<unsigned long long>(expectedEpoch),
                     static_cast<unsigned long long>(fromRegionInfo->GetIdentityEpoch()),
                     static_cast<unsigned long long>(fromRegionInfo->GetRouteInstallEpoch()),
                     static_cast<unsigned>(fromRegionInfo->GetRouteState()), n);
            }
#ifndef MRT_REGION_EPOCH_TEST
            CHECK_DETAIL(false,
                         "RouteObject identity expired before route lookup: region=%p seen=%llu now=%llu",
                         fromRegionInfo, static_cast<unsigned long long>(expectedEpoch),
                         static_cast<unsigned long long>(fromRegionInfo->GetIdentityEpoch()));
#endif
            return nullptr;
        }
        // Absent ghost guard with a CURRENT identity is a defined negative answer, not
        // staleness: seen==now proves no teardown intervened since capture, so the caller
        // never established ghost lineage — it is probing (R1 bulk-fix consumption pattern,
        // routine post-k8 where reuse retires carriers). Defined miss: count + nullptr.
        if (UNLIKELY(!fromRegionInfo->IsGhostFromRegion())) {
            size_t n = routeNotGhostProbeCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT, "[RouteObject] not_ghost_probe_miss region=%p identity=%llu state=%u n=%zu",
                     fromRegionInfo, static_cast<unsigned long long>(expectedEpoch),
                     static_cast<unsigned>(fromRegionInfo->GetRouteState()), n);
            }
            return nullptr;
        }
        if (RouteRegion(fromRegionInfo) || fromRegionInfo->IsCompacted()) {
            // Merge synthesis (k6 domain split × k7 seqlock snapshot):
            // identity domain gates consumption (ruling A: routes bind to identityEpoch);
            // presence and installation stamp come from the acquired IMMUTABLE by-value
            // snapshot. k6's post-geometry recheck window is structurally superseded —
            // a local snapshot cannot change under the reader, and the install-stamp
            // check below already pins the route to the caller-held identity.
            RouteInfo routeInfo = fromRegionInfo->AcquireRouteInfo();
            if (UNLIKELY(expectedEpoch != fromRegionInfo->GetIdentityEpoch() ||
                         !routeInfo.IsInstalled() || routeInfo.GetInstallEpoch() != expectedEpoch)) {
                size_t n = routeEpochMismatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((n & (n - 1)) == 0) {
                    VLOG(REPORT,
                         "[RouteObject] epoch_mismatch region=%p epoch_seen=%llu epoch_now=%llu "
                         "route_present=%u route_epoch=%llu state=%u n=%zu",
                         fromRegionInfo, static_cast<unsigned long long>(expectedEpoch),
                         static_cast<unsigned long long>(fromRegionInfo->GetIdentityEpoch()),
                         static_cast<unsigned>(routeInfo.IsInstalled()),
                         static_cast<unsigned long long>(routeInfo.GetInstallEpoch()),
                         static_cast<unsigned>(fromRegionInfo->GetRouteState()), n);
                }
#ifndef MRT_REGION_EPOCH_TEST
                CHECK_DETAIL(false,
                             "RouteObject route carrier expired before geometry read: region=%p seen=%llu "
                             "now=%llu route=%llu",
                             fromRegionInfo, static_cast<unsigned long long>(expectedEpoch),
                             static_cast<unsigned long long>(fromRegionInfo->GetIdentityEpoch()),
                             static_cast<unsigned long long>(routeInfo.GetInstallEpoch()));
#endif
                return nullptr;
            }
#ifdef MRT_REGION_EPOCH_TEST
            RouteRecordAfterAcquireForTest(fromRegionInfo);
#endif
            return fromRegionInfo->GetRoute(fromObj, routeInfo);
        }
        return nullptr;
    }

    // ABI-compatible wrapper. Internal GC readers carry caller-held expectedEpoch
    // through the three-argument overload above.
    BaseObject* RouteObject(BaseObject* fromObj, RegionInfo* fromRegionInfo)
    {
        return RouteObject(fromObj, fromRegionInfo, fromRegionInfo->GetIdentityEpoch());
    }

    BaseObject* RouteObject(BaseObject* fromObj)
    {
        RegionInfo* fromRegionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(fromObj));
        if (fromRegionInfo == nullptr) {
            return nullptr;
        }

        const uint64_t expectedEpoch = fromRegionInfo->GetIdentityEpoch();
        return RouteObject(fromObj, fromRegionInfo, expectedEpoch);
    }

    size_t GetRouteEpochMismatchCount() const
    {
        return routeEpochMismatchCount.load(std::memory_order_relaxed);
    }

    void ResetRouteEpochMismatchCount()
    {
        routeEpochMismatchCount.store(0, std::memory_order_relaxed);
    }

    bool RouteRegion(RegionInfo* fromRegionInfo)
    {
        // A non-ghost region is simply not routable — a defined negative answer for
        // probing consumers (TryForward family calls this directly; post-k8 reuse
        // retires carriers routinely). The old hard CHECK assumed every caller
        // pre-established ghost lineage, which the R1 probing pattern never promised.
        if (UNLIKELY(!fromRegionInfo->IsGhostFromRegion())) {
            return false;
        }
        do {
            RegionInfo::RouteState oldState = fromRegionInfo->GetRouteState();
            if (oldState == RegionInfo::RouteState::ROUTED || oldState == RegionInfo::RouteState::FORWARDED) {
                return true;
            }
            if (oldState == RegionInfo::RouteState::COMPACTED) {
                return false;
            }
            if (oldState == RegionInfo::RouteState::ROUTING) {
                sched_yield();
                continue;
            }

            CHECK(oldState == MapleRuntime::RegionInfo::FORWARDABLE);
            if (fromRegionInfo->TryLockRouting(oldState)) {
                if (RouteOrCompactRegionImpl(fromRegionInfo)) {
                    fromRegionInfo->SetRouteState(RegionInfo::RouteState::ROUTED);
                    return true;
                } else {
                    fromRegionInfo->SetRouteState(RegionInfo::RouteState::COMPACTED);
                    return false;
                }
            }
        } while (true);
    }

    void PrepareFromRegionList()
    {
        size_t retainedRegions = 0;
        size_t retainedBytes = 0;
        RegionInfo* region = ghostFromRegionList.GetHeadRegion();
        while (region != nullptr) {
            RegionInfo* next = region->GetNextGhostRegion();
            if (region->IsGhostFromRegion() && region->IsGarbageRegion()) {
                ++retainedRegions;
                retainedBytes += region->GetGhostRegionSize();
            }
            region = next;
        }
        ghostFromRegionList.VisitAllGhostRegions([](RegionInfo* region) {
            DLOG(REGION, "visit ghost from region %p@[%#zx, %#zx)", region, region->GetRegionStart(),
                 region->GetRegionEnd());
            region->DispelGhostFromRegion();
        });
        region = ghostFromRegionList.GetHeadRegion();
        while (region != nullptr) {
            RegionInfo* next = region->GetNextGhostRegion();
            if (TryTakeGarbageRegionAfterDispel(region)) {
                ReclaimRegion(region);
            }
            region = next;
        }
        VLOG(REPORT, "[GhostRetention] retained_regions=%zu retained_bytes=%zu", retainedRegions, retainedBytes);

        fromRegionList.VisitAllRegions([](RegionInfo* region) {
            DLOG(REGION, "visit from region %p@[%#zx+%zu, %#zx)", region, region->GetRegionStart(),
                 region->GetLiveByteCount(), region->GetRegionEnd());
            region->PrepareForwardableRegion();
        });

        fromRegionList.CopyListTo(ghostFromRegionList);
    }

    void ClearAllLiveInfo()
    {
        ClearLiveInfo(tlRegionList);
        ClearLiveInfo(recentFullRegionList);
        ClearLiveInfo(fullTraceRegions);
        ClearLiveInfo(unmovableFromRegionList);
        ClearLiveInfo(recentPinnedRegionList);
        ClearLiveInfo(oldPinnedRegionList);
        ClearLiveInfo(rawPointerPinnedRegionList);
        ClearLiveInfo(oldLargeRegionList);
        ClearLiveInfo(recentLargeRegionList);
        ClearLiveInfo(largeTraceRegions);
    }

private:
    __attribute__((always_inline, visibility("hidden")))
    RegionInfo* TakeReclaimableGarbageRegion(size_t* gatedBytes = nullptr)
    {
        std::lock_guard<std::mutex> lock(garbageRegionList.GetListMutex());
        RegionInfo* candidate = nullptr;
        size_t bytes = 0;
        for (RegionInfo* region = garbageRegionList.GetHeadRegion(); region != nullptr;
             region = region->GetNextRegion()) {
            if (region->IsGhostFromRegion()) {
                bytes += region->GetGhostRegionSize();
            } else if (candidate == nullptr) {
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

    __attribute__((always_inline, visibility("hidden")))
    bool TryTakeGarbageRegionAfterDispel(RegionInfo* target)
    {
        std::lock_guard<std::mutex> lock(garbageRegionList.GetListMutex());
        for (RegionInfo* region = garbageRegionList.GetHeadRegion(); region != nullptr;
             region = region->GetNextRegion()) {
            if (region == target) {
                CHECK(region->IsGarbageRegion());
                CHECK(!region->IsGhostFromRegion());
                RemoveRegionLocked(&garbageRegionList, region);
                return true;
            }
        }
        return false;
    }

    __attribute__((always_inline, visibility("hidden"))) size_t GetGatedGarbageBytes()
    {
        std::lock_guard<std::mutex> lock(garbageRegionList.GetListMutex());
        size_t bytes = 0;
        for (RegionInfo* region = garbageRegionList.GetHeadRegion(); region != nullptr;
             region = region->GetNextRegion()) {
            if (region->IsGhostFromRegion()) {
                bytes += region->GetGhostRegionSize();
            }
        }
        return bytes;
    }

    // Acquire a region list mutex which the collector also takes while the world is stopped.
    // Waiting for it in a saferegion is required so that a contended mutator cannot stall
    // StopTheWorld (MutatorManager.cpp:485-490), but the mutex must never be owned while the
    // saferegion guard is destroyed: LeaveSaferegion() parks the mutator in SuspendForSync()
    // (Mutator.h:172-186, Mutator.cpp:229-280) and the collector would then wait for that mutex
    // forever. Wait in try-lock rounds so every saferegion transition happens unlocked, exactly
    // as FreeRegionManager::TakeRegion() does for the free unit trees (FreeRegionManager.h:45-92).
    static void LockRegionListInSaferegion(std::mutex& listMutex)
    {
        while (!listMutex.try_lock()) {
            ScopedEnterSaferegion enterSaferegion(true);
        }
    }

    // caller must own recentPinnedRegionList's list mutex, and must not release it in between.
    uintptr_t AllocPinnedLocked(size_t size)
    {
        uintptr_t addr = 0;
        RegionInfo* headRegion = recentPinnedRegionList.GetHeadRegion();
        if (headRegion != nullptr) {
            addr = headRegion->Alloc(size);
        }
        if (addr == 0) {
            addr = AllocPinnedFromFreeList(size);
        }
        return addr;
    }

    static const size_t MAX_UNIT_COUNT_PER_REGION;
    static const size_t HUGE_PAGE;
    inline void CheckRegionWhetherCreatedInFixPhase(RegionInfo* region);
    inline void TagHugePage(RegionInfo* region, size_t num) const;
    inline void UntagHugePage(RegionInfo* region, size_t num) const;

    void ClearLiveInfo(RegionList& list)
    {
        RegionList tmp("temp region list");
        list.CopyListTo(tmp);
        tmp.VisitAllRegions([](RegionInfo* region) { region->ClearLiveInfo(); });
    }

    FreeRegionManager freeRegionManager;

    // region lists actually represent life cycle of regions.
    // each region must belong to only one list at any time.

    // regions for movable (small-sized) objects.
    // regions for thread-local allocation.
    // regions in this list are already used for allocation but not full yet, i.e. local regions.
    RegionList tlRegionList;

    // recentFullRegionList is a list of regions which is already full, thus escape current gc.
    RegionList recentFullRegionList;

    // if region is allocated during gc trace phase, it is called a trace-region, it is recorded here when it is full.
    RegionCache fullTraceRegions;

    // fromRegionList is a list of full regions waiting to be collected (i.e. for forwarding).
    // region type must be FROM_REGION.
    RegionList fromRegionList;
    RegionList ghostFromRegionList;

    // regions exempted by ExemptFromRegions, which will not be moved during current GC.
    RegionList unmovableFromRegionList;

    // cache for fromRegionList after forwarding.
    RegionList garbageRegionList;

    // regions for pinned (small-sized) objects.
    // region lists for small-sized pinned objects which are not be moved during concurrent gc, but
    // may be moved during compaction.
    RegionList recentPinnedRegionList;
    RegionList oldPinnedRegionList;

    // region lists for small-sized raw-pointer objects (i.e. future, monitor)
    // which can not be moved ever (even during compaction).
    RegionList rawPointerPinnedRegionList;

    // regions for large-sized objects.
    // large region is recorded here after large object is allocated.
    RegionList oldLargeRegionList;

    // if large region is allocated when gc is not running, it is recorded here.
    RegionList recentLargeRegionList;

    // if large region is allocated during gc trace phase, it is called a trace-region,
    // it is recorded here when it is full.
    RegionCache largeTraceRegions;

    uintptr_t regionInfoStart = 0; // the address of first RegionInfo

    uintptr_t regionHeapStart = 0; // the address of first region to allocate object
    uintptr_t regionHeapEnd = 0;

    // the time when previous region was allocated, which is assigned with returned value by timeutil::NanoSeconds().
    std::atomic<uint64_t> prevRegionAllocTime = { 0 };

    std::atomic<size_t> youngAllocatedBytes = { 0 };
    __attribute__((visibility("hidden")))
    void RecordYoungRegionAccounting(YoungAccountingSource source, RegionInfo* region);
    std::atomic<size_t> youngAccountingOrdinal = { 1 };
    std::atomic<size_t> youngDiagnosticAccountedBytes = { 0 };
    std::atomic<size_t> youngDiagnosticHeapBaseline = { 0 };
    size_t youngObjectBytesBaseline = 0;
    size_t youngRegionCapacityBaseline = 0;
    size_t youngRawPrivateBytesBaseline = 0;
    size_t youngValidationObjectBaseline = 0;
    size_t youngPinnedSlotBytes = 0; // guarded by freePinnedSlotListMutex
    std::atomic<size_t> youngDiagnosticNewRegionEvents = { 0 };
    std::atomic<size_t> youngDiagnosticNewRegionBytes = { 0 };
    std::atomic<size_t> youngDiagnosticReusedGarbageEvents = { 0 };
    std::atomic<size_t> youngDiagnosticReusedGarbageBytes = { 0 };
    std::atomic<size_t> youngDiagnosticReusedFreeEvents = { 0 };
    std::atomic<size_t> youngDiagnosticReusedFreeBytes = { 0 };
    std::atomic<size_t> routeEpochMismatchCount = { 0 };
    // Probe misses on non-ghost regions with current identity (defined negative answers).
    std::atomic<size_t> routeNotGhostProbeCount = { 0 };

    // heap space not allocated yet for even once. this value should not be decreased.
    std::atomic<uintptr_t> inactiveZone = { 0 };
    size_t maxUnitCountPerRegion = MAX_UNIT_COUNT_PER_REGION;   // max units count for threadLocal buffer.
    size_t maxUnitCountPerPinnedRegion = maxUnitCountPerRegion; // max units count for pinned region.
    size_t largeObjectThreshold;
    double fromSpaceGarbageThreshold = 0.5; // 0.5: default garbage ratio.
    double exemptedRegionThreshold;
#if defined(__EULER__)
    double cacheRatio;
#endif
    std::mutex freePinnedSlotListMutex;
    FreePinnedSlotLists freePinnedSlotLists;

    __attribute__((visibility("hidden"))) size_t GetAllocPointerBytes(size_t& regionCapacityBytes) const;
    __attribute__((visibility("hidden"))) size_t GetValidationObjectBytes() const;
    __attribute__((visibility("hidden"))) size_t GetRawPrivateBytes() const;
};
} // namespace MapleRuntime
#endif // MRT_REGION_MANAGER_H
