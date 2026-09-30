// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_ALLOCATION_STALL_QUEUE_H
#define MRT_ALLOCATION_STALL_QUEUE_H

#include "Heap/z/zStat.hpp"
#include "Heap/z/zPageAge.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>
#include "Heap/z/zFuture.hpp"
#include "Heap/z/zList.inline.hpp"
#include "Heap/z/zVirtualMemoryManager.hpp"
#include "Heap/z/zArray.inline.hpp"


#include "Heap/z/zAllocationFlags.hpp"

namespace MapleRuntime {

// ZGC zPageAllocator.cpp:85-296. Single-partition allocation state; NUMA
// multi-partition composition remains in A03n.
class ZPartition;
class ZMemoryAllocation {
    const size_t _size;
    ZPartition* _partition{nullptr};
    ZVirtualMemory _satisfied_from_cache_vmem;
    ZArray<ZVirtualMemory> _partial_vmems;
    int _num_harvested{0};
    size_t _harvested{0};
    size_t _increased_capacity{0};
    size_t _committed_capacity{0};
    bool _commit_failed{false};
public:
    explicit ZMemoryAllocation(size_t size) : _size(size) {}
    void reset_for_retry()
    {
        CHECK(_satisfied_from_cache_vmem.is_null());
        _partition = nullptr;
        _partial_vmems.clear();
        _num_harvested = 0;
        _harvested = 0;
        _increased_capacity = 0;
        _committed_capacity = 0;
        _commit_failed = false;
    }
    size_t size() const { return _size; }
    ZPartition& partition() const { CHECK(_partition != nullptr); return *_partition; }
    void set_partition(ZPartition* partition) { CHECK(_partition == nullptr); _partition = partition; }
    ZVirtualMemory satisfied_from_cache_vmem() const { return _satisfied_from_cache_vmem; }
    void set_satisfied_from_cache_vmem(ZVirtualMemory vmem)
    {
        CHECK(_satisfied_from_cache_vmem.is_null() && _partial_vmems.is_empty());
        CHECK(vmem.size() == size());
        _satisfied_from_cache_vmem = vmem;
    }
    void set_satisfied_from_cache_vmem_fast_medium(ZVirtualMemory vmem)
    {
        CHECK(_satisfied_from_cache_vmem.is_null() && _partial_vmems.is_empty());
        CHECK(ZPageSizeMediumEnabled && vmem.size() >= ZPageSizeMediumMin && vmem.size() <= ZPageSizeMediumMax);
        CHECK((vmem.size() & (vmem.size() - 1)) == 0);
        _satisfied_from_cache_vmem = vmem;
    }
    ZArray<ZVirtualMemory>* partial_vmems() { return &_partial_vmems; }
    const ZArray<ZVirtualMemory>* partial_vmems() const { return &_partial_vmems; }
    int num_harvested() const { return _num_harvested; }
    size_t harvested() const { return _harvested; }
    void set_harvested(int count, size_t size) { _num_harvested = count; _harvested = size; }
    size_t increased_capacity() const { return _increased_capacity; }
    void set_increased_capacity(size_t size) { _increased_capacity = size; }
    size_t committed_capacity() const { return _committed_capacity; }
    void set_committed_capacity(size_t size)
    {
        CHECK(_committed_capacity == 0);
        _committed_capacity = size;
        _commit_failed = size != _increased_capacity;
    }
    bool commit_failed() const { return _commit_failed; }
};

class ZSinglePartitionAllocation {
    ZMemoryAllocation _allocation;
public:
    explicit ZSinglePartitionAllocation(size_t size) : _allocation(size) {}
    size_t size() const { return _allocation.size(); }
    ZMemoryAllocation* allocation() { return &_allocation; }
    const ZMemoryAllocation* allocation() const { return &_allocation; }
    void reset_for_retry() { _allocation.reset_for_retry(); }
};

// One object represents one blocked allocation.  It is deliberately owned by
// the allocator caller; the queue only retains the pointer until a terminal
// answer is published.
class ZPageAllocation {
public:
    ZPageAllocation(size_t size, uint8_t role, PageAge age, ZAllocationFlags flags = {});
    ZPageAllocation(const ZPageAllocation&) = delete;
    ZPageAllocation& operator=(const ZPageAllocation&) = delete;

    size_t GetSize() const
    {
        const ZVirtualMemory cached = singleAllocation.allocation()->satisfied_from_cache_vmem();
        return flags.fast_medium() && !cached.is_null() ? cached.size() : size;
    }
    uint32_t YoungSeqnum() const { return youngSeqnum; }
    uint32_t OldSeqnum() const { return oldSeqnum; }
    ZAllocationFlags Flags() const { return flags; }
    uint8_t GetRole() const { return role; }
    PageAge Age() const { return age; }
    ZSinglePartitionAllocation* single_partition_allocation() { return &singleAllocation; }
    const ZSinglePartitionAllocation* single_partition_allocation() const { return &singleAllocation; }
    void reset_for_retry() { singleAllocation.reset_for_retry(); }

    // zPageAllocator.cpp:525-531 ZPageAllocation::wait/satisfy over ZFuture<bool>.
    bool Wait();
    void Satisfy(bool value);

private:
    friend class RegionManager;
    friend class ZList<ZPageAllocation>;

    const size_t size;
    const uint32_t youngSeqnum;
    const uint32_t oldSeqnum;
    const uint8_t role;
    const PageAge age;
    const ZAllocationFlags flags;
    ZSinglePartitionAllocation singleAllocation;
    // zPageAllocator.cpp:420-421 ZPageAllocation: ZFuture<bool> _stall_result
    // and the ZListNode that links it on the allocator's stalled list.
    ZFuture<bool> stallResult;
    ZListNode<ZPageAllocation> _node;
};
using AllocationStallRequest = ZPageAllocation;

} // namespace MapleRuntime

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

// ZGC zPageAllocator.hpp:58-137: partition accounting and thin memory operations.
class ZPartition {
public:
    RegionManager& regionManager;
    uint32_t numaId;
    ZMappedCache cache;
    Uncommitter uncommitter;
    size_t minCapacity{0};
    std::atomic<size_t> capacity{0};
    std::atomic<size_t> claimed{0};
    size_t used{0};
    std::atomic<size_t> currentMaxCapacity{0};
    ZPartition(uint32_t id, RegionManager& manager)
        : regionManager(manager), numaId(id), uncommitter(*this) {}
    size_t available() const { return currentMaxCapacity.load() - used - claimed.load(); }
    size_t increase_capacity(size_t size);
    void decrease_capacity(size_t size, bool set_max_capacity);
    void free_memory(const ZVirtualMemory& vmem);
    void claim_from_cache_or_increase_capacity(ZMemoryAllocation* allocation);
    bool claim_capacity(ZMemoryAllocation* allocation);
    bool claim_capacity_fast_medium(ZMemoryAllocation* allocation);
    bool prime(size_t size);
    ZVirtualMemory claim_virtual(size_t size);
    size_t claim_virtual(size_t size, ZArray<ZVirtualMemory>* out);
    void free_virtual(const ZVirtualMemory& vmem);
    ZVirtualMemory free_and_claim_virtual_from_low_exact_or_many(size_t size, ZArray<ZVirtualMemory>* out);
    void claim_physical(const ZVirtualMemory& vmem);
    void free_physical(const ZVirtualMemory& vmem);
    size_t commit_physical(const ZVirtualMemory& vmem);
    size_t uncommit_physical(const ZVirtualMemory& vmem);
    void map_virtual(const ZVirtualMemory& vmem);
    void unmap_virtual(const ZVirtualMemory& vmem);
    void sort_segments_physical(const ZVirtualMemory& vmem);
    ZVirtualMemory prepare_harvested_and_claim_virtual(ZMemoryAllocation* allocation);
    void commit_increased_capacity(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem);
    void map_memory(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem);
    void free_memory_alloc_failed(ZMemoryAllocation* allocation);
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

namespace MapleRuntime {
class CompactCollector;
template<Generation G>
class ForwardTask;





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
    size_t GetCommittedCapacity() const { return capacity(); }
    void StartUncommitters();
    void StopUncommitters();

    size_t GetHeapCapacity() const { return heapCapacity; }
    size_t soft_max_capacity() const;


    // zPageAllocator.cpp:1201-1260: page resource ownership belongs to
    // this allocator, not to its object-allocation consumers.
    MAddress GetSpaceStartAddress() const { return reservedStart; }
    MAddress GetSpaceEndAddress() const { return reservedEnd; }

    bool is_initialized() const { return _initialized; }
    bool PrimeCache(size_t size);
    RegionManager();
    RegionManager(const HeapParam& param, double garbageThreshold);

    RegionManager(const RegionManager&) = delete;

    RegionManager& operator=(const RegionManager&) = delete;



    // ZObjectAllocator::alloc / alloc_for_relocation. These pages never belong
    // to an AllocBuffer: a thread's TLAB and a CPU's shared page are distinct.

    // ZHeap::account_alloc_page/account_undo_alloc_page: backing extents,
    // independent of the thread-local requested bytes and retirement waste.

    // Stable cycle history: read under the statistics lock, at a safepoint,
    // or with managed access preventing the next young pause.
    void InitializeTLAB(AllocBuffer& buffer);
    void PublishTLABStatistics(const TLABStatistics& statistics);
    void RetireTLAB(AllocBuffer& buffer, TLABStatistics& statistics);

    bool StallAllocation(AllocationStallRequest& request);
    bool ClaimCapacityOrStall(AllocationStallRequest& request);
    bool ClaimAllocationLocked(AllocationStallRequest& request);
    void ReturnPageMemory(const ZVirtualMemory& memory);
    bool IsAllocationStalling() const;
    bool IsAllocationStallingForOld() const;
    void HandleAllocStallingForYoung();
    void StopStalledAllocations();
    void HandleAllocStallingForOld(bool clearedAllSoftRefs);
    size_t AllocationStallsNow() const;
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

    // ZGeneration::select_relocation_set iterates only pages owned by that
    // generation (zGeneration.cpp:195-221).  An old relocation pass may
    // observe a young page in our shared list, but it must not relocate or
    // promote it using the old mark view.
    static constexpr bool GenerationMayRelocateYoung(Generation generation)
    {
        return generation == Generation::Young;
    }


    void DumpRegionStats(const char* msg) const;


#if defined(__EULER__)
    double GetCacheRatio() const { return cacheRatio; }
#endif

    uintptr_t GetRegionHeapStart() const { return regionHeapStart; }
    uintptr_t GetRegionHeapEnd() const { return regionHeapEnd; }

    ~RegionManager();

    // take a region with *num* units for allocation
    ZPage* TakeRegion(size_t num, ZPageType, PageAge age = PageAge::old, ZAllocationFlags flags = {});



    // caller assures size is truely large (> region size)





    void RestoreToSpaceStateWords();




    size_t GetYoungAllocatedSize() const;


    void ReclaimRegion(ZPage* region);
    size_t ReleaseRegion(ZPage* region);




    // ZGC zGeneration.cpp:211-213: drop is_allocating pages at CSet select (pre-flip).

    size_t GetUsedRegionSize() const { return GetUsedBytes(); }

    size_t GetRecentAllocatedSize() const;
    size_t GetSurvivedSize() const;
    size_t GetFromSpaceSize() const;
    size_t SumAllocatedByRoles(std::initializer_list<ZPageRole> roles) const;

    size_t GetUsedBytes() const;

    size_t GetCachedBytes() const;
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



    void SetGarbageThreshold(double garbageThreshold);










private:
    // zPageAllocator.cpp:2248-2266: consumed by safe retirement after the
    // page table no longer publishes the old descriptor.
    void ReclaimRetiredRegion(ZPage* region);
    void ReleaseRetiredRegion(ZPage* region);
    void ReturnRetiredPageMemory(const ZVirtualMemory& memory);






    inline void CheckRegionWhetherCreatedInFixPhase(ZPage* region);

    ZPage* AllocateSharedPage(size_t size, ZPageType role, PageAge age, ZAllocationFlags flags);

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
    friend class ZPartition;
    std::vector<std::unique_ptr<ZPartition>> partitions;
    size_t nextPartition{0}; // A03n: preferred NUMA routing is deferred.
    void InitializePartitions(size_t maxCapacity);
    void PrintCacheOn() const;
    bool claim_capacity(ZPageAllocation* allocation);
    bool claim_capacity_fast_medium(ZPageAllocation* allocation);
    ZPage* alloc_page_inner(ZPageAllocation* allocation);
    ZVirtualMemory satisfied_from_cache_vmem(const ZPageAllocation* allocation) const;
    ZVirtualMemory claim_virtual_memory(ZPageAllocation* allocation);
    ZVirtualMemory claim_virtual_memory_single_partition(ZSinglePartitionAllocation* allocation);
    void claim_physical_for_increased_capacity(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem);
    bool commit_and_map(ZPageAllocation* allocation, const ZVirtualMemory& vmem);
    bool commit_and_map_single_partition(ZSinglePartitionAllocation* allocation, const ZVirtualMemory& vmem);
    void commit(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem);
    bool commit_single_partition(ZSinglePartitionAllocation* allocation, const ZVirtualMemory& vmem);
    void map_committed_single_partition(ZSinglePartitionAllocation* allocation, const ZVirtualMemory& vmem);
    void cleanup_failed_commit_single_partition(ZSinglePartitionAllocation* allocation, const ZVirtualMemory& vmem);
    void free_after_alloc_page_failed(ZPageAllocation* allocation);
    void free_memory_alloc_failed(ZPageAllocation* allocation);
    void free_memory_alloc_failed_single_partition(ZSinglePartitionAllocation* allocation);
    void free_memory_alloc_failed(ZMemoryAllocation* allocation);
    ZPage* create_page(ZPageAllocation* allocation, const ZVirtualMemory& vmem);
    ZPageAllocatorStats StatsInner(const ZGeneration* generation) const;
public:
    size_t capacity() const;
    size_t current_max_capacity() const;
    static ZVirtualMemory VirtualMemoryOf(size_t index, size_t count);
    static size_t IndexOf(const ZVirtualMemory& vmem);
private:

    // #710: page lifecycle identity lives in ZPage's role word and the page
    // table (zPageTable.hpp:57-77); there are no page lists. The relocation
    // set (zRelocationSet.hpp) is the from-space work source.
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
    mutable std::mutex pageAllocatorMutex;
    ZList<ZPageAllocation> stalled;
    bool stallClosed{false};
    bool _initialized{false};
    void SatisfyStalledAllocations();
    void NotifyOutOfMemory();
    void RestartGC() const;
    std::atomic<size_t> pageAllocatorUsed{ 0 };

    uintptr_t regionHeapStart = 0; // the address of first region to allocate object
    uintptr_t regionHeapEnd = 0;

    size_t heapCapacity = 0;
    TLABAllocationAverage tlabAllocatingThreads;
    TLABAllocationAverage tlabRequestedFraction;

    double fromSpaceGarbageThreshold = 0.5; // 0.5: default garbage ratio.
    double exemptedRegionThreshold;
#if defined(__EULER__)
    double cacheRatio;
#endif
};

} // namespace MapleRuntime

#include "Heap/z/zPageAllocator.inline.hpp"
#include "Heap/z/zRelocate.hpp"
#endif // MRT_REGION_MANAGER_H
