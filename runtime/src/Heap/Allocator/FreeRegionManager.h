// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_FREE_REGION_MANAGER_H
#define MRT_FREE_REGION_MANAGER_H

#include <vector>
#include <memory>
#include "RangeRegistry.h"

#include "AllocationStallQueue.h"
#include "CartesianTree.h"
#include "RegionInfo.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/Collector/Uncommitter.h"

namespace MapleRuntime {
class RegionManager;

// This class is and should be accessed only for region allocation. we do not rely on it to check region status.
class FreeRegionManager {
    using UnitIndex = CartesianTree::Index;
    using UnitCount = CartesianTree::Count;

public:
    explicit FreeRegionManager(RegionManager& manager) : regionManager(manager) {}

    virtual ~FreeRegionManager() { markQuarantineTree.Fini(); }
    void Initialize(UnitCount regionCnt, const std::vector<MemoryRange>& reservations, MemMap& owner);
    bool ClaimPageMemory(size_t num, uint32_t partition, PageMemory& memory);
    bool PreparePageMemory(PageMemory& memory);

    // zPageAllocator.cpp:1470-1515: consume the already-owned vmem outside
    // the allocator lock. A02p owns partial-commit results and suffix cleanup.
    RegionInfo* MaterializePageMemory(PageMemory& memory, RegionInfo::UnitRole role,
                                     bool expectPhysicalMem, bool clearPayload, size_t& committedUnits)
    {
        (void)expectPhysicalMem;
        committedUnits = 0;
        if (!PreparePageMemory(memory)) { return nullptr; }
        const size_t idx = memory.index;
        const size_t num = memory.units;
        committedUnits = memory.committed ? num : 0;
        const bool wasCommitted = memory.committed;
        FromPageDetach::ReusePermitScope reusePermit;
        if (!wasCommitted) {
            const size_t committed = RegionInfo::CommitUnits(idx, num);
            CHECK(committed <= num * RegionInfo::UNIT_SIZE && committed % RegionInfo::UNIT_SIZE == 0);
            committedUnits = committed / RegionInfo::UNIT_SIZE;
            if (committedUnits != num) {
                return nullptr;
            }
            memory.committed = true;
        }
        if ((wasCommitted || memory.harvestedUnits != 0) && clearPayload) {
            RegionInfo::ClearUnits(idx, num, FillerZeroDiag::Site::DIRTY_TAKE);
        }
        RegionInfo* region = RegionInfo::InitRegion(idx, num, role);
        if (!wasCommitted) {
            PrehandleReleasedUnit(clearPayload, idx, num);
        }
        return region;
    }

    void AddGarbageUnits(UnitIndex idx, UnitCount num);

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
    size_t ReleaseMarkQuarantineToDirty();

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
    UnitCount GetDirtyUnitCount() const;
    UnitCount GetReleasedUnitCount() const;
    UnitCount GetReleasedMaxBlock() const;
    UnitCount GetDirtyMaxBlock() const;
    size_t GetReleasedNodeCount() const;
    size_t GetDirtyNodeCount() const;
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

    struct Partition {
        uint32_t node;
        std::vector<MemoryRange> reservations;
        RangeRegistry virtualMemory;
        MappedCache cache;
        size_t pendingGrowth{ 0 };
        explicit Partition(uint32_t id) : node(id) {}
    };
    Partition& PartitionFor(uintptr_t address);
    void InsertCommitted(Partition& partition, UnitIndex index, UnitCount count);
    void ReturnMemory(UnitIndex index, UnitCount count);
    mutable std::mutex cacheMutex;
    std::vector<std::unique_ptr<Partition>> partitions;
    MemMap* backingOwner{ nullptr };
    size_t nextPartition{ 0 };

    // Post-dispel units held until major mark ends (see AddMarkQuarantineUnits).
    mutable std::mutex markQuarantineTreeMutex;
    CartesianTree markQuarantineTree;

    mutable std::mutex detachQuarantineMutex;
    std::vector<DetachQuarantineEntry> detachQuarantine;
};
} // namespace MapleRuntime
#endif // MRT_FREE_REGION_MANAGER_H
