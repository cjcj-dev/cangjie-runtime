// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_ALLOCATION_STALL_QUEUE_H
#define MRT_ALLOCATION_STALL_QUEUE_H

#include "Heap/Allocator/RegionManager.h"
#include "Heap/z/zStat.hpp"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>
#include "Heap/z/zFuture.inline.hpp"
#include "Heap/z/zList.inline.hpp"
#include "Heap/z/zVirtualMemoryManager.hpp"
#include "Heap/z/zArray.inline.hpp"

#if defined(MRT_GC_UNIT_TESTS) || defined(MRT_TESTABLE_INTERNALS)
#define MRT_ALLOCATION_STALL_OBSERVE 1
#endif

#include "Heap/z/zAllocationFlags.hpp"

namespace MapleRuntime {

// ZVirtualMemory represented in heap granules; ownership travels with the
// page allocation until materialization or hand-back. The partition index
// selects the cache and virtual registry that own the complete allocation.
struct PageMemory {
    size_t index{ 0 };
    size_t size{ 0 };
    uint32_t partition{ 0 };
    bool committed{ false };
    // ZMemoryAllocation::partial_vmems: these extents leave the mapped cache
    // under the allocator owner and travel with the allocation request.
    ZArray<ZVirtualMemory> partialMappings;
    bool virtualClaimed{ true };
    size_t harvestedBytes{ 0 };

};

// One object represents one blocked allocation.  It is deliberately owned by
// the allocator caller; the queue only retains the pointer until a terminal
// answer is published.
class ZPageAllocation {
public:
    ZPageAllocation(size_t size, uint8_t role, bool physical, bool clear, ZAllocationFlags flags = {})
        : size(size), role(role), physical(physical), clear(clear), flags(flags) {}
    ZPageAllocation(const ZPageAllocation&) = delete;
    ZPageAllocation& operator=(const ZPageAllocation&) = delete;

    size_t GetSize() const { return size; }
    ZAllocationFlags Flags() const { return flags; }
    uint8_t GetRole() const { return role; }
    bool ExpectsPhysicalMemory() const { return physical; }
    bool ClearsPayload() const { return clear; }
    PageMemory& Memory() { return memory; }
    const PageMemory& Memory() const { return memory; }

    // zPageAllocator.cpp:525-531 ZPageAllocation::wait/satisfy over ZFuture<bool>.
    bool Wait()
    {
        return stallResult.get();
    }

    void Satisfy(bool value)
    {
        stallResult.set(value);
    }

private:
    friend class AllocationStallQueue;
    friend class ZList<ZPageAllocation>;

    const size_t size;
    uint64_t sequence{ 0 };
    const uint8_t role;
    const bool physical;
    const bool clear;
    const ZAllocationFlags flags;
    PageMemory memory;
    // zPageAllocator.cpp:420-421 ZPageAllocation: ZFuture<bool> _stall_result
    // and the ZListNode that links it on the allocator's stalled list.
    ZFuture<bool> stallResult;
    ZListNode<ZPageAllocation> _node;
};
using AllocationStallRequest = ZPageAllocation;

// Allocator-owned FIFO.  Enqueue returns true only for the transition from
// empty to non-empty, giving the first waiter ownership of the GC request.
class AllocationStallQueue {
public:
    explicit AllocationStallQueue(std::mutex& owner) : mutex(owner) {}

    // The allocator holds the same owner across claim failure and enqueue.
    bool EnqueueLocked(ZPageAllocation& request)
    {
        const bool requestGc = !gcInProgress;
        gcInProgress = true;
        request.sequence = ++lastSequence;
        requests.insert_last(&request);
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
        ++enqueued;
#endif
        return requestGc;
    }

    // zHeap.inline.hpp: is_alloc_stalling; read the actual outstanding FIFO.
    bool IsStalling() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return !requests.is_empty();
    }

    uint64_t CaptureWaveBoundary() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return lastSequence;
    }

    size_t SatisfyAvailable(const std::function<bool(ZPageAllocation&)>& claim)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return SatisfyAvailableLocked(claim);
    }

    size_t SatisfyAvailableLocked(const std::function<bool(ZPageAllocation&)>& claim)
    {
        size_t satisfied = 0;
        while (!requests.is_empty()) {
            ZPageAllocation* request = requests.first();
            if (!claim(*request)) {
                break;
            }
            requests.remove_first();
            request->Satisfy(true);
            ++satisfied;
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
            ++dequeued;
            ++satisfiedCount;
#endif
        }
        return satisfied;
    }

    bool CompleteWave(uint64_t boundary)
    {
        std::lock_guard<std::mutex> lock(mutex);
        while (!requests.is_empty() && requests.first()->sequence <= boundary) {
            ZPageAllocation* request = requests.first();
            requests.remove_first();
            request->Satisfy(false);
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
            ++dequeued;
            ++failedCount;
#endif
        }
        if (requests.is_empty()) {
            gcInProgress = false;
            return false;
        }
        return true;
    }

#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    size_t Pending() const;
    size_t EnqueuedCount() const;
    size_t DequeuedCount() const;
    size_t SatisfiedCount() const;
    size_t FailedCount() const;
#endif

private:
    std::mutex& mutex;
    // zPageAllocator.hpp:165 ZList<ZPageAllocation> _stalled.
    ZList<ZPageAllocation> requests;
    uint64_t lastSequence{ 0 };
    bool gcInProgress{ false };
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    size_t enqueued{ 0 };
    size_t dequeued{ 0 };
    size_t satisfiedCount{ 0 };
    size_t failedCount{ 0 };
#endif
};

} // namespace MapleRuntime

#include "Heap/Allocator/AllocationStallQueue.h"
#endif // MRT_ALLOCATION_STALL_QUEUE_H

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_FREE_REGION_MANAGER_H
#define MRT_FREE_REGION_MANAGER_H

#include <vector>
#include <memory>
#include "Heap/z/zMappedCache.hpp"
#include "Heap/z/zPhysicalMemoryManager.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"
#include "Heap/z/zVirtualMemoryManager.inline.hpp"


#include "Heap/z/zPage.hpp"
#include "Heap/z/zUncommitter.hpp"

namespace MapleRuntime {
class RegionManager;

// This class is and should be accessed only for region allocation. we do not rely on it to check region status.
class FreeRegionManager {

public:
    explicit FreeRegionManager(RegionManager& manager) : regionManager(manager) {}

    virtual ~FreeRegionManager() = default;
    // ZPageAllocator(min/initial/max capacity) owns _virtual/_physical and one
    // ZPartition per NUMA id (zPageAllocator.cpp:1201-1260); the partitions
    // here consume the two managers the same way.
    void Initialize(ZVirtualMemoryManager& virtualMemory,
                    ZPhysicalMemoryManager& physicalMemory, size_t maxCapacity);
    bool ClaimPageMemory(size_t num, PageMemory& memory, ZAllocationFlags flags = {});
    bool PreparePageMemory(PageMemory& memory);

    // zPageAllocator.cpp:1470-1515 alloc_page_inner: consume the already-owned
    // vmem outside the allocator lock: claim_physical_for_increased_capacity →
    // commit_and_map (cleanup_failed_commit on a partial commit) → create_page.
    ZPage* MaterializePageMemory(PageMemory& memory, ZPageType role,
                                     bool expectPhysicalMem, bool clearPayload, size_t& committedBytes,
                                     PageAge age = PageAge::old);
    // ZPartition::free_memory_alloc_failed (zPageAllocator.cpp:1079-1101).
    // Requires the owning RegionManager page allocator lock.
    void FreeMemoryAllocFailed(PageMemory& memory);

    // ZPartition thin functions (zPageAllocator.cpp:790-920). They forward to
    // the two managers with this partition's numa id.
    ZVirtualMemory claim_virtual(size_t size, uint32_t partition_id);
    size_t claim_virtual(size_t size, uint32_t partition_id, ZArray<ZVirtualMemory>* vmems_out);
    void free_virtual(const ZVirtualMemory& vmem, uint32_t partition_id);
    ZVirtualMemory free_and_claim_virtual_from_low_exact_or_many(size_t size, uint32_t partition_id,
                                                                 ZArray<ZVirtualMemory>* vmems_in_out);
    void claim_physical(const ZVirtualMemory& vmem, uint32_t partition_id);
    void free_physical(const ZVirtualMemory& vmem, uint32_t partition_id);
    size_t commit_physical(const ZVirtualMemory& vmem, uint32_t partition_id);
    size_t uncommit_physical(const ZVirtualMemory& vmem);
    void map_virtual(const ZVirtualMemory& vmem, uint32_t partition_id);
    void unmap_virtual(const ZVirtualMemory& vmem);
    void sort_segments_physical(const ZVirtualMemory& vmem);
    void stash_segments(const ZArraySlice<const ZVirtualMemory>& vmems, ZArray<zbacking_index>* stash_out) const;
    void restore_segments(const ZVirtualMemory& vmem, const ZArray<zbacking_index>& stash);
    void restore_segments(const ZArraySlice<const ZVirtualMemory>& vmems, const ZArray<zbacking_index>& stash);

    // ZPartition::_capacity accounting (zPageAllocator.cpp:648-676). The
    // committed capacity account lives here and nowhere else.
    size_t increase_capacity(uint32_t partition_id, size_t size);
    void decrease_capacity(uint32_t partition_id, size_t size, bool set_max_capacity);
    size_t capacity() const;

    // Global granule index plus byte extent <-> ZVirtualMemory.
    static ZVirtualMemory VirtualMemoryOf(size_t index, size_t count);
    static size_t IndexOf(const ZVirtualMemory& vmem);
    uint32_t PartitionIdOf(const ZVirtualMemory& vmem) const { return virtualMemory->lookup_partition_id(vmem); }

    void AddGarbageMemory(size_t idx, size_t num, bool allowSaferegion = true);

    // mark-epoch quarantine: units reclaimed after from-page reclaim must not enter the dirty
    // tree (mutator TakeRegion → ClearPageMemory) until the next major concurrent mark ends.
    // INV: concurrent mark may still hold plain strong refs into this range (SATB).
    void AddMarkQuarantineMemory(size_t idx, size_t num);

    // Release point = major PostTrace entry (TRACE+CLEAR_SATB done). Moves all quarantined
    // units into the dirty tree so allocation may ClearPageMemory them again.
    size_t ReleaseMarkQuarantineToDirty();

    size_t GetMarkQuarantineBytes() const
    {
        std::lock_guard<std::mutex> lg(markQuarantineTreeMutex);
        size_t bytes = 0;
        for (const auto& memory : markQuarantineMemory) { bytes += memory.size(); }
        return bytes;
    }

    size_t GetCachedBytes() const;
    // ZPartition::print_cache_on (zPageAllocator.cpp:1118-1121) for every partition.
    void PrintCacheOn() const;
    // zUncommitter.cpp:395-403: flush from the mapped cache under the page
    // allocator lock and record the flushed amount as claimed.
    size_t RemoveForUncommit(size_t flush, ZArray<ZVirtualMemory>* out);
    // zUncommitter.cpp:417-419: the flushed memory left the cache and was
    // uncommitted; adjust claimed and capacity.
    void UncommitFlushed(size_t flushed);

private:

    inline void ClearReleasedMemory(bool expectPhysicalMem, size_t idx, size_t num) const
    {
        if (expectPhysicalMem) {
            ZPage::ClearPageMemory(idx, num);
        }
    }
    RegionManager& regionManager;

    // ZPartition (zPageAllocator.hpp:57-141): numa id, mapped cache and the
    // capacity account; virtual/physical memory is reached through the managers.
    class ZPartition {
    public:
        uint32_t numaId;
        ZMappedCache cache;
        size_t capacity{ 0 };
        size_t claimed{ 0 };
        size_t used{ 0 };
        size_t currentMaxCapacity{ 0 };
        explicit ZPartition(uint32_t id) : numaId(id) {}
        size_t available() const { return currentMaxCapacity - used - claimed; }
        bool claim_capacity_fast_medium(PageMemory& memory);
    };
    using Partition = ZPartition;
    void InsertCommitted(Partition& partition, size_t index, size_t count);
    void FreeMemory(size_t index, size_t count);
    mutable std::mutex cacheMutex;
    std::vector<std::unique_ptr<Partition>> partitions;
    ZVirtualMemoryManager* virtualMemory{ nullptr };
    ZPhysicalMemoryManager* physicalMemory{ nullptr };
    size_t nextPartition{ 0 };

    // Post-dispel units held until major mark ends (see AddMarkQuarantineMemory).
    mutable std::mutex markQuarantineTreeMutex;
    std::vector<ZVirtualMemory> markQuarantineMemory;

};
} // namespace MapleRuntime
#endif // MRT_FREE_REGION_MANAGER_H

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
#include <memory>
#include <map>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

#include "Heap/z/zThreadLocalAllocBuffer.hpp"

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/ColourEncoding.h"
#include "Common/RunType.h"

#include "Heap/z/zDeferredConstructed.hpp"
#include "Heap/z/zLock.hpp"
#include "Heap/z/zRangeRegistry.hpp"
#include "Heap/z/zPageAge.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zValue.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zRelocate.hpp"
#include "securec.h"
#include "Heap/Allocator/SlotList.h"

namespace MapleRuntime {
class CompactCollector;
template<Generation G>
class ForwardTask;




struct FreePinnedSlotLists {
    static constexpr size_t ATOMIC_OBJECT_SIZE = 16;
    SlotList freeAtomicSlotList;

private:
    friend class RegionManager;
    uintptr_t PopFront(size_t size)
    {
        switch (size) {
            case ATOMIC_OBJECT_SIZE:
                return freeAtomicSlotList.PopFront(size);
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
            default:
                return;
        }
    }

    void Clear()
    {
        freeAtomicSlotList.Clear();
    }
};

// RegionManager needs to know header size and alignment in order to iterate objects linearly
// and thus its Alloc should be rewrite with AllocObj(objSize)
namespace GcUnit { struct GcHeapFixture; }

class ZGeneration;

// zPageAllocator.hpp:291-330 — point-in-time allocator account feeding
// ZStatHeap's sample points. Host differences (PLAN §5): the host heap has no
// min-heap-size parameter (min_capacity() reports 0); used is region-granular
// committed-used, matching ZGC's page-granular _used.
class ZPageAllocatorStats {
public:
    ZPageAllocatorStats(size_t minCapacity, size_t maxCapacity, size_t softMaxCapacity, size_t capacity,
                        size_t used, size_t usedHigh, size_t usedLow, size_t usedGeneration, size_t freed,
                        size_t promoted, size_t compacted, size_t allocationStalls)
        : _minCapacity(minCapacity), _maxCapacity(maxCapacity), _softMaxCapacity(softMaxCapacity),
          _capacity(capacity), _used(used), _usedHigh(usedHigh), _usedLow(usedLow),
          _usedGeneration(usedGeneration), _freed(freed), _promoted(promoted), _compacted(compacted),
          _allocationStalls(allocationStalls)
    {}

    size_t min_capacity() const { return _minCapacity; }
    size_t max_capacity() const { return _maxCapacity; }
    size_t soft_max_capacity() const { return _softMaxCapacity; }
    size_t capacity() const { return _capacity; }
    size_t used() const { return _used; }
    size_t used_high() const { return _usedHigh; }
    size_t used_low() const { return _usedLow; }
    size_t used_generation() const { return _usedGeneration; }
    size_t freed() const { return _freed; }
    size_t promoted() const { return _promoted; }
    size_t compacted() const { return _compacted; }
    size_t allocation_stalls() const { return _allocationStalls; }

private:
    const size_t _minCapacity;
    const size_t _maxCapacity;
    const size_t _softMaxCapacity;
    const size_t _capacity;
    const size_t _used;
    const size_t _usedHigh;
    const size_t _usedLow;
    const size_t _usedGeneration;
    const size_t _freed;
    const size_t _promoted;
    const size_t _compacted;
    const size_t _allocationStalls;
};

class RegionManager {
    friend struct GcUnit::GcHeapFixture;
    friend class ZObjectAllocator;
    friend struct PinRootTestAccess;
    friend struct IkeKeepTestAccess;
    friend struct IsFromRegTestAccess;

public:
    /* region memory layout:
        1. region info for each region, part of heap metadata
        2. region space for allocation, i.e., the heap
    */
    __attribute__((visibility("hidden"))) static size_t GetHeapMemorySize(size_t heapSize);

    __attribute__((visibility("hidden"))) static size_t GetAlignedHeapSize(size_t heapSize);

    // get metadataSize by regionNum or unitNumber
    // page-table geometry, not a reverse metadata array
    __attribute__((visibility("hidden"))) static size_t GetMetadataSize();
#if defined(__EULER__)
    void SetCacheRatio(double minSize, double maxSize, double defaultParam);
#endif
    // ZPageAllocator::ZPageAllocator(min/initial/max capacity): the two memory
    // managers are constructed for max_capacity and consumed by the partitions.
    void Initialize(size_t regionNum, uintptr_t regionInfoStart, ZVirtualMemoryManager& virtualMemory,
                    ZPhysicalMemoryManager& physicalMemory, const HeapParam& heapParam, double garbageThreshold);
    // Address envelope [lowest reserved offset, ZAddressOffsetMax).
    static ZVirtualMemory ReservedAddressSpan(const ZVirtualMemoryManager& virtualMemory);
    // P01 reverse-metadata ABI adapter; called only before runtime allocation.
    static std::vector<ZPage::ReservedSegment> ReservedSegments(ZVirtualMemoryManager& virtualMemory);

    void VisitPageOwners(const std::function<void(ZPage*)>& visitor) const
    {
        ZPage::VisitPageOwners(visitor);
    }

    // ZPageAllocator::capacity(): sum of ZPartition::_capacity.
    size_t GetCommittedCapacity() const { return freeRegionManager.capacity(); }

    size_t GetHeapCapacity() const { return heapCapacity; }


    // zPageAllocator.cpp:1201-1260: page resource ownership belongs to
    // this allocator, not to its object-allocation consumers.
    MAddress GetSpaceStartAddress() const { return reservedStart; }
    MAddress GetSpaceEndAddress() const { return reservedEnd; }

    RegionManager();
    RegionManager(const HeapParam& param, double garbageThreshold);

    RegionManager(const RegionManager&) = delete;

    RegionManager& operator=(const RegionManager&) = delete;

    // allowSaferegion=false: no ScopedEnterSaferegion under ROUTING (routefix / REPORT-routespin).


    // ZObjectAllocator::alloc / alloc_for_relocation. These pages never belong
    // to an AllocBuffer: a thread's TLAB and a CPU's shared page are distinct.
    // P14: the handshake pause must serialize pinned installation with retirement/seqnum.
    std::mutex& PinnedAllocationMutex() { return pinnedAllocationMutex; }

    // ZHeap::account_alloc_page/account_undo_alloc_page: backing extents,
    // independent of the thread-local requested bytes and retirement waste.

    // Stable cycle history: read under the statistics lock, at a safepoint,
    // or with managed access preventing the next young pause.
    size_t GetTLABUsed() const { return lastTLABUsed; }
    size_t GetTLABCapacity() const { return static_cast<size_t>(tlabCapacity); }
    void InitializeTLAB(AllocBuffer& buffer);
    void ResetTLABUsage();
    void PublishTLABStatistics();
    void RetireTLABStatistics(AllocBuffer& buffer);

    template<Generation G>
    void ForwardFromRegions(ZWorkers& workers);
    template<Generation G>
    void ForwardFromRegions();
    template<Generation G>
    void ForwardRegion(ZPage* region);
    ZRelocateQueue& GetZRelocateQueue() { return relocateQueue; }
    bool StallAllocation(AllocationStallRequest& request, bool requestGc);
    bool ClaimAllocationLocked(AllocationStallRequest& request);
    void ReturnPageMemory(const PageMemory& memory);
    void SatisfyStalledAllocations();
    bool IsAllocationStalling() const { return allocationStallQueue.IsStalling(); }
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    MRT_EXPORT size_t PendingStalledAllocations() const;
    MRT_EXPORT size_t EnqueuedStalledAllocations() const;
    MRT_EXPORT size_t DequeuedStalledAllocations() const;
    MRT_EXPORT size_t SatisfiedStalledAllocations() const;
    MRT_EXPORT size_t FailedStalledAllocations() const;
#endif
    // In-place relocation account (feeds ZStatRelocation::AtRelocateEnd).
    // Not observation-gated: the relocation report reads it in every build.
    void ResetInPlaceRelocatedCounts()
    {
        inPlaceSmallCount.store(0, std::memory_order_relaxed);
        inPlaceMediumCount.store(0, std::memory_order_relaxed);
    }
    void NoteInPlaceRelocated(const ZPage* region)
    {
        if (region->is_medium()) {
            inPlaceMediumCount.fetch_add(1, std::memory_order_relaxed);
        } else if (region->is_small()) {
            inPlaceSmallCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::pair<size_t, size_t> InPlaceRelocatedCounts() const
    {
        return { inPlaceSmallCount.load(std::memory_order_relaxed),
                 inPlaceMediumCount.load(std::memory_order_relaxed) };
    }
    // zPageAllocator.cpp:1362 stats field: currently stalled mutators. Host
    // difference: the stall queue only counts under MRT_ALLOCATION_STALL_OBSERVE.
    size_t AllocationStallsNow() const
    {
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
        return allocationStallQueue.Pending();
#else
        return 0;
#endif
    }
    template<Generation G>
    void ForwardClaimedPage(ZPage* region, ZForwarding* owner, bool claimed = false,
                            bool inPlace = false);
    template<Generation G>
    void StartForwardFromRegions(ZWorkers& workers);
    template<Generation G>
    void DrainForwardFromRegions();
    bool RelocationStarted() const { return relocationStarted; }
    // ZRelocateWork::update_remset_promoted, called by the relocating page worker.
    static void RememberPromotedObject(BaseObject* object);
    // ZRelocationSet::flip_promoted_pages: page pointers only; liveness belongs to the page.
    void RememberFlipPromotedPages(ZWorkers& workers);
    void ResetFlipPromotedPages();
    void promote_used(const ZPage* from, const ZPage* to);
    void safe_destroy_page(ZPage* page);
    void free_page(ZPage* page);
    void StampCensusBoundaries();
    void PromoteAllRegions();
    // CompactRegion's list-ownership tail. A concurrent stay-young path may
    // already have moved the region to recent-full; never steal its links.
    void EnlistCompactedRegionForAllocator(ZPage* region);
    // Put a region the forward path finished with in place back where a collection-set builder
    // will find it; CompactRegion leaves it thread-local, which no builder walks.
    void RehomeCompactedInPlaceRegion(ZPage* region);
    void CompactRegion(ZPage* region);

    void ExemptFromRegion(ZPage* region);
    // Rehome onto unmovableFrom without publishing kept. PrepareYoung parks
    // leftover from-pages here; they were expired at cycle start and must not
    // be re-published as this cycle's done (zRelocationSetSelector.cpp:114-196).
    void ParkUnmovableFromRegion(ZPage* region);
    // ZGC zRelocationSetSelector.cpp:114-196 / zGeneration.cpp:205-213: a page
    // not in this cycle's relocation set is an ordinary candidate next cycle.
    // Kept (IsForwardingDone via Exempt) is in-cycle only.
    // zRelocate.cpp:1041-1047: relocate() returns only after every page in the
    // relocation set is done. Finish every ROUTED page or publish it kept.
    void FinishIncompleteFromRegions(ZGenerationId generation);
    // zRelocate.cpp:1346-1352 flip_survived: keep the page, reset age, leave young.
    // Must not remain LONE_FROM / FROM after TakeHead — barriers treat those as from-space.
    void EnlistStayYoungSurvivor(ZPage* region, bool advanceAge = true);
    static void BumpYoungSurvivorAge(ZPage* region);
    static void FinishStayYoungInPlace(ZPage* region, bool advanceAge = true);

    // ZGeneration::select_relocation_set iterates only pages owned by that
    // generation (zGeneration.cpp:195-221).  An old relocation pass may
    // observe a young page in our shared list, but it must not relocate or
    // promote it using the old mark view.
    static constexpr bool GenerationMayRelocateYoung(Generation generation)
    {
        return generation == Generation::Young;
    }


    void DumpRegionStats(const char* msg) const;

    uintptr_t GetInactiveZone() const { return inactiveZone; }

#if defined(__EULER__)
    double GetCacheRatio() const { return cacheRatio; }
#endif

    uintptr_t GetRegionHeapStart() const { return regionHeapStart; }
    uintptr_t GetRegionHeapEnd() const { return regionHeapEnd; }

    ~RegionManager();

    // take a region with *num* units for allocation
    // allowSaferegion=false: best-effort, never enter saferegion (ROUTING critical section).
    ZPage* TakeRegion(size_t num, ZPageType, bool expectPhysicalMem = false,
                           bool allowSaferegion = true, bool clearPayload = true, PageAge age = PageAge::old, ZAllocationFlags flags = {});


    uintptr_t AllocPinned(size_t size);

    // caller assures size is truely large (> region size)





    void RestoreToSpaceStateWords();

    void CountLiveObject(const BaseObject* obj);

    void AssembleSmallGarbageCandidates();
    void AssembleLargeGarbageCandidates();
    void AssemblePinnedGarbageCandidates(bool collectAll);
    YoungCollectionStats PrepareYoungGarbageCandidates(const std::function<void(ZPage*)>& visitor);

    void CollectFromSpaceGarbage();


    size_t GetYoungAllocatedSize() const;

    template<Generation G>
    size_t CollectRegion(ZPage* region);

    void ReclaimRegion(ZPage* region);
    // Like ReclaimRegion but units enter mark-quarantine tree, not dirty tree.
    void ReclaimRegionToMarkQuarantine(ZPage* region);
    size_t ReleaseRegion(ZPage* region);

    void ReclaimGarbageRegions();

    size_t CollectLargeGarbage();

    size_t CollectPinnedGarbage();
    size_t CollectFreePinnedSlots(ZPage* region);

    // Ignore dynamic pinned regions and from regions whose garbage objects are quite few, return the garbage size that
    // can be reclaimed.
    size_t ExemptFromRegions();
    // ZGC zGeneration.cpp:211-213: drop is_allocating pages at CSet select (pre-flip).

    void ForEachObjUnsafe(const std::function<void(BaseObject*)>& visitor,
                          bool skipKnownEmptyRegions = false) const;
    void ForEachObjSafe(const std::function<void(BaseObject*)>& visitor) const;

    size_t GetUsedRegionSize() const { return GetUsedBytes(); }

    size_t GetRecentAllocatedSize() const;
    size_t GetSurvivedSize() const;
    size_t GetFromSpaceSize() const;
    size_t GetPinnedSpaceSize() const;
    size_t SumAllocatedByRoles(std::initializer_list<ZPageRole> roles) const;

    size_t GetUsedBytes() const;

    size_t GetCachedBytes() const { return freeRegionManager.GetCachedBytes(); }
    // Address space not yet backed by committed capacity (ZGC: current_max_capacity - capacity).
    size_t GetUncommittedBytes() const { return GetHeapCapacity() - GetCommittedCapacity(); }

    size_t GetCommittedBytes() const { return GetCommittedCapacity(); }

    // Diagnostic total over large pages, from the page table (no page list).
    size_t GetLargeObjectSize() const;

    size_t GetAllocatedSize() const;

    // zPageAllocator.cpp:1363-1373 — snapshot feeding ZStatHeap. Stats reads;
    // UpdateAndStats first resets the per-collection used high/low trackers
    // (zPageAllocator.cpp:1332-1346 update_collection_stats, called at mark
    // start). Generation is needed for the per-generation used/freed/promoted/
    // compacted fields.
    ZPageAllocatorStats Stats(const ZGeneration* generation) const;
    ZPageAllocatorStats UpdateAndStats(const ZGeneration* generation);
    void UpdateCollectionStats(ZGenerationId id);
    void increase_used_generation(ZGenerationId id, size_t size)
    {
        usedPerGeneration[id == ZGenerationId::young ? 0 : 1].fetch_add(size, std::memory_order_relaxed);
    }
    void decrease_used_generation(ZGenerationId id, size_t size)
    {
        usedPerGeneration[id == ZGenerationId::young ? 0 : 1].fetch_sub(size, std::memory_order_relaxed);
    }
    void NoteUsedGenerationDelta(Generation generation, ssize_t delta)
    {
        const ZGenerationId id = generation == Generation::Young ? ZGenerationId::young : ZGenerationId::old;
        if (delta >= 0) {
            increase_used_generation(id, static_cast<size_t>(delta));
        } else {
            decrease_used_generation(id, static_cast<size_t>(-delta));
        }
    }
    size_t used_generation(ZGenerationId id) const
    {
        return usedPerGeneration[id == ZGenerationId::young ? 0 : 1].load(std::memory_order_relaxed);
    }
    size_t UsedGeneration(ZGenerationId id) const { return used_generation(id); }


    void ClearFreePinnedSlots() { freePinnedSlotLists.Clear(); }

    // wait for a period of time to allocate region which will avoid harm to gc
    void RequestForRegion(size_t size);

    // Pacing inputs of RequestForRegion, published at the end of each
    // non-young collection (post-major live bytes and collection rate).
    void SetLastCollectionStats(size_t liveBytes, double rate)
    {
        lastLiveBytesAfterGC.store(liveBytes, std::memory_order_release);
        lastCollectionRate.store(rate, std::memory_order_release);
    }


    void SetGarbageThreshold(double garbageThreshold);

    void HandleTraceRegions();
    // Stamps `role` on the page when the matching trace cache is active.
    bool TryStampTraceRegion(ZPage* region, ZPageRole role);

    void PrepareTrace();


    bool RelocateClaimedPage(ZPage* region);





    // Release point for OPTION_2 mark-epoch gate: major PostTrace after PrepareForwardTable.
    // Concurrent mark (TRACE+CLEAR_SATB) has finished; plain strong refs into quarantined
    // ranges are no longer traced. Safe to publish units to dirty tree for ClearPageMemory reuse.
    // Note: this major's just-installed quarantine (from PrepareForwardTable above) is also
    // released here — mark is already done, so no TRACE can race those units. Units held from
    // prior minor PrepareForwardTable are the ones that covered the TRACE window.
    void ReleaseMarkQuarantine();


    // Probe-only: visit every region on managed lists with its list name (tag-reuse scan).
    template <typename F>
    void VisitAllManagedRegionsForProbe(F&& visitor);

private:
    // zPageAllocator.cpp:2248-2266: consumed by safe retirement after the
    // page table no longer publishes the old descriptor.
    void ReclaimRetiredRegion(ZPage* region);
    void ReclaimRetiredRegionToMarkQuarantine(ZPage* region);
    void ReleaseRetiredRegion(ZPage* region);
    void ReturnRetiredPageMemory(const PageMemory& memory, bool allowSaferegion = true);



    ZPage* TakeReclaimableGarbageRegion(size_t* gatedBytes = nullptr);

    bool TryTakeGarbageRegionAfterDispel(ZPage* target);

    size_t GetGatedGarbageBytes();

    // Acquire a region list mutex which the collector also takes while the world is stopped.
    // Waiting for it in a saferegion is required so that a contended mutator cannot stall
    // StopTheWorld (MutatorManager.cpp:485-490), but the mutex must never be owned while the
    // saferegion guard is destroyed: LeaveSaferegion() parks the mutator in SuspendForSync()
    // (Mutator.h:172-186, Mutator.cpp:229-280) and the collector would then wait for that mutex
    // forever. Wait in try-lock rounds so every saferegion transition happens unlocked, exactly
    // as FreeRegionManager::TakeRegion() does for the free unit trees (FreeRegionManager.h:45-92).
    static void LockPageMutexInSaferegion(std::mutex& listMutex);

    // caller must own the pinned allocation mutex, and must not release it in between.
    uintptr_t AllocPinnedLocked(size_t size);

    inline void CheckRegionWhetherCreatedInFixPhase(ZPage* region);

    ZPage* AllocateSharedPage(size_t size, ZPageType role, PageAge age, ZAllocationFlags flags, bool clearPayload = true);
    void UndoSharedPage(ZPage* page);

    MAddress reservedStart = 0;
    MAddress reservedEnd = 0;
    struct MetadataMapping {
        void* base { nullptr };
        size_t size { 0 };
        ~MetadataMapping();
    } metadata;
    // Declared before the caches: backing/address resources are destroyed
    // only after their page/cache entries, as in ZPageAllocator.
    std::unique_ptr<ZVirtualMemoryManager> virtualMemory;
    std::unique_ptr<ZPhysicalMemoryManager> physicalMemory;
    FreeRegionManager freeRegionManager;

    // #710: page lifecycle identity lives in ZPage's role word and the page
    // table (zPageTable.hpp:57-77); there are no page lists. The relocation
    // set (zRelocationSet.hpp) is the from-space work source.
    ZRelocateQueue relocateQueue;
    // Serializes pinned-page installation with retirement/seqnum (P14), and
    // pinned TLAB staging handoff.
    std::mutex pinnedAllocationMutex;
    // RegionCache activations (PrepareTrace/HandleTraceRegions): while active,
    // freshly filled pages are stamped FullTrace/LargeTrace instead of
    // RecentFull/RecentLarge.
    bool fullTraceCacheActive{ false };
    bool largeTraceCacheActive{ false };
    ZWorkers* relocationWorkers{ nullptr };
    bool relocationStarted{ false };
    bool relocationDrained{ false };
    // zRelocate.cpp:1121 shape: in-place relocated page counts by size class,
    // accumulated while a from-space pass runs and read at its end. Host
    // difference: ZGC counts these on ZRelocateSmall/MediumAllocator; here the
    // in-place decision is made inside RegionManager::ForwardClaimedPage /
    // RelocateClaimedPage, so the counters live with that driver.
    std::atomic<size_t> inPlaceSmallCount{ 0 };
    std::atomic<size_t> inPlaceMediumCount{ 0 };
    // zPageAllocator.hpp:157-162 shape: per-generation used (region-granular)
    // and per-collection used high/low, updated at the pageAllocatorUsed
    // mutation points.
    std::atomic<size_t> usedPerGeneration[2]{};
    size_t collectionUsedHigh[2]{ 0, 0 };
    size_t collectionUsedLow[2]{ 0, 0 };
    void TrackUsedPeakLocked()
    {
        for (size_t i = 0; i < 2; ++i) {
            if (pageAllocatorUsed > collectionUsedHigh[i]) { collectionUsedHigh[i] = pageAllocatorUsed; }
            if (pageAllocatorUsed < collectionUsedLow[i]) { collectionUsedLow[i] = pageAllocatorUsed; }
        }
    }
    // zPageAllocator.cpp:1518: ordinary allocation and stall share one owner.
    friend class Uncommitter;
    std::mutex pageAllocatorMutex;
    AllocationStallQueue allocationStallQueue{ pageAllocatorMutex };
    size_t pageAllocatorUsed{ 0 };
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
#endif
    uintptr_t regionHeapStart = 0; // the address of first region to allocate object
    uintptr_t regionHeapEnd = 0;

    // the time when previous region was allocated, which is assigned with returned value by timeutil::NanoSeconds().
    std::atomic<uint64_t> prevRegionAllocTime = { 0 };
    std::atomic<size_t> lastLiveBytesAfterGC{ 0 };
    std::atomic<double> lastCollectionRate{ 0.0 };

    // heap space not allocated yet for even once. this value should not be decreased.
    std::atomic<uintptr_t> inactiveZone = { 0 }; // highest handed-out address, diagnostic envelope only
    size_t heapCapacity = 0;
    std::atomic<size_t> tlabUsed{ 0 };
    size_t lastTLABUsed = 0;
    double tlabCapacity = 0;
    TLABAllocationAverage tlabAllocatingThreads;
    TLABAllocationAverage tlabRequestedFraction;
    std::mutex tlabStatisticsLock;
    TLABStatistics retiredTLABStatistics;

    double fromSpaceGarbageThreshold = 0.5; // 0.5: default garbage ratio.
    double exemptedRegionThreshold;
#if defined(__EULER__)
    double cacheRatio;
#endif
    std::mutex freePinnedSlotListMutex;
    FreePinnedSlotLists freePinnedSlotLists;
};

} // namespace MapleRuntime

#include "Heap/z/zPageAllocator.inline.hpp"
#include "Heap/z/zRelocate.hpp"
#endif // MRT_REGION_MANAGER_H
