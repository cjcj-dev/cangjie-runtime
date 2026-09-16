// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGIONMANAGER_INLINE_H
#define MRT_REGIONMANAGER_INLINE_H

#include "Heap/z/zPageAllocator.hpp"

namespace MapleRuntime {

inline __attribute__((visibility("hidden"))) size_t RegionManager::GetHeapMemorySize(size_t heapSize)
    {
        size_t unitNum = GetHeapUnitCount(heapSize);
        size_t metadataSize = GetMetadataSize(unitNum);
        size_t roundedHeapSize = 0;
        CHECK_DETAIL(CheckedRoundUpSize(heapSize, RegionInfo::UNIT_SIZE, roundedHeapSize),
                     "heap size round-up overflows: heapSize=%zu unitSize=%zu", heapSize,
                     RegionInfo::UNIT_SIZE);
        size_t totalSize = 0;
        CHECK_DETAIL(CheckedAddSize(metadataSize, roundedHeapSize, totalSize),
                     "heap reservation geometry overflows: metadataSize=%zu heapSize=%zu",
                     metadataSize, roundedHeapSize);
        return totalSize;
    }

inline __attribute__((visibility("hidden"))) size_t RegionManager::GetHeapUnitCount(size_t heapSize)
    {
        size_t roundedHeapSize = 0;
        CHECK_DETAIL(CheckedRoundUpSize(heapSize, RegionInfo::UNIT_SIZE, roundedHeapSize),
                     "heap unit geometry overflows: heapSize=%zu unitSize=%zu", heapSize,
                     RegionInfo::UNIT_SIZE);
        heapSize = roundedHeapSize;
        size_t unitNum = heapSize / RegionInfo::UNIT_SIZE;
        return unitNum;
    }

inline __attribute__((visibility("hidden"))) size_t RegionManager::GetMetadataSize(size_t num)
    {
        (void)num;
        return MapleRuntime::MRT_PAGE_SIZE;
    }

    template<Generation G>
inline size_t RegionManager::CollectRegion(RegionInfo* region)
    {
        DLOG(REGION, "collect region %p@[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
             region->is_marked() ? region->live_bytes() : 0, region->GetRegionEnd(), region->GetRegionType());
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
            return region->GetRegionSize() - (region->is_marked() ? region->live_bytes() : 0);
        }
    }

inline void RegionManager::AddRawPointerObject(BaseObject* obj)
    {
        // Pin needs a plain load-good address. High colour bits ⇒ missing barrier
        // at the call site (would OOB in GetUnitIdxAt; fail closed here).
        MAddress rawAddr = reinterpret_cast<MAddress>(obj);
        CHECK(rawAddr == 0 || (rawAddr >> 48) == 0);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(rawAddr);
        region->IncRawPointerObjectCount();

        // CSet empty-free (ExemptFromRegions) TryDeletes FROM under the same
        // list lock (zGeneration.cpp:211-221 register_empty_page). Inc first so
        // a GC that already claimed GARBAGE still sees rawPtrCnt>0. Retry the
        // unlisted window between TryDelete and Prepend.
        for (;;) {
            if (fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                               RegionInfo::RegionType::RAW_POINTER_PINNED_REGION) ||
                garbageRegionList.TryDeleteRegion(region, RegionInfo::RegionType::GARBAGE_REGION,
                                                  RegionInfo::RegionType::RAW_POINTER_PINNED_REGION)) {
                GCPhase phase = Heap::GetHeap().GetGCPhase(region->IsYoungRegion() ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD);
                CHECK(phase != GCPhase::GC_PHASE_FORWARD && phase != GCPhase::GC_PHASE_PREFORWARD);
                if (phase == GCPhase::GC_PHASE_POST_TRACE) {
                    region->ClearGhostRegionBit();
                }
                rawPointerPinnedRegionList.PrependRegion(region, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
                break;
            }
            const RegionInfo::RegionType t = region->GetRegionType();
            if (t != RegionInfo::RegionType::FROM_REGION && t != RegionInfo::RegionType::GARBAGE_REGION) {
                CHECK(t != RegionInfo::RegionType::LONE_FROM_REGION);
                break;
            }
            std::this_thread::yield();
        }
    }

inline void RegionManager::RemoveRawPointerObject(BaseObject* obj)
    {
        MAddress rawAddr = reinterpret_cast<MAddress>(obj);
        CHECK(rawAddr == 0 || (rawAddr >> 48) == 0);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(rawAddr);
        region->DecRawPointerObjectCount();
    }

inline void RegionManager::ReclaimGarbageRegions()
    {
        RegionInfo* garbage = TakeReclaimableGarbageRegion();
        while (garbage != nullptr) {
            ReclaimRegion(garbage);
            garbage = TakeReclaimableGarbageRegion();
        }
        // STEER3: scrub runs here (async reclaim), not inside young STW.
        SatisfyStalledAllocations();
    }

inline size_t RegionManager::GetRecentAllocatedSize() const
    {
        return recentFullRegionList.GetAllocatedSize() + recentLargeRegionList.GetAllocatedSize() +
            recentPinnedRegionList.GetAllocatedSize();
    }

inline size_t RegionManager::GetSurvivedSize() const
    {
        return fromRegionList.GetAllocatedSize() + oldPinnedRegionList.GetAllocatedSize() +
            oldLargeRegionList.GetAllocatedSize();
    }

inline size_t RegionManager::GetUsedUnitCount() const
    {
        return
            fromRegionList.GetUnitCount() + unmovableFromRegionList.GetUnitCount() +
            recentFullRegionList.GetUnitCount() + oldLargeRegionList.GetUnitCount() +
            recentLargeRegionList.GetUnitCount() + oldPinnedRegionList.GetUnitCount() +
            recentPinnedRegionList.GetUnitCount() + rawPointerPinnedRegionList.GetUnitCount() +
            largeTraceRegions.GetUnitCount() + fullTraceRegions.GetUnitCount() +
            tlRegionList.GetUnitCount();
    }

inline size_t RegionManager::GetAllocatedSize() const
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

inline void RegionManager::MergeRawPointerRegions(RegionList& smallSizeRegionList, RegionList& largeSizeRegionList)
    {
        const size_t smallRegions = smallSizeRegionList.GetRegionCount();
        const size_t smallUnits = smallSizeRegionList.GetUnitCount();
        recentFullRegionList.MergeRegionList(smallSizeRegionList, RegionInfo::RegionType::RECENT_FULL_REGION);
        RecentFullAccounting::Enqueue(smallRegions, smallUnits);
        recentLargeRegionList.MergeRegionList(largeSizeRegionList, RegionInfo::RegionType::RECENT_LARGE_REGION);
    }

inline void RegionManager::HandleTraceRegions()
    {
        fullTraceRegions.DeactivateRegionCache();
        const size_t traceRegions = fullTraceRegions.GetRegionCount();
        const size_t traceUnits = fullTraceRegions.GetUnitCount();
        recentFullRegionList.MergeRegionList(fullTraceRegions, RegionInfo::RegionType::RECENT_FULL_REGION);
        RecentFullAccounting::Enqueue(traceRegions, traceUnits);

        largeTraceRegions.DeactivateRegionCache();
        recentLargeRegionList.MergeRegionList(largeTraceRegions, RegionInfo::RegionType::RECENT_LARGE_REGION);

        tlRegionList.ClearTraceRegionFlag();
        recentPinnedRegionList.ClearTraceRegionFlag();
        oldPinnedRegionList.ClearTraceRegionFlag();
    }

inline void RegionManager::PrepareTrace()
    {
        fullTraceRegions.ActivateRegionCache();
        largeTraceRegions.ActivateRegionCache();
        // twoflags: Assemble just filtered previous-cycle stamps; clear so this TRACE
        // re-stamps only regions that allocate after this mark start.
        ClearNotRelocatableThisCycleFlags();
    }

inline void RegionManager::ReleaseMarkQuarantine()
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
        SatisfyStalledAllocations();
    }


    template <typename F>
inline void RegionManager::VisitAllManagedRegionsForProbe(F&& visitor)
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

inline RegionInfo* RegionManager::TakeReclaimableGarbageRegion(size_t* gatedBytes)
    {
        std::lock_guard<std::mutex> lock(garbageRegionList.GetListMutex());
        RegionInfo* candidate = nullptr;
        size_t bytes = 0;
        for (RegionInfo* region = garbageRegionList.GetHeadRegion(); region != nullptr;
             region = region->GetNextRegion()) {
            if (region->IsGhostFromRegion()) {
                bytes += region->GetGhostRegionSize();
            } else if (candidate == nullptr && region->GetRawPointerObjectCount() == 0) {
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

inline bool RegionManager::TryTakeGarbageRegionAfterDispel(RegionInfo* target)
    {
        std::lock_guard<std::mutex> lock(garbageRegionList.GetListMutex());
        for (RegionInfo* region = garbageRegionList.GetHeadRegion(); region != nullptr;
             region = region->GetNextRegion()) {
            if (region == target) {
                CHECK_DETAIL(region->IsGarbageRegion(),
                             "TryTakeGarbageRegionAfterDispel region=%p type=%u ghost=%u "
                             "(garbage list still names a non-GARBAGE region)",
                             region, static_cast<unsigned>(region->GetRegionType()),
                             static_cast<unsigned>(region->IsGhostFromRegion()));
                CHECK(!region->IsGhostFromRegion());
                // routedest: refuse a held region here too, so it is neither quarantined nor
                // reclaimed. Same defence-in-depth role as TakeReclaimableGarbageRegion.
                if (region->GetRawPointerObjectCount() > 0) {
                    return false;
                }
                RemoveRegionLocked(&garbageRegionList, region);
                return true;
            }
        }
        return false;
    }

inline size_t RegionManager::GetGatedGarbageBytes()
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

inline void RegionManager::LockRegionListInSaferegion(std::mutex& listMutex)
    {
        while (!listMutex.try_lock()) {
            ScopedEnterSaferegion enterSaferegion(true);
        }
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
template<Generation G>
inline void ExecuteForwardTask(RegionManager& regionManager, RegionList& fromRegionList)
{
    while (true) {
        // zRelocate.cpp:1193-1203: serve a mutator's requested receipt
        // before advancing the ordinary relocation iterator.
        RelocationRequestQueue::Selection selected =
            regionManager.GetRelocationRequestQueue().SelectBeforeOrdinary([&fromRegionList]() -> void* {
                return fromRegionList.TakeHeadRegion(RegionInfo::RegionType::LONE_FROM_REGION);
            });
        if (!selected) {
            selected = regionManager.GetRelocationRequestQueue().SynchronizePoll();
            if (selected.workersDone) {
                break;
            }
            if (!selected) {
                continue;
            }
        }
        if (!selected.is_request()) {
            RegionInfo* region = static_cast<RegionInfo*>(selected.ordinary);
            regionManager.ForwardClaimedPage<G>(region, ForwardingTable::RetainPageOwner(region));
            continue;
        }

        RegionInfo* region = static_cast<RegionInfo*>(selected.request->owner());
        // If an ordinary iterator already removed the page, its worker will
        // lose the forwarding claim. This claimant still owns the page task.
        (void)fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                             RegionInfo::RegionType::LONE_FROM_REGION);
        regionManager.ForwardClaimedPage<G>(region,
            ForwardingTable::RetainPageOwner(region), true);
    }
}

} // namespace detail

// The relocation worker task submitted by DrainForwardFromRegions. Test builds
// export Work so the unit runner binds the product SO; default builds retain
// the implicit inline virtual with no MRT_EXPORT and no dynamic export.
template<Generation G>
class ForwardTask : public ZTask {
public:
    ForwardTask(RegionManager& manager, RegionList& fromSpace)
        : ZTask("ZRelocateTask"), regionManager(manager), fromRegionList(fromSpace) {}

    ~ForwardTask() override = default;
#if defined(MRT_TESTABLE_INTERNALS)
    MRT_EXPORT void work() override;
#else
    __attribute__((visibility("hidden"))) void work() override
    {
        detail::ExecuteForwardTask<G>(regionManager, fromRegionList);
    }
#endif

private:
    RegionManager& regionManager;
    RegionList& fromRegionList;
};












} // namespace MapleRuntime
#endif
