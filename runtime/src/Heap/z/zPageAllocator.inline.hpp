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
        size_t metadataSize = 0;
        CHECK_DETAIL(CheckedMulSize(num, sizeof(RegionInfo), metadataSize),
                     "region metadata geometry overflows: units=%zu regionInfoSize=%zu", num,
                     sizeof(RegionInfo));
        size_t roundedMetadataSize = 0;
        CHECK_DETAIL(CheckedRoundUpSize(metadataSize, MapleRuntime::MRT_PAGE_SIZE, roundedMetadataSize),
                     "region metadata round-up overflows: metadataSize=%zu pageSize=%zu", metadataSize,
                     MapleRuntime::MRT_PAGE_SIZE);
        return roundedMetadataSize;
    }

    template<Generation G>
inline size_t RegionManager::CollectRegion(RegionInfo* region)
    {
        MarkView<G> view = region->GetRouteMarkView<G>();
        const bool knownEmpty = IsKnownEmptyForView(region, view);
        // whoempty: record the *decision*, with the live-byte count it was made on, into the same
        // ring ClearUnits writes to.  ClearUnits passes liveBefore as a literal 0
        // (RegionInfo.h:1429 `TraceClear::NoteRange(unitAddress, size, "clear_units", nullptr, 0)`),
        // so a `liveBefore=0` in a clear entry says nothing about the region -- it is a constant.
        // This entry carries the real number, taken at the one edge where a region dies.
        TraceClear::NoteRange(region->GetRegionStart(), region->GetRegionSize(),
                              knownEmpty ? "coll_empty" : "coll_live", region, region->GetLiveByteCount(),
                              static_cast<unsigned>(G), 0);
        DLOG(REGION, "collect region %p@[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
             region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType());
        // f3why2/livesame: always-on enter + knownEmpty_marked class.

        // emptylive: epoch-split size-walk on knownEmpty (gate MRT_GCV2_EMPTYLIVE).

        GarbRegionDiag::NoteCollectEnter(region);
        // Probe: knownEmpty region still holds valid object headers (gcreclaim / B2 H1).
        {
            // gcreclaim was written for exactly the question now in hand -- does a region we are
            // about to declare empty still contain valid object headers, and are any of them
            // marked. This live census stays until the corresponding blocker closes.
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
                         validObjs, markedObjs, static_cast<unsigned>(region->RelocateObserve()));
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
                GCPhase phase = Heap::GetHeap().GetGCPhase();
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
        DumpScrubCostAndReset("post-reclaim-batch");
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

inline void RegionManager::ClearAllLiveInfo()
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

inline uintptr_t RegionManager::AllocPinnedLocked(size_t size)
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

    template<Generation G>
inline void RegionManager::ClearLiveInfo(RegionList& list)
    {
        RegionList tmp("temp region list");
        list.CopyListTo(tmp);
        tmp.VisitAllRegions([](RegionInfo* region) {
            MarkView<G> view = region->GetMarkView<G>();
            region->ClearLiveInfo(view);
        });
    }

inline void RegionManager::TagHugePage(RegionInfo* region, size_t num) const
{
#if defined (__linux__) || defined(__OHOS__) || defined(__ANDROID__)
    (void)madvise(reinterpret_cast<void*>(region->GetRegionStart()), num * RegionInfo::UNIT_SIZE, MADV_HUGEPAGE);
#else
    (void)region;
    (void)num;
#endif
}

inline void RegionManager::UntagHugePage(RegionInfo* region, size_t num) const
{
#if defined (__linux__) || defined(__OHOS__) || defined(__ANDROID__)
    (void)madvise(reinterpret_cast<void*>(region->GetRegionStart()), num * RegionInfo::UNIT_SIZE, MADV_NOHUGEPAGE);
#else
    (void)region;
    (void)num;
#endif
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
#if defined(MRT_GCV2_REGION_WAIT_DIAG)
        static std::atomic<size_t> g_regionWaitClaim{ 0 };
        const size_t claimN = g_regionWaitClaim.fetch_add(1, std::memory_order_relaxed) + 1;
        if (claimN <= 8 || (claimN & (claimN - 1)) == 0) {
            LOG(RTLOG_ERROR,
                "[GCV2][region-wait-claim] n=%zu from=%p claim=1 pending=%zu",
                claimN, reinterpret_cast<void*>(selected.request->from()),
                regionManager.GetRelocationRequestQueue().PendingCount());
        }
#endif
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
class ForwardTask : public GCWorkerTask {
public:
    ForwardTask(RegionManager& manager, RegionList& fromSpace)
        : regionManager(manager), fromRegionList(fromSpace) {}

    ~ForwardTask() override = default;
#if defined(MRT_TESTABLE_INTERNALS)
    MRT_EXPORT void Work(uint32_t) override;
#else
    __attribute__((visibility("hidden"))) void Work(uint32_t) override
    {
        detail::ExecuteForwardTask<G>(regionManager, fromRegionList);
    }
#endif

private:
    RegionManager& regionManager;
    RegionList& fromRegionList;
};






inline bool RegionManager::RouteRegion(RegionInfo* fromRegionInfo, bool mayWait)
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
                 static_cast<unsigned>(fromRegionInfo->RelocateObserve()),
                 static_cast<unsigned>(fromRegionInfo->IsYoungRegion()),
                 static_cast<unsigned>(fromRegionInfo->IsLiveCountAuthoritative()));
            return false;
        }
        // zRelocate.cpp:1155-1158 claimant runs page work; consumers wait (zRelocate.cpp:403-409).
        auto owner = ForwardingTable::RetainPageOwner(fromRegionInfo);
        if (owner && owner->is_done()) {
            return !owner->in_place();
        }
        if (owner && ZForwardingLife::CurrentPageWork() == owner.get()) {
            if (RelocateClaimedPage(fromRegionInfo)) {
                return true;
            }
            owner->set_in_place();
            return false;
        }
        if (!mayWait) {
            return false;
        }
        while (true) {
            owner = ForwardingTable::RetainPageOwner(fromRegionInfo);
            if (owner && owner->is_done()) {
                return !owner->in_place();
            }
            if (owner && ZForwardingLife::CurrentPageWork() == owner.get()) {
                if (RelocateClaimedPage(fromRegionInfo)) {
                    return true;
                }
                owner->set_in_place();
                return false;
            }
            sched_yield();
        }
    }





} // namespace MapleRuntime
#endif
