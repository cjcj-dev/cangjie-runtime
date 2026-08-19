// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGION_MANAGER_H
#define MRT_REGION_MANAGER_H

#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

#include "AllocBuffer.h"
#include "Allocator.h"
#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "RoutePublish.h"
#include "Common/RunType.h"
#include "FreeRegionManager.h"
#include "Heap/Verify/FwdInflight.h"
#include "Heap/GcThreadPool.h"
#include "RegionList.h"
#include "Heap/Verify/EmptyLiveDiag.h"
#include "Heap/Verify/F3Why2Diag.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/HealPairDiag.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/MarkFaceSnap.h"
#include "Heap/Verify/RegionLifeDiag.h"
#include "Heap/Verify/SealCheck.h"
#include "Heap/Verify/PermWhoAdmit.h"
#include "Heap/Verify/PinFireDiag.h"
#include "securec.h"
#include "SlotList.h"
#include "Sync/Sync.h"

namespace MapleRuntime {
class CopyCollector;
class CompactCollector;
class VerifyRegions;
class TagReuseProbe;
class WCollector;

struct YoungCollectionStats {
    size_t candidateRegions = 0;
    size_t candidateBytes = 0;
    size_t reclaimedRegions = 0;
    size_t reclaimedBytes = 0;
};

struct FreePinnedSlotLists {
    static constexpr size_t ATOMIC_OBJECT_SIZE = 16;
    static constexpr size_t SYNC_OBJECT_SIZE = CJFuture::SYNC_OBJECT_SIZE;
    SlotList freeAtomicSlotList;
    SlotList freeSyncSlotList;

private:
    friend class RegionManager;
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

public:
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
    friend class VerifyRegions;
    friend class TagReuseProbe;
    friend struct PinRootTestAccess;

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

    // allowSaferegion=false: no ScopedEnterSaferegion under ROUTING (routefix / REPORT-routespin).
    RegionInfo* AllocateThreadLocalRegion(bool expectPhysicalMem = false, bool youngRegion = true,
                                          bool allowSaferegion = true);

    template<Generation G>
    void ForwardFromRegions(GCThreadPool* threadPool);
    template<Generation G>
    void ForwardFromRegions();
    template<Generation G>
    void ForwardRegion(RegionInfo* region);
    // Before clearing the young flag on a promoted region, record every live
    // old→young out-edge that mutators skipped while the source was still young.
    static size_t RecordPromotedCrossGenEdges(RegionInfo* region);
    static size_t ConsumePromotedCrossGenEdgeCount();
    // Non-young holders (pinned/large at birth, or post-promote old) + IDLE bare store:
    // edges never enter RecordCrossGenEdge. Stamp remset before each minor.
    // See ops/design/G1_WRITE_BARRIER_DESIGN.md (phase≤INIT fast path).
    size_t RecordPinnedCrossGenEdges();
    void StampCensusBoundaries();
    void PromoteAllRegions();
    // Put a region the forward path finished with in place back where a collection-set builder
    // will find it; CompactRegion leaves it on tlRegionList, which no builder walks.
    void RehomeCompactedInPlaceRegion(RegionInfo* region);
    void CompactRegion(RegionInfo* region);
    void CompactRegion(RegionInfo* region, RegionInfo* toRegion1);

    void ExemptFromRegion(RegionInfo* region);
    // zRelocate.cpp:1346-1352 flip_survived: keep the page, reset age, leave young.
    // Must not remain LONE_FROM / FROM after TakeHead — barriers treat those as from-space.
    void EnlistStayYoungSurvivor(RegionInfo* region);
    static void BumpYoungSurvivorAge(RegionInfo* region);
    static void FinishStayYoungInPlace(RegionInfo* region);

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
    // allowSaferegion=false: best-effort, never enter saferegion (ROUTING critical section).
    RegionInfo* TakeRegion(size_t num, RegionInfo::UnitRole, bool expectPhysicalMem = false,
                           bool allowSaferegion = true);

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
            RegionLifeDiag::SetNextFreePath(RegionLifeDiag::PATH_UNUSED_PINNED);
            (void)CollectRegion<Generation::Old>(region);
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
        // twoflags: POST_TRACE+ only (independent of isTraceRegion).
        if (phase == GC_PHASE_POST_TRACE || phase == GC_PHASE_PREFORWARD ||
            phase == GC_PHASE_FORWARD) {
            region->SetNotRelocatableThisCycle(1);
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
                garbageRegionList.PrependRegion(region, RegionInfo::RegionType::GARBAGE_REGION);
            } else {
                ReclaimRegion(region);
            }
            region = fromRegionList.TakeHeadRegion();
        }
#else
        garbageRegionList.MergeRegionList(fromRegionList, RegionInfo::RegionType::GARBAGE_REGION);
#endif
    }

    size_t GetThreadLocalRegionSize() const
    {
        return maxUnitCountPerRegion * RegionInfo::UNIT_SIZE;
    }

    size_t GetYoungAllocatedSize() const
    {
        return RegionInfo::GetYoungRegionCount() * GetThreadLocalRegionSize();
    }

    static bool IsKnownEmptyForView(RegionInfo* region, MarkView<Generation::Young> view)
    {
        return region->IsKnownYoungEmpty(view);
    }

    static bool IsKnownEmptyForView(RegionInfo* region, MarkView<Generation::Old> view)
    {
        return region->IsKnownEmpty(view);
    }

    template<Generation G>
    size_t CollectRegion(RegionInfo* region)
    {
        MarkView<G> view = region->GetRouteMarkView<G>();
        const bool knownEmpty = IsKnownEmptyForView(region, view);
        // whoempty: record the *decision*, with the live-byte count it was made on, into the same
        // ring ClearUnits writes to.  ClearUnits passes liveBefore as a literal 0
        // (RegionInfo.h:1429 `TraceClear::NoteRange(unitAddress, size, "clear_units", nullptr, 0)`),
        // so a `liveBefore=0` in a clear entry says nothing about the region -- it is a constant.
        // This entry carries the real number, taken at the one edge where a region dies.
        TraceClear::NoteRange(region->GetRegionStart(), region->GetRegionSize(),
                              knownEmpty ? "coll_empty" : "coll_live", region, region->GetLiveByteCount());
        DLOG(REGION, "collect region %p@[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
             region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType());
        // f3why2/livesame: always-on enter + knownEmpty_marked class.
        F3Why2Diag::NoteCollectEnter(region);
        // emptylive: epoch-split size-walk on knownEmpty (gate MRT_GCV2_EMPTYLIVE).
        EmptyLiveDiag::NoteCollectEnter(region);
        GarbRegionDiag::NoteCollectEnter(region);
        // regionlife: free-edge accounting (default off; also via WHOZERO).
        {
            uint16_t freePath = RegionLifeDiag::TakeNextFreePath();
            RegionLifeDiag::NoteFree(region, freePath);
            // holdercapture: last instant the mark face still exists. InitFreeUnits is
            // below; after it the bitmap/LiveInfo answer for the wrong reason.
            MarkFaceSnap::NoteRegionFree(region, freePath);
        }
        HealPairDiag::NoteCollect(region->GetRegionStart(), region->GetRegionEnd(),
                                 region->GetLiveByteCount(),
                                 static_cast<uint32_t>(region->GetRegionType()),
                                 knownEmpty ? 1U : 0U);
        // Probe: knownEmpty region still holds valid object headers (gcreclaim / B2 H1).
        {
            // gcreclaim was written for exactly the question now in hand -- does a region we are
            // about to declare empty still contain valid object headers, and are any of them
            // marked -- and then pinned off.  This is the fourth instrument this session that was
            // already built and switched off (TraceClear, PermWhoAdmit, ToverFailDiag were the
            // others), so the campaign's bottleneck is not writing probes.
            //
            // What it decides: 45 of 45 unusable zero-header targets sit in regions this call
            // classified knownEmpty with a real GetLiveByteCount() of 0.  validObjs > 0 there means
            // the region was not empty at all, and markedObjs separates the two causes -- objects
            // present but unmarked (the mark closure missed them) from objects marked while the
            // emptiness test still said empty (the test and the bitmap disagree).
            static constexpr bool probe = true;
            if (probe && region != nullptr && knownEmpty) {
                size_t start = region->GetRegionStart();
                size_t alloc = region->GetRegionAllocPtr();
                size_t end = region->GetRegionEnd();
                size_t residual = alloc > start ? (alloc - start) : 0;
                size_t validObjs = 0;
                size_t markedObjs = 0;
                if (residual > 0 && !region->IsLargeRegion()) {
                    uintptr_t pos = start;
                    while (pos < alloc) {
                        BaseObject* o = from_region_addr(pos);
                        if (!o->IsValidObject()) {
                            break;
                        }
                        size_t sz = o->GetSize();
                        if (sz == 0) {
                            break;
                        }
                        ++validObjs;
                        if (region->IsMarkedObject(view, o)) {
                            ++markedObjs;
                        }
                        pos += sz;
                    }
                }
                // A from-region that has finished evacuating legitimately looks like this: its
                // from-copies are still readable and none of them are marked in the new view,
                // because they moved.  35,498 of these were logged in one N=10 run, all with
                // type=4 (LONE_FROM) route=5 (FORWARDED) markedObjs=0 -- correct behaviour, not a
                // defect, and reporting it as one would have been a wrong conclusion drawn from a
                // big number.  Narrow to the case that cannot be explained that way: a region
                // holding valid objects that is not from-space at all.
                const bool fromSpace = region->IsFromRegion() || region->IsLoneFromRegion() ||
                    region->IsUnmovableFromRegion() || region->IsGhostFromRegion();
                if (validObjs > 0 && !fromSpace) {
                    // LOG rather than VLOG(REPORT): REPORT is gated on MRT_REPORT and lands in a
                    // separate report.log.<pid> sink, which cost a turn earlier this session when a
                    // probe looked silent because its output had gone somewhere else.
                    LOG(RTLOG_ERROR,
                         "[GCRECLAIM][empty-notfrom] region=%p start=%#zx alloc=%#zx end=%#zx type=%u young=%u "
                         "live=%zu residual=%zu validObjs=%zu markedObjs=%zu route=%u BYPASS=1",
                         region, start, alloc, end, region->GetRegionType(),
                         static_cast<unsigned>(region->IsYoungRegion()), region->GetLiveByteCount(), residual,
                         validObjs, markedObjs, static_cast<unsigned>(region->GetRouteState()));
                }
            }
        }

        // STEER3 CALLSITE_AUDIT: scrub HERE (once), not at ReclaimRegion.
        // Linux TakeRegion often reuses garbage via ClearUnits WITHOUT ReclaimRegion
        // (RegionManager.cpp TakeRegion same-size head path). Scrub-only-at-Reclaim
        // therefore never ran on the hot path. Collect is the unique "region dies" edge.
        ScrubRememberedSetForRegion(region);

        region->LockWriteRegion();
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
        // Pin needs a plain load-good address. High colour bits ⇒ missing barrier
        // at the call site (would OOB in GetUnitIdxAt; fail closed here).
        MAddress rawAddr = reinterpret_cast<MAddress>(obj);
        CHECK(rawAddr == 0 || (rawAddr >> 48) == 0);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(rawAddr);
        region->IncRawPointerObjectCount();
        PinFireDiag::NoteAddRawPointer();
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
        MAddress rawAddr = reinterpret_cast<MAddress>(obj);
        CHECK(rawAddr == 0 || (rawAddr >> 48) == 0);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(rawAddr);
        region->DecRawPointerObjectCount();
    }

    void ReclaimRegion(RegionInfo* region);
    // Like ReclaimRegion but units enter mark-quarantine tree, not dirty tree.
    void ReclaimRegionToMarkQuarantine(RegionInfo* region);
    size_t ReleaseRegion(RegionInfo* region);
    // Clear the two exact bitmap slices owned by [regionStart, regionEnd).
    // Called on both CollectRegion and the direct large-region release path.
    static void ScrubRememberedSetForRegion(RegionInfo* region);
    // Emit + reset process-local scrub cost counters (STEER3).
    static void DumpScrubCostAndReset(const char* point);

    void ReclaimGarbageRegions()
    {
        RegionInfo* garbage = TakeReclaimableGarbageRegion();
        while (garbage != nullptr) {
            ReclaimRegion(garbage);
            garbage = TakeReclaimableGarbageRegion();
        }
        // STEER3: scrub runs here (async reclaim), not inside young STW.
        DumpScrubCostAndReset("post-reclaim-batch");
    }

    size_t CollectLargeGarbage();

    size_t CollectPinnedGarbage();
    size_t CollectFreePinnedSlots(RegionInfo* region);

    // targetSize: size of memory which we do not release and keep it as cache for future allocation.
    size_t ReleaseGarbageRegions(size_t targetSize) { return freeRegionManager.ReleaseGarbageRegions(targetSize); }
    size_t UncommitIdleUnits(size_t maxBytes, uint64_t idleBeforeNs)
    {
        return freeRegionManager.UncommitIdleUnits(maxBytes, idleBeforeNs);
    }

    // Ignore dynamic pinned regions and from regions whose garbage objects are quite few, return the garbage size that
    // can be reclaimed.
    size_t ExemptFromRegions();
    void ReassembleFromSpace();

    void ForEachObjUnsafe(const std::function<void(BaseObject*)>& visitor,
                          bool skipKnownEmptyRegions = false) const;
    void ForEachObjSafe(const std::function<void(BaseObject*)>& visitor) const;

    size_t GetUsedRegionSize() const { return GetUsedUnitCount() * RegionInfo::UNIT_SIZE; }

    size_t GetRecentAllocatedSize() const
    {
        return recentFullRegionList.GetAllocatedSize() + recentLargeRegionList.GetAllocatedSize() +
            recentPinnedRegionList.GetAllocatedSize();
    }

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
    size_t GetReleasedUnitCount() const { return freeRegionManager.GetReleasedUnitCount(); }
    size_t GetGarbageUnitCount() const { return garbageRegionList.GetUnitCount(); }

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
        // twoflags: Assemble just filtered previous-cycle stamps; clear so this TRACE
        // re-stamps only regions that allocate after this mark start.
        ClearNotRelocatableThisCycleFlags();
    }

    // twoflags: walk live region lists and clear notRelocatableThisCycle.
    void ClearNotRelocatableThisCycleFlags();

    // routedest: walk the same lists and drop routeDestHold for one route generation.
    void ClearRouteDestHoldFlags();

    bool RouteOrCompactRegionImpl(RegionInfo* region);

    static bool RouteIsPublished(BaseObject* fromObj, RegionInfo* fromRegionInfo)
    {
        if (fromObj != nullptr && fromObj->IsForwarded()) {
            return true;
        }
        if (fromRegionInfo == nullptr) {
            return false;
        }
        RegionInfo::RouteState rs = fromRegionInfo->GetRouteState();
        return rs == RegionInfo::RouteState::FORWARDED || rs == RegionInfo::RouteState::COMPACTED;
    }

    RoutePlan PlanRoute(BaseObject* fromObj, RegionInfo* fromRegionInfo, CopierRouteToken)
    {
        return RoutePlan{ ComputeRoute(fromObj, fromRegionInfo, FwdInflight::Site::ROUTE_WITH_REGION) };
    }

    RoutePlan PlanRoute(BaseObject* fromObj, CopierRouteToken)
    {
        return PlanRouteLookup(fromObj);
    }

    RoutePlan PlanRoute(BaseObject* fromObj, RegionInfo* fromRegionInfo, StwRouteToken)
    {
        return RoutePlan{ ComputeRoute(fromObj, fromRegionInfo, FwdInflight::Site::ROUTE_WITH_REGION) };
    }

    RoutePlan PlanRoute(BaseObject* fromObj, StwRouteToken)
    {
        return PlanRouteLookup(fromObj);
    }

    PublishedRoute FindPublishedRoute(BaseObject* fromObj, RegionInfo* fromRegionInfo)
    {
        BaseObject* to = ComputeRoute(fromObj, fromRegionInfo, FwdInflight::Site::ROUTE_WITH_REGION);
        if (to == nullptr || !RouteIsPublished(fromObj, fromRegionInfo)) {
            return PublishedRoute{ nullptr };
        }
        return PublishedRoute{ to };
    }

    PublishedRoute FindPublishedRoute(BaseObject* fromObj)
    {
        RegionInfo* fromRegionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(fromObj));
        if (fromRegionInfo == nullptr) {
            return PublishedRoute{ nullptr };
        }
        BaseObject* to = ComputeRoute(fromObj, fromRegionInfo, FwdInflight::Site::ROUTE_LOOKUP);
        if (to == nullptr || !RouteIsPublished(fromObj, fromRegionInfo)) {
            return PublishedRoute{ nullptr };
        }
        return PublishedRoute{ to };
    }

    bool RouteRegion(RegionInfo* fromRegionInfo)
    {
        // fysfixb / 352ed4e8: non-ghost is a defined negative answer, not invariant break.
        // Producers that clear ghost: DispelGhostFromRegion (PrepareFromRegionList),
        // ClearGhostRegionBit (raw-pin POST_TRACE), TakeRegion reuse. Consumers
        // (ForwardRegion / TryForwardObject) may still hold a region* after the
        // carrier retired or after liveBytes==0 skipped install (pre-a2e7ee37).
        // Soft-null matches RouteObject's GetGhostFromRegionAt==null path.
        if (UNLIKELY(!fromRegionInfo->IsGhostFromRegion())) {
            VLOG(REPORT,
                 "[GCV2][ghost-softnull] region=%p start=%#zx live=%zu route=%u young=%u "
                 "auth=%u — RouteRegion soft-miss (ghost cleared or never installed)",
                 fromRegionInfo, fromRegionInfo->GetRegionStart(), fromRegionInfo->GetLiveByteCount(),
                 static_cast<unsigned>(fromRegionInfo->GetRouteState()),
                 static_cast<unsigned>(fromRegionInfo->IsYoungRegion()),
                 static_cast<unsigned>(fromRegionInfo->IsLiveCountAuthoritative()));
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
                // sealcheck E_seal (per-region): face freezes before geometry read.
                // RouteOrCompactRegionImpl reads GetLiveByteCount / VisitLiveObjects next.
                SealCheck::NoteSeal(fromRegionInfo);
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

    template<Generation G>
    void PrepareFromRegionList()
    {
        size_t retainedRegions = 0;
        size_t retainedBytes = 0;
        size_t markQuarantinedRegions = 0;
        size_t markQuarantinedBytes = 0;
        ghostFromRegionList.VisitAllGhostRegions(
            [this, &retainedRegions, &retainedBytes, &markQuarantinedRegions,
             &markQuarantinedBytes](RegionInfo* region) {
            DLOG(REGION, "visit ghost from region %p@[%#zx, %#zx)", region, region->GetRegionStart(),
                 region->GetRegionEnd());
            // Count ghost garbage retention before dispel (historical GhostRetention metric).
            if (region->IsGhostFromRegion() && region->IsGarbageRegion()) {
                ++retainedRegions;
                retainedBytes += region->GetGhostRegionSize();
            }
            region->DispelGhostFromRegion();
            if (TryTakeGarbageRegionAfterDispel(region)) {
                // mark-epoch gate (OPTION_2): do not publish to dirty tree until major mark ends.
                // Mutator TakeRegion would ClearUnits payload while concurrent mark may still
                // follow plain SATB edges into this range (REPORT-tracewin 16/16).
                size_t bytes = region->GetRegionSize();
                ReclaimRegionToMarkQuarantine(region);
                ++markQuarantinedRegions;
                markQuarantinedBytes += bytes;
            }
        });
        // A7 cost baseline: deferring the dispel by one cycle would hold roughly this much extra,
        // so the number has to be on the table before the change, not after.  LOG rather than
        // VLOG(REPORT) for the same reason as the gcreclaim probe -- REPORT lands in a separate
        // sink and a probe that looks silent has cost this campaign a turn before.
        LOG(RTLOG_ERROR, "[GhostRetention] retained_regions=%zu retained_bytes=%zu", retainedRegions, retainedBytes);
        VLOG(REPORT, "[MarkQuarantine] installed_regions=%zu installed_bytes=%zu held_units=%u",
             markQuarantinedRegions, markQuarantinedBytes, freeRegionManager.GetMarkQuarantineUnitCount());

        // routedest: the walk above retired the whole outgoing route generation —
        // DispelGhostFromRegion clears the ghost bit and sets routeState NORMAL in one
        // statement, after which both product readers fail (RouteRegion soft-nulls on
        // !IsGhostFromRegion, and the `|| IsCompacted()` bypass is false too). Drop the
        // destination holds those routes were keeping alive, before the next generation's
        // destinations are enrolled by the PrepareForwardableRegion walk below.
        //
        // Dropping a hold does not by itself make a region reclaimable: it must still be
        // picked up by a later Assemble / PrepareYoungGarbageCandidates, evacuated and
        // collected. That is why this ordering does not have to be defended against the
        // reclaim schedules that are not phase-driven — the mutator garbage fast path and
        // the finalizer both reach a live region only through TakeReclaimableGarbageRegion,
        // and a held region never reaches garbageRegionList in the first place.
        ClearRouteDestHoldFlags();

        fromRegionList.VisitAllRegions([](RegionInfo* region) {
            DLOG(REGION, "visit from region %p@[%#zx+%zu, %#zx)", region, region->GetRegionStart(),
                 region->GetLiveByteCount(), region->GetRegionEnd());
            MarkView<G> view = region->GetMarkView<G>();
            region->PrepareForwardableRegion(view);
        });

        fromRegionList.CopyListTo(ghostFromRegionList);
    }

    // Release point for OPTION_2 mark-epoch gate: major PostTrace after PrepareForwardTable.
    // Concurrent mark (TRACE+CLEAR_SATB) has finished; plain strong refs into quarantined
    // ranges are no longer traced. Safe to publish units to dirty tree for ClearUnits reuse.
    // Note: this major's just-installed quarantine (from PrepareForwardTable above) is also
    // released here — mark is already done, so no TRACE can race those units. Units held from
    // prior minor PrepareForwardTable are the ones that covered the TRACE window.
    void ReleaseMarkQuarantine()
    {
        size_t heldBefore = freeRegionManager.GetMarkQuarantineUnitCount();
        size_t units = freeRegionManager.ReleaseMarkQuarantineToDirty();
        size_t bytes = units * RegionInfo::UNIT_SIZE;
        VLOG(REPORT,
             "[MarkQuarantine] released_units=%zu released_bytes=%zu held_before=%zu held_after=%u",
             units, bytes, heldBefore, freeRegionManager.GetMarkQuarantineUnitCount());
        // Cost metric same family as ghostorder: peak retained bytes under mark-epoch gate.
        VLOG(REPORT, "[GhostRetention] retained_regions=%zu retained_bytes=%zu", heldBefore,
             heldBefore * RegionInfo::UNIT_SIZE);
    }

    void ClearAllLiveInfo()
    {
        ClearLiveInfo<Generation::Old>(tlRegionList);
        ClearLiveInfo<Generation::Old>(recentFullRegionList);
        ClearLiveInfo<Generation::Old>(fullTraceRegions);
        ClearLiveInfo<Generation::Old>(unmovableFromRegionList);
        ClearLiveInfo<Generation::Old>(recentPinnedRegionList);
        ClearLiveInfo<Generation::Old>(oldPinnedRegionList);
        ClearLiveInfo<Generation::Old>(rawPointerPinnedRegionList);
        ClearLiveInfo<Generation::Old>(oldLargeRegionList);
        ClearLiveInfo<Generation::Old>(recentLargeRegionList);
        ClearLiveInfo<Generation::Old>(largeTraceRegions);
    }

    // Probe-only: visit every region on managed lists with its list name (tag-reuse scan).
    template <typename F>
    void VisitAllManagedRegionsForProbe(F&& visitor)
    {
        auto walk = [&visitor](const char* name, RegionList& list) {
            list.VisitAllRegions([&visitor, name](RegionInfo* region) { visitor(region, name); });
        };
        walk("tlRegionList", tlRegionList);
        walk("recentFullRegionList", recentFullRegionList);
        walk("fromRegionList", fromRegionList);
        ghostFromRegionList.VisitAllGhostRegions(
            [&visitor](RegionInfo* region) { visitor(region, "ghostFromRegionList"); });
        walk("unmovableFromRegionList", unmovableFromRegionList);
        walk("garbageRegionList", garbageRegionList);
        walk("recentPinnedRegionList", recentPinnedRegionList);
        walk("oldPinnedRegionList", oldPinnedRegionList);
        walk("rawPointerPinnedRegionList", rawPointerPinnedRegionList);
        walk("oldLargeRegionList", oldLargeRegionList);
        walk("recentLargeRegionList", recentLargeRegionList);
        walk("fullTraceRegions", fullTraceRegions);
        walk("largeTraceRegions", largeTraceRegions);
    }

    // Production: before ReleaseMemory(previous tag), null liveInfo/liveInfo0/retained that
    // still point into the dying range. Same region set as the probe walk (incl. garbage).
    // Phase: STW inside PrepareForwardTable → ClearPreviousForwardData (minor ×2, major ×1).
    void NullLiveInfoFieldsInRange(uintptr_t rangeStart, size_t rangeSize)
    {
        auto nullOne = [rangeStart, rangeSize](RegionInfo* region) {
            if (region != nullptr) {
                region->NullLiveInfoFieldsInRange(rangeStart, rangeSize);
            }
        };
        auto walk = [&nullOne](RegionList& list) {
            list.VisitAllRegions([&nullOne](RegionInfo* region) { nullOne(region); });
        };
        walk(tlRegionList);
        walk(recentFullRegionList);
        walk(fromRegionList);
        ghostFromRegionList.VisitAllGhostRegions(nullOne);
        walk(unmovableFromRegionList);
        walk(garbageRegionList);
        walk(recentPinnedRegionList);
        walk(oldPinnedRegionList);
        walk(rawPointerPinnedRegionList);
        walk(oldLargeRegionList);
        walk(recentLargeRegionList);
        walk(fullTraceRegions);
        walk(largeTraceRegions);
    }

private:
    RoutePlan PlanRouteLookup(BaseObject* fromObj)
    {
        RegionInfo* fromRegionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(fromObj));
        if (fromRegionInfo == nullptr) {
            return RoutePlan{ nullptr };
        }
        return RoutePlan{ ComputeRoute(fromObj, fromRegionInfo, FwdInflight::Site::ROUTE_LOOKUP) };
    }

    BaseObject* ComputeRoute(BaseObject* fromObj, RegionInfo* fromRegionInfo, FwdInflight::Site site)
    {
        RegionInfo::RetainScope retain(fromRegionInfo);
        if (!retain.ok()) {
            return nullptr;
        }
        FwdInflight::Scope inflight(fromRegionInfo, site);
        if (RouteRegion(fromRegionInfo) || fromRegionInfo->IsCompacted()) {
            OptionalRouteTicket ticket = fromRegionInfo->AdmitForRoute(fromObj);
            if (!ticket) {
                return nullptr;
            }
            BaseObject* to = fromRegionInfo->GetRoute(ticket.value());
            PermWhoAdmit::NoteRoute(fromRegionInfo, fromObj, to);
            return to;
        }
        return nullptr;
    }

    RegionInfo* TakeReclaimableGarbageRegion(size_t* gatedBytes = nullptr)
    {
        std::lock_guard<std::mutex> lock(garbageRegionList.GetListMutex());
        RegionInfo* candidate = nullptr;
        size_t bytes = 0;
        for (RegionInfo* region = garbageRegionList.GetHeadRegion(); region != nullptr;
             region = region->GetNextRegion()) {
            if (region->IsGhostFromRegion()) {
                bytes += region->GetGhostRegionSize();
            } else if (candidate == nullptr &&
                       !RouteDestHold::HoldsBack(region, RouteDestHold::Site::TAKE_GARBAGE)) {
                // routedest: defence in depth. A held region should never have reached
                // garbageRegionList — the two Assemble gates and the two young gates refuse
                // it first — so a non-zero count at this site means one of those was
                // bypassed. This one chokepoint covers both reclaim schedules that are not
                // phase-driven at once: the mutator garbage fast path (TakeRegion) and the
                // finalizer, whose ReclaimGarbageRegions loops on this function.
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

    bool TryTakeGarbageRegionAfterDispel(RegionInfo* target)
    {
        std::lock_guard<std::mutex> lock(garbageRegionList.GetListMutex());
        for (RegionInfo* region = garbageRegionList.GetHeadRegion(); region != nullptr;
             region = region->GetNextRegion()) {
            if (region == target) {
                CHECK(region->IsGarbageRegion());
                CHECK(!region->IsGhostFromRegion());
                // routedest: refuse a held region here too, so it is neither quarantined nor
                // reclaimed. Same defence-in-depth role as TakeReclaimableGarbageRegion.
                if (RouteDestHold::HoldsBack(region, RouteDestHold::Site::TAKE_AFTER_DISPEL)) {
                    return false;
                }
                RemoveRegionLocked(&garbageRegionList, region);
                return true;
            }
        }
        return false;
    }

    size_t GetGatedGarbageBytes()
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

    template<Generation G>
    void ClearLiveInfo(RegionList& list)
    {
        RegionList tmp("temp region list");
        list.CopyListTo(tmp);
        tmp.VisitAllRegions([](RegionInfo* region) {
            MarkView<G> view = region->GetMarkView<G>();
            region->ClearLiveInfo(view);
        });
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
};
} // namespace MapleRuntime
#endif // MRT_REGION_MANAGER_H
