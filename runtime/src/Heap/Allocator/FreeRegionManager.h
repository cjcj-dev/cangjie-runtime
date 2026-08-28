// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_FREE_REGION_MANAGER_H
#define MRT_FREE_REGION_MANAGER_H

#include <cstdint>
#include <vector>

#include "CartesianTree.h"
#include "RegionInfo.h"
#include "Heap/Collector/Uncommitter.h"
#include "Common/ScopedObjectAccess.h"

namespace MapleRuntime {
class RegionManager;

// This class is and should be accessed only for region allocation. we do not rely on it to check region status.
class FreeRegionManager {
    using UnitIndex = CartesianTree::Index;
    using UnitCount = CartesianTree::Count;

public:
    explicit FreeRegionManager(RegionManager& manager) : regionManager(manager) {}

    virtual ~FreeRegionManager()
    {
        dirtyUnitTree.Fini();
        releasedUnitTree.Fini();
        markQuarantineTree.Fini();
    }

    void Initialize(UnitCount regionCnt)
    {
        releasedUnitTree.Init(regionCnt);
        dirtyUnitTree.Init(regionCnt);
        markQuarantineTree.Init(regionCnt);
    }

    // allowSaferegion: when false, never ScopedEnterSaferegion (ROUTING critical section —
    // holding routeState=ROUTING while waiting on phase transition deadlocks PreForward;
    // see REPORT-routespin.md). Best-effort one pass; caller falls back to CompactRegion.
    RegionInfo* TakeRegion(size_t num, RegionInfo::UnitRole uclass, bool expectPhysicalMem,
                           bool allowSaferegion = true, bool clearPayload = true)
    {
        UnitIndex idx = 0;
        bool tryDirtyTree = true;
        bool tryReleasedTree = true;

        // try as hard as we can to take free regions for allocation.
        while (tryDirtyTree || tryReleasedTree) {
            // first try to get a dirty region.
            if (tryDirtyTree && dirtyUnitTreeMutex.try_lock()) {
                bool dirtyOk = false;
                {
                    // TakeUnits may refresh the residual free-tree node via
                    // InitRegionInfo before returning the selected extent.
                    // Carry a structural permit only across that maintenance;
                    // the selected extent is checked immediately afterwards.
                    FromPageDetach::ReusePermitScope treePermit;
#if defined(__OHOS__)
                    dirtyOk = dirtyUnitTree.TakeUnitsLowAddr(num, idx);
#else
                    dirtyOk = dirtyUnitTree.TakeUnits(num, idx);
#endif
                }
                if (dirtyOk) {
                    MAddress start = RegionInfo::GetUnitAddress(idx);
                    RegionInfo* dirtyRegion = RegionInfo::TryGetRegionInfoAt(start);
                    if (!FromPageDetach::FromPageDetachCheck(dirtyRegion,
                                                             FromPageDetach::Site::TAKE_DIRTY_REUSE)) {
                        dirtyUnitTreeMutex.unlock();
                        AddDetachQuarantineUnits(idx, num, false, false);
                        continue;
                    }
                    FromPageDetach::ReusePermitScope reusePermit;
                    TraceClear::NoteRegionEvent(start, num * RegionInfo::UNIT_SIZE, "dirty_take", dirtyRegion, 0,
                                                static_cast<unsigned int>(dirtyRegion->IsGhostFromRegion()),
                                                static_cast<unsigned int>(dirtyRegion->GetRegionType()),
                                                static_cast<unsigned int>(dirtyRegion->GetRouteState()));
                    DLOG(REGION, "c-tree %p alloc dirty units[%u+%u, %u) @[0x%zx, 0x%zx), %u dirty-units left",
                        &dirtyUnitTree, idx, num, idx + num, RegionInfo::GetUnitAddress(idx),
                        RegionInfo::GetUnitAddress(idx + num), dirtyUnitTree.GetTotalCount());

                    if (clearPayload) {
                        // Ordinary allocations require zero-filled reused payload. A segmented
                        // large reference array establishes that state itself after publication.
                        RegionInfo::ClearUnits(idx, num, FillerZeroDiag::Site::DIRTY_TAKE);
                    }
                    RegionInfo* region = RegionInfo::InitRegion(idx, num, uclass);
                    dirtyUnitTreeMutex.unlock();
                    return region;
                }
                tryDirtyTree = false; // once we fail to take units, stop trying.
                dirtyUnitTreeMutex.unlock();
            }

            // then try to get a released region.
            if (tryReleasedTree && releasedUnitTreeMutex.try_lock()) {
                bool releasedOk = false;
                {
                    FromPageDetach::ReusePermitScope treePermit;
#if defined(__OHOS__)
                    releasedOk = releasedUnitTree.TakeUnitsLowAddr(num, idx);
#else
                    releasedOk = releasedUnitTree.TakeUnits(num, idx);
#endif
                }
                if (releasedOk) {
                    RegionInfo* releasedRegion =
                        RegionInfo::TryGetRegionInfoAt(RegionInfo::GetUnitAddress(idx));
                    if (!FromPageDetach::FromPageDetachCheck(releasedRegion,
                                                             FromPageDetach::Site::TAKE_RELEASED_REUSE)) {
                        releasedUnitTreeMutex.unlock();
                        AddDetachQuarantineUnits(idx, num, true, false);
                        continue;
                    }
                    FromPageDetach::ReusePermitScope reusePermit;
                    RegionInfo::CommitUnits(idx, num);
                    Uncommitter::CancelCycle();
                    DLOG(REGION, "c-tree %p alloc released units[%u+%u, %u) @[0x%zx, 0x%zx), %u released-units left",
                        &releasedUnitTree, idx, num, idx + num, RegionInfo::GetUnitAddress(idx),
                        RegionInfo::GetUnitAddress(idx + num), releasedUnitTree.GetTotalCount());
                    RegionInfo* region = RegionInfo::InitRegion(idx, num, uclass);
                    releasedUnitTreeMutex.unlock();
                    PrehandleReleasedUnit(expectPhysicalMem && clearPayload, idx, num);
                    return region;
                }
                tryReleasedTree = false; // once we fail to take units, stop trying.
                releasedUnitTreeMutex.unlock();
            }
            // routefix: ROUTING holders must not park here (LeaveSaferegion → WaitForPhaseTransition).
            if (!allowSaferegion) {
                return nullptr;
            }
            ScopedEnterSaferegion enterSaferegion(true);
        }

        return nullptr;
    }

    // add units [idx, idx + num)
    void AddGarbageUnits(UnitIndex idx, UnitCount num)
    {
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        if (UNLIKELY(!dirtyUnitTree.MergeInsert(idx, num, true))) {
            LOG(RTLOG_FATAL, "tid %d: failed to add dirty units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    // mark-epoch quarantine: units reclaimed after DispelGhost must not enter the dirty
    // tree (mutator TakeRegion → ClearUnits) until the next major concurrent mark ends.
    // INV: concurrent mark may still hold plain strong refs into this range (SATB).
    void AddMarkQuarantineUnits(UnitIndex idx, UnitCount num)
    {
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(markQuarantineTreeMutex);
        if (UNLIKELY(!markQuarantineTree.MergeInsert(idx, num, true))) {
            LOG(RTLOG_FATAL, "tid %d: failed to add mark-quarantine units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    // Release point = major PostTrace entry (TRACE+CLEAR_SATB done). Moves all quarantined
    // units into the dirty tree so allocation may ClearUnits them again.
    size_t ReleaseMarkQuarantineToDirty()
    {
        size_t releasedUnits = 0;
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lockQ(markQuarantineTreeMutex);
        std::lock_guard<std::mutex> lockD(dirtyUnitTreeMutex);
        while (true) {
            auto node = markQuarantineTree.RootNode();
            if (node == nullptr) {
                break;
            }
            UnitIndex idx = node->GetIndex();
            UnitCount num = node->GetCount();
            CHECK_DETAIL(markQuarantineTree.TakeUnits(num, idx, false),
                         "tid %d: failed to promote mark-quarantine units[%u+%u, %u)", GetTid(), idx, num,
                         idx + num);
            if (UNLIKELY(!dirtyUnitTree.MergeInsert(idx, num, true))) {
                LOG(RTLOG_FATAL, "tid %d: failed to promote mark-quarantine units [%u+%u, %u) to dirty",
                    GetTid(), idx, num, idx + num);
            }
            releasedUnits += num;
        }
        return releasedUnits;
    }

    UnitCount GetMarkQuarantineUnitCount() const
    {
        std::lock_guard<std::mutex> lg(markQuarantineTreeMutex);
        return markQuarantineTree.GetTotalCount();
    }

    static bool ExtentReadyForReleasedCache(RegionInfo* region)
    {
        if (region == nullptr) {
            return true;
        }
        if (region->ForwardingRefCount() != 0) {
            return false;
        }
        return !ForwardingTable::HasLiveCarrier(region->GetRegionStart(),
                                                region->GetRegionSizeForDetachCheck());
    }

    void AddReleaseUnits(UnitIndex idx, UnitCount num);

    UnitCount GetDirtyUnitCount() const
    {
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        return dirtyUnitTree.GetTotalCount();
    }

    UnitCount GetReleasedUnitCount() const
    {
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        return releasedUnitTree.GetTotalCount();
    }

    // Return the largest contiguous block in released/dirty tree (root node = max-heap top)
    UnitCount GetReleasedMaxBlock() const
    {
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        const auto* r = releasedUnitTree.RootNode();
        return r ? r->GetCount() : 0;
    }
    UnitCount GetDirtyMaxBlock() const
    {
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        const auto* r = dirtyUnitTree.RootNode();
        return r ? r->GetCount() : 0;
    }
    size_t GetReleasedNodeCount() const
    {
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        return releasedUnitTree.GetNodeCount();
    }
    size_t GetDirtyNodeCount() const
    {
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        return dirtyUnitTree.GetNodeCount();
    }

#if defined(MRT_DEBUG)
    void DumpReleasedUnitTree() const { releasedUnitTree.DumpTree("released-unit tree"); }
    void DumpDirtyUnitTree() const { dirtyUnitTree.DumpTree("dirty-unit tree"); }
#endif

    size_t CalculateBytesToRelease() const;
    size_t ReleaseGarbageRegions(size_t targetCachedSize);
    size_t UncommitIdleUnits(size_t maxBytes, uint64_t idleBeforeNs, bool honorCancel = true);

    // Phase-2 FROM_PAGE_DETACH_GATE. Entries are withheld from both allocator
    // trees until a major mark closure rechecks the same central predicate.
    void AddDetachQuarantineRegion(RegionInfo* region, bool releasePhysical = false);
    void AddDetachQuarantineUnits(UnitIndex idx, UnitCount num, bool released, bool needsInit,
                                  bool releasePhysical = false);
    size_t ReleaseDetachQuarantineAfterMajor();
    bool HasDetachQuarantine() const
    {
        std::lock_guard<std::mutex> lock(detachQuarantineMutex);
        return !detachQuarantine.empty();
    }

private:
    size_t UncommitIdleUnitsImpl(size_t maxBytes, uint64_t idleBeforeNs, bool honorCancel);

    struct DetachQuarantineEntry {
        UnitIndex idx;
        UnitCount num;
        uint8_t rechecks;
        bool released;
        bool needsInit;
        bool releasePhysical;
    };

    inline void PrehandleReleasedUnit(bool expectPhysicalMem, size_t idx, size_t num) const
    {
        if (expectPhysicalMem) {
            RegionInfo::ClearUnits(idx, num, FillerZeroDiag::Site::RELEASED_PRE);
        }
    }
    RegionManager& regionManager;

    // physical pages of released units are probably released and they are prepared for allocation.
    mutable std::mutex releasedUnitTreeMutex;
    CartesianTree releasedUnitTree;

    // dirty units are neither cleared nor released, thus must be zeroed explicitly for allocation.
    mutable std::mutex dirtyUnitTreeMutex;
    CartesianTree dirtyUnitTree;

    // Post-dispel units held until major mark ends (see AddMarkQuarantineUnits).
    mutable std::mutex markQuarantineTreeMutex;
    CartesianTree markQuarantineTree;

    mutable std::mutex detachQuarantineMutex;
    std::vector<DetachQuarantineEntry> detachQuarantine;
};
} // namespace MapleRuntime
#endif // MRT_FREE_REGION_MANAGER_H
