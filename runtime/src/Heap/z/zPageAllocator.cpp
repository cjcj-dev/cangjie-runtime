// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "RuntimeConfig.h"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zFuture.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/concurrentGCThread.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zNUMA.inline.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/shared/collectedHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
static const ZStatCriticalPhase ZCriticalPhaseAllocationStall("Allocation Stall");




void RegionManager::InitializePartitions(size_t maxCapacity)
{
    partitions.clear();
    nextPartition = 0;
    for (uint32_t id = 0; id < ZPerNUMAStorage::count(); ++id) {
        partitions.emplace_back(new ZPartition(id, *this));
        partitions.back()->currentMaxCapacity = NumaTopology::calculate_share(id, maxCapacity, ZGranuleSize);
    }
}

// ZGC zPageAllocator.cpp:953-994. Construction owns the cache exclusively.
bool ZPartition::prime(size_t size)
{
    if (size == 0) { return true; }
    ZArray<ZVirtualMemory> vmems;
    const size_t claimedSize = claim_virtual(size, &vmems);
    CHECK(claimedSize == size);
    increase_capacity(claimedSize);
    for (const ZVirtualMemory vmem : vmems) {
        claim_physical(vmem);
        if (commit_physical(vmem) != vmem.size()) { return false; }
        map_virtual(vmem);
        cache.insert(vmem);
    }
    return true;
}

bool RegionManager::PrimeCache(size_t size)
{
    for (const auto& partition : partitions) {
        if (!partition->prime(NumaTopology::calculate_share(partition->numaId, size, ZGranuleSize))) { return false; }
    }
    return true;
}

void RegionManager::StartUncommitters()
{
    for (auto& partition : partitions) { partition->uncommitter.Start(); }
}

void RegionManager::StopUncommitters()
{
    for (auto& partition : partitions) { partition->uncommitter.Stop(); }
}

ZVirtualMemory RegionManager::VirtualMemoryOf(size_t index, size_t count)
{
    return ZVirtualMemory(ZAddress::offset(to_zaddress_unsafe(ZPage::GranuleAddress(index))), count);
}

size_t RegionManager::IndexOf(const ZVirtualMemory& vmem)
{
    const size_t index = ZPage::GranuleIndex(untype(ZOffset::address_unsafe(vmem.start())));
    CHECK(index != std::numeric_limits<uint32_t>::max());
    return index;
}

// ZGC zPageAllocator.cpp:790-920: thin operations belong to the partition.
ZVirtualMemory ZPartition::claim_virtual(size_t size)
{
    return regionManager.virtualMemory->remove_from_low(size, numaId);
}

size_t ZPartition::claim_virtual(size_t size, ZArray<ZVirtualMemory>* out)
{
    return regionManager.virtualMemory->remove_from_low_many_at_most(size, numaId, out);
}

void ZPartition::free_virtual(const ZVirtualMemory& vmem)
{
    regionManager.virtualMemory->insert(vmem, numaId);
}

ZVirtualMemory ZPartition::free_and_claim_virtual_from_low_exact_or_many(size_t size, ZArray<ZVirtualMemory>* out)
{
    return regionManager.virtualMemory->insert_and_remove_from_low_exact_or_many(size, numaId, out);
}

void ZPartition::claim_physical(const ZVirtualMemory& vmem) { regionManager.physicalMemory->alloc(vmem, numaId); }
void ZPartition::free_physical(const ZVirtualMemory& vmem) { regionManager.physicalMemory->free(vmem, numaId); }
size_t ZPartition::commit_physical(const ZVirtualMemory& vmem) { return regionManager.physicalMemory->commit(vmem, numaId); }
size_t ZPartition::uncommit_physical(const ZVirtualMemory& vmem) { return regionManager.physicalMemory->uncommit(vmem); }
void ZPartition::map_virtual(const ZVirtualMemory& vmem) { regionManager.physicalMemory->map(vmem, numaId); }
void ZPartition::unmap_virtual(const ZVirtualMemory& vmem) { regionManager.physicalMemory->unmap(vmem); }
void ZPartition::sort_segments_physical(const ZVirtualMemory& vmem)
{
    regionManager.physicalMemory->sort_segments_physical(vmem);
}

// ZGC zPageAllocator.cpp:648-699: single writer under allocator lock, atomic readers.
size_t ZPartition::increase_capacity(size_t size)
{
    const size_t increased = std::min(size, currentMaxCapacity.load() - capacity.load());
    if (increased > 0) {
        capacity.fetch_add(increased);
        uncommitter.Cancel();
    }
    return increased;
}

void ZPartition::decrease_capacity(size_t size, bool set_max_capacity)
{
    CHECK(capacity.load() >= size);
    capacity.fetch_sub(size);
    if (set_max_capacity) {
        VLOG(REPORT, "Forced to lower max partition (%u) capacity from %zuM to %zuM", numaId,
             currentMaxCapacity.load() / MB, capacity.load() / MB);
        currentMaxCapacity.store(capacity.load());
    }
}

size_t RegionManager::current_max_capacity() const
{
    size_t total = 0;
    for (const auto& partition : partitions) { total += partition->currentMaxCapacity.load(); }
    return total;
}

size_t RegionManager::capacity() const
{
    size_t total = 0;
    for (const auto& partition : partitions) { total += partition->capacity.load(); }
    return total;
}

void ZPartition::free_memory(const ZVirtualMemory& vmem)
{
    cache.insert(vmem);
    CHECK(used >= vmem.size());
    used -= vmem.size();
}

// ZGC zPageAllocator.cpp:702-762.
void ZPartition::claim_from_cache_or_increase_capacity(ZMemoryAllocation* allocation)
{
    const size_t size = allocation->size();
    CHECK(available() >= size);
    allocation->set_partition(this);
    const ZVirtualMemory vmem = cache.remove_contiguous(size);
    if (!vmem.is_null()) {
        allocation->set_satisfied_from_cache_vmem(vmem);
        return;
    }
    const size_t increased = increase_capacity(size);
    allocation->set_increased_capacity(increased);
    if (increased == size) { return; }
    const size_t harvested = cache.remove_discontiguous(size - increased, allocation->partial_vmems());
    allocation->set_harvested(allocation->partial_vmems()->length(), harvested);
    CHECK(harvested + increased == size);
}

bool ZPartition::claim_capacity(ZMemoryAllocation* allocation)
{
    if (available() < allocation->size()) { return false; }
    claim_from_cache_or_increase_capacity(allocation);
    used += allocation->size();
    return true;
}

bool ZPartition::claim_capacity_fast_medium(ZMemoryAllocation* allocation)
{
    CHECK(ZPageSizeMediumEnabled);
    const ZVirtualMemory vmem = cache.remove_contiguous_power_of_2(ZPageSizeMediumMin, ZPageSizeMediumMax);
    if (vmem.is_null()) { return false; }
    allocation->set_satisfied_from_cache_vmem_fast_medium(vmem);
    allocation->set_partition(this);
    used += vmem.size();
    return true;
}

// ZGC zPageAllocator.cpp:1544-1595. NUMA preferred routing remains A03n.
bool RegionManager::claim_capacity(ZPageAllocation* allocation)
{
    if (allocation->Flags().fast_medium()) { return claim_capacity_fast_medium(allocation); }
    for (size_t visited = 0; visited < partitions.size(); ++visited) {
        const size_t selected = (nextPartition + visited) % partitions.size();
        if (partitions[selected]->claim_capacity(allocation->single_partition_allocation()->allocation())) {
            nextPartition = (selected + 1) % partitions.size();
            return true;
        }
    }
    return false;
}

bool RegionManager::claim_capacity_fast_medium(ZPageAllocation* allocation)
{
    for (size_t visited = 0; visited < partitions.size(); ++visited) {
        const size_t selected = (nextPartition + visited) % partitions.size();
        if (partitions[selected]->claim_capacity_fast_medium(allocation->single_partition_allocation()->allocation())) {
            nextPartition = (selected + 1) % partitions.size();
            return true;
        }
    }
    return false;
}

// ZGC zPageAllocator.cpp:1001-1046: no allocator lock around memory operations.
ZVirtualMemory ZPartition::prepare_harvested_and_claim_virtual(ZMemoryAllocation* allocation)
{
    for (const ZVirtualMemory vmem : *allocation->partial_vmems()) { unmap_virtual(vmem); }
    const size_t harvested = allocation->harvested();
    ZArray<zbacking_index> stash;
    regionManager.physicalMemory->stash_segments(*allocation->partial_vmems(), &stash);
    const ZVirtualMemory result = free_and_claim_virtual_from_low_exact_or_many(allocation->size(), allocation->partial_vmems());
    if (!result.is_null()) {
        regionManager.physicalMemory->restore_segments(result.first_part(harvested), stash);
    } else {
        regionManager.physicalMemory->restore_segments(*allocation->partial_vmems(), stash);
    }
    if (result.is_null()) {
        for (const ZVirtualMemory vmem : *allocation->partial_vmems()) { map_virtual(vmem); }
    }
    return result;
}

ZVirtualMemory RegionManager::satisfied_from_cache_vmem(const ZPageAllocation* allocation) const
{
    return allocation->single_partition_allocation()->allocation()->satisfied_from_cache_vmem();
}

ZVirtualMemory RegionManager::claim_virtual_memory(ZPageAllocation* allocation)
{
    return claim_virtual_memory_single_partition(allocation->single_partition_allocation());
}

ZVirtualMemory RegionManager::claim_virtual_memory_single_partition(ZSinglePartitionAllocation* single)
{
    ZMemoryAllocation* const allocation = single->allocation();
    ZPartition& partition = allocation->partition();
    return allocation->harvested() > 0 ? partition.prepare_harvested_and_claim_virtual(allocation)
                                     : partition.claim_virtual(allocation->size());
}

// ZGC zPageAllocator.cpp:1762-1780.
void RegionManager::claim_physical_for_increased_capacity(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem)
{
    const size_t alreadyCommitted = allocation->harvested();
    const size_t nonCommitted = allocation->size() - alreadyCommitted;
    CHECK(nonCommitted == allocation->increased_capacity());
    if (nonCommitted > 0) { allocation->partition().claim_physical(vmem.last_part(alreadyCommitted)); }
}

// ZGC zPageAllocator.cpp:1058-1070.
void ZPartition::commit_increased_capacity(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem)
{
    CHECK(allocation->increased_capacity() > 0);
    const size_t committed = commit_physical(vmem.last_part(allocation->harvested()));
    allocation->set_committed_capacity(committed);
}

void ZPartition::map_memory(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem)
{
    CHECK(&allocation->partition() == this);
    sort_segments_physical(vmem);
    map_virtual(vmem);
}

bool RegionManager::commit_and_map(ZPageAllocation* allocation, const ZVirtualMemory& vmem)
{
    CHECK(allocation->GetSize() == vmem.size());
    return commit_and_map_single_partition(allocation->single_partition_allocation(), vmem);
}

// ZGC zPageAllocator.cpp:1792-1806.
bool RegionManager::commit_and_map_single_partition(ZSinglePartitionAllocation* single, const ZVirtualMemory& vmem)
{
    const bool success = commit_single_partition(single, vmem);
    map_committed_single_partition(single, vmem);
    if (success) { return true; }
    cleanup_failed_commit_single_partition(single, vmem);
    return false;
}

void RegionManager::commit(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem)
{
    if (allocation->increased_capacity() > 0) { allocation->partition().commit_increased_capacity(allocation, vmem); }
}

bool RegionManager::commit_single_partition(ZSinglePartitionAllocation* single, const ZVirtualMemory& vmem)
{
    commit(single->allocation(), vmem);
    return !single->allocation()->commit_failed();
}

void RegionManager::map_committed_single_partition(ZSinglePartitionAllocation* single, const ZVirtualMemory& vmem)
{
    ZMemoryAllocation* const allocation = single->allocation();
    const size_t total = allocation->harvested() + allocation->committed_capacity();
    const ZVirtualMemory committed = vmem.first_part(total);
    if (committed.size() > 0) { allocation->partition().map_memory(allocation, committed); }
}

// ZGC zPageAllocator.cpp:1906-1932.
void RegionManager::cleanup_failed_commit_single_partition(ZSinglePartitionAllocation* single, const ZVirtualMemory& vmem)
{
    ZMemoryAllocation* const allocation = single->allocation();
    CHECK(allocation->commit_failed());
    CHECK(allocation->partial_vmems()->is_empty());
    const size_t total = allocation->harvested() + allocation->committed_capacity();
    const ZVirtualMemory succeeded = vmem.first_part(total);
    const ZVirtualMemory failed = vmem.last_part(total);
    if (succeeded.size() > 0) { allocation->partial_vmems()->append(succeeded); }
    allocation->partition().free_physical(failed);
    allocation->partition().free_virtual(failed);
}

// ZGC zPageAllocator.cpp:1079-1101.
void ZPartition::free_memory_alloc_failed(ZMemoryAllocation* allocation)
{
    CHECK(&allocation->partition() == this);
    CHECK(used >= allocation->size());
    used -= allocation->size();
    size_t freed = 0;
    for (const ZVirtualMemory vmem : *allocation->partial_vmems()) {
        freed += vmem.size();
        cache.insert(vmem);
    }
    CHECK_DETAIL(allocation->harvested() + allocation->committed_capacity() == freed,
                 "must have freed all: %zu + %zu == %zu", allocation->harvested(), allocation->committed_capacity(), freed);
    const size_t remaining = allocation->size() - freed;
    if (remaining > 0) { decrease_capacity(remaining, allocation->commit_failed()); }
}

void RegionManager::free_memory_alloc_failed(ZMemoryAllocation* allocation)
{
    allocation->partition().free_memory_alloc_failed(allocation);
}

void RegionManager::free_memory_alloc_failed_single_partition(ZSinglePartitionAllocation* single)
{
    free_memory_alloc_failed(single->allocation());
}

void RegionManager::free_memory_alloc_failed(ZPageAllocation* allocation)
{
    const size_t before = current_max_capacity();
    free_memory_alloc_failed_single_partition(allocation->single_partition_allocation());
    if (before != current_max_capacity()) {
        VLOG(REPORT, "Forced to lower max heap size from %zuM to %zuM", before / MB, current_max_capacity() / MB);
    }
}

void RegionManager::free_after_alloc_page_failed(ZPageAllocation* allocation)
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    free_memory_alloc_failed(allocation);
    CHECK(pageAllocatorUsed >= allocation->GetSize());
    pageAllocatorUsed -= allocation->GetSize();
    TrackUsedPeakLocked();
    allocation->reset_for_retry();
    SatisfyStalledAllocations();
}

size_t RegionManager::GetCachedBytes() const
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    size_t bytes = 0;
    for (const auto& partition : partitions) {
        bytes += partition->capacity.load() - partition->used - partition->claimed.load();
    }
    return bytes;
}

void RegionManager::PrintCacheOn() const
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    for (const auto& partition : partitions) {
        VLOG(REPORT, "Partition %u    used %zuM, capacity %zuM, max capacity %zuM", partition->numaId,
             partition->used / MB, partition->capacity.load() / MB, partition->currentMaxCapacity.load() / MB);
        partition->cache.print_on();
    }
}

void RegionManager::SetGarbageThreshold(double garbageThreshold)
{
    fromSpaceGarbageThreshold = garbageThreshold;
}

#if defined(__EULER__)
void RegionManager::SetCacheRatio(double minSize, double maxSize, double defaultParam)
{
    auto env = GetRuntimeConfigValue("cjCacheRatio");
    if (env == nullptr) {
        cacheRatio = defaultParam;
        return;
    }
    double size = CString::ParsePosDecFromEnv(env);
    if (size - minSize >= 0 && maxSize - size >= 0) {
        cacheRatio = size;
        return;
    } else {
        LOG(RTLOG_ERROR, "Unsupported cjCacheRatio parameter.Valid cjCacheRatio range is [%f, %f].\n",
            minSize, maxSize);
    }
    cacheRatio = defaultParam;
}
#endif

ZVirtualMemory RegionManager::ReservedAddressSpan(const ZVirtualMemoryManager& virtualMemory)
{
    // zPageTable.cpp:37-42: address tables cover the whole heap address
    // domain [0, ZAddressOffsetMax). The reverse metadata array is indexed
    // per unit, so it starts at the lowest reserved offset instead of 0.
    const uint32_t partitions = ZPerNUMAStorage::count();
    zoffset lowest = zoffset::invalid;
    for (uint32_t partitionId = 0; partitionId < partitions; ++partitionId) {
        const zoffset candidate = virtualMemory.lowest_available_address(partitionId);
        if (candidate == zoffset::invalid) { continue; }
        if (lowest == zoffset::invalid || candidate < lowest) { lowest = candidate; }
    }
    CHECK_DETAIL(lowest != zoffset::invalid, "virtual memory manager owns no address space");
    return ZVirtualMemory(lowest, ZAddressOffsetMax - untype(lowest));
}

// ZGC's manager owns the sole free-range registry. Cangjie's reverse metadata
// and compiler slot-domain table also need the actual reservation boundaries.
// Borrow/return the initial free ranges before any page can be claimed; do not
// retain a second runtime registry or replace holes with an address envelope.
std::vector<ZPage::ReservedSegment> RegionManager::ReservedSegments(ZVirtualMemoryManager& virtualMemory)
{
    std::vector<ZPage::ReservedSegment> segments;
    for (uint32_t partitionId = 0; partitionId < ZPerNUMAStorage::count(); ++partitionId) {
        ZArray<ZVirtualMemory> ranges;
        virtualMemory.remove_from_low_many_at_most(ZAddressOffsetMax, partitionId, &ranges);
        for (const ZVirtualMemory& range : ranges) {
            segments.push_back({ untype(ZOffset::address_unsafe(range.start())), range.size() });
            virtualMemory.insert(range, partitionId);
        }
    }
    std::sort(segments.begin(), segments.end(), [](const ZPage::ReservedSegment& a,
                                                  const ZPage::ReservedSegment& b) {
        return a.start < b.start;
    });
    return segments;
}

size_t RegionManager::soft_max_capacity() const
{
    return std::min(SoftMaxHeapSize.load(std::memory_order_acquire),
                    current_max_capacity());
}

void RegionManager::Initialize(size_t pageSize, uintptr_t regionInfoAddr, ZVirtualMemoryManager& virtualMemory,
                               ZPhysicalMemoryManager& physicalMemory, const HeapParam& heapParam,
                               double garbageThreshold)
{
    // The capacity is in bytes; reservation boundaries span the virtual heap.
    const ZVirtualMemory span = ReservedAddressSpan(virtualMemory);
    const std::vector<ZPage::ReservedSegment> segments = ReservedSegments(virtualMemory);
    const size_t metadataSize = GetMetadataSize();
    this->regionHeapStart = segments.front().start;
    this->regionHeapEnd = segments.back().End();
    heapCapacity = pageSize;
    CHECK(pageSize <= span.size());
    SetGarbageThreshold(garbageThreshold);
#if defined(__EULER__)
    SetCacheRatio(0.0, 1.0, 1.0);
#endif
    // propagate region heap layout
    ZPage::InitializeSegments(regionInfoAddr + metadataSize, segments);
    InitializePartitions(pageSize);
    this->exemptedRegionThreshold = heapParam.exemptionThreshold;
    DLOG(REPORT, "region info @0x%zx+%zu, heap [0x%zx, 0x%zx), capacity bytes %zu", regionInfoAddr, metadataSize,
         regionHeapStart, regionHeapEnd, pageSize);
}

void RegionManager::promote_used(const ZPage* from, const ZPage* to)
{
    CHECK(from->start() == to->start());
    CHECK(from->size() == to->size());
    CHECK(from->age() != PageAge::old);
    CHECK(to->age() == PageAge::old);
    NoteUsedGenerationDelta(Generation::Young, -static_cast<ssize_t>(to->size()));
    NoteUsedGenerationDelta(Generation::Old, static_cast<ssize_t>(to->size()));
}

void RegionManager::safe_destroy_page(ZPage* page)
{
    _safe_destroy.schedule_delete(page);
}

void RegionManager::free_page(ZPage* page)
{
    // ZGC zPageAllocator.cpp:2253-2266: extract ownership before destroying
    // the descriptor. Forwarding remains owned by the relocation set.
    const ZGenerationId id = page->generation_id();
    const size_t size = page->size();
    ZArray<ZVirtualMemory> vmems;
    prepare_memory_for_free(page, &vmems);
    decrease_used_generation(id, size);
    free_memory(&vmems);
}

// ZGC zPageAllocator.cpp:2269-2284: every page is prepared and accounted in
// the loop; the extracted vmems are returned once after it.
void RegionManager::free_pages(ZGenerationId id, const ZArray<ZPage*>* pages)
{
    ZArray<ZVirtualMemory> vmems;
    for (int i = 0; i < pages->length(); ++i) {
        ZPage* const page = pages->at(i);
        CHECK(page->generation_id() == id);
        const size_t size = page->size();
        prepare_memory_for_free(page, &vmems);
        decrease_used_generation(id, size);
    }
    free_memory(&vmems);
}

void RegionManager::enable_safe_destroy() const
{
    _safe_destroy.enable_deferred_delete();
}

void RegionManager::disable_safe_destroy() const
{
    _safe_destroy.disable_deferred_delete();
}

void RegionManager::prepare_memory_for_free(ZPage* page, ZArray<ZVirtualMemory>* vmems)
{
    const ZVirtualMemory vmem = page->virtual_memory();
    safe_destroy_page(page);
    vmems->push(vmem);
}

void RegionManager::VisitPageOwners(const std::function<void(ZPage*)>& visitor) const
{
    for (ZGenerationId id : {ZGenerationId::young, ZGenerationId::old}) {
        ZGenerationPagesIterator iter(&ZPageTable::heap_table(), id, const_cast<RegionManager*>(this));
        for (ZPage* page; iter.next(&page);) {
            visitor(page);
        }
    }
}



// ZGC zPageAllocator.cpp:426-440: capture generation epochs at request construction.
ZPageAllocation::ZPageAllocation(size_t size, uint8_t role, PageAge age, ZAllocationFlags flags)
    : size(size), youngSeqnum(ZGeneration::young()->seqnum()), oldSeqnum(ZGeneration::old()->seqnum()),
      role(role), age(age), flags(flags), singleAllocation(size) {}

// ZGC zPageAllocator.cpp:525-531.
bool ZPageAllocation::Wait()
{
    return stallResult.get();
}

void ZPageAllocation::Satisfy(bool value)
{
    stallResult.set(value);
}

// ZGC zPageAllocator.cpp:1518-1542: a single decision owns both enqueue and wait.
bool RegionManager::ClaimCapacityOrStall(AllocationStallRequest& request)
{
    {
        std::lock_guard<std::mutex> lock(pageAllocatorMutex);
        if (ClaimAllocationLocked(request)) { return true; }
        if (request.Flags().non_blocking() || stallClosed) { return false; }
        stalled.insert_last(&request);
    }
    return StallAllocation(request);
}

bool RegionManager::StallAllocation(AllocationStallRequest& request)
{
    ZStatTimer timer(ZCriticalPhaseAllocationStall);
    // ZGC zPageAllocator.cpp:1443-1448: asynchronous minor request, then one wait.
    ZDriver::minor()->collect(ZDriverRequest(GC_REASON_ALLOCATION_STALL, ZYoungGCThreads, 0));

    const bool satisfied = request.Wait();
    // Pair with the posting owner before the caller destroys its request.
    // zPageAllocator.cpp:1454-1464.
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    return satisfied;
}



// ZGC zPageAllocator.cpp:2149-2165: memory is returned under the allocator
// lock, independently of when safe_destroy consumes the descriptor.
void RegionManager::free_memory(ZArray<ZVirtualMemory>* vmems)
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    for (const ZVirtualMemory vmem : *vmems) {
        partitions.at(virtualMemory->lookup_partition_id(vmem))->free_memory(vmem);
        CHECK(pageAllocatorUsed >= vmem.size());
        pageAllocatorUsed -= vmem.size();
        TrackUsedPeakLocked();
    }
    SatisfyStalledAllocations();
}

// ZGC zPageAllocator.cpp:2167-2189: reserve capacity, dequeue, then notify.
void RegionManager::SatisfyStalledAllocations()
{
    while (ZPageAllocation* request = stalled.first()) {
        if (!ClaimAllocationLocked(*request)) { return; }
        stalled.remove(request);
        request->Satisfy(true);
    }
}

// ZGC zPageAllocator.cpp:2295-2300.
static bool HasAllocSeenYoung(const ZPageAllocation* request)
{
    return request->YoungSeqnum() != ZGeneration::young()->seqnum();
}

static bool HasAllocSeenOld(const ZPageAllocation* request)
{
    return request->OldSeqnum() != ZGeneration::old()->seqnum();
}

bool RegionManager::IsAllocationStalling() const
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    return stalled.first() != nullptr;
}

bool RegionManager::IsAllocationStallingForOld() const
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    const ZPageAllocation* request = stalled.first();
    return request != nullptr && HasAllocSeenYoung(request) && !HasAllocSeenOld(request);
}

// ZGC zPageAllocator.cpp:2320-2332: only a request predating old mark-start fails.
void RegionManager::NotifyOutOfMemory()
{
    while (ZPageAllocation* request = stalled.first()) {
        if (!HasAllocSeenOld(request)) { return; }
        stalled.remove(request);
        request->Satisfy(false);
    }
}

// ZGC zPageAllocator.cpp:2334-2363: keep late requests queued and drive their GC.
void RegionManager::RestartGC() const
{
    const ZPageAllocation* request = stalled.first();
    if (request == nullptr) { return; }
    if (!HasAllocSeenYoung(request)) {
        ZDriver::minor()->collect(ZDriverRequest(GC_REASON_ALLOCATION_STALL, ZYoungGCThreads, 0));
    } else {
        ZDriver::major()->collect(ZDriverRequest(GC_REASON_ALLOCATION_STALL, ZYoungGCThreads, ZOldGCThreads));
    }
}

void RegionManager::HandleAllocStallingForYoung()
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    RestartGC();
}

void RegionManager::HandleAllocStallingForOld(bool clearedAllSoftRefs)
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    if (clearedAllSoftRefs) { NotifyOutOfMemory(); }
    RestartGC();
}

// Cangjie can finish its runtime while native allocation callers still wait.
// HotSpot has no corresponding allocator shutdown: see advisor 20260920T050835Z.
// The exiting driver closes the queue and publishes each remaining failure once.
void RegionManager::StopStalledAllocations()
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    stallClosed = true;
    while (ZPageAllocation* request = stalled.first()) {
        stalled.remove(request);
        request->Satisfy(false);
    }
}

bool RegionManager::ClaimAllocationLocked(AllocationStallRequest& request)
{
    if (!claim_capacity(&request)) { return false; }
    pageAllocatorUsed += request.GetSize();
    TrackUsedPeakLocked();
    return true;
}



void RegionManager::PromoteAllRegions()
{
    VisitPageOwners([&](ZPage* region) {
        if (region->IsValidRegion() && !region->IsGarbageRegion()) {
            if (region->IsYoungRegion()) {
                region->PromoteYoungRegion();
            } else {
                // Preserve the pre-genface cleanup for already-old regions.
                region->reset(PageAge::old);
            }
        }
    });
}

// ZGC zPageAllocator.cpp:1401-1424: construct once, preserve seqnums across retry.
ZPage* RegionManager::TakeRegion(size_t num, ZPageType type, PageAge age, ZAllocationFlags flags)
{
    ZPageAllocation allocation(num, static_cast<uint8_t>(type), age, flags);
    ZPage* const page = alloc_page_inner(&allocation);
    if (page != nullptr && !flags.gc_relocation() && ConcurrentGCThread::IsRuntimeInitialized()) {
        ZStatInc(ZStatMutatorAllocRate::counter(), allocation.GetSize());
        ZStatMutatorAllocRate::sample_allocation(allocation.GetSize());
    }
    return page;
}

// ZGC zPageAllocator.cpp:1467-1515: address exhaustion terminates this request.
ZPage* RegionManager::alloc_page_inner(ZPageAllocation* allocation)
{
retry:
    if (!ClaimCapacityOrStall(*allocation)) { return nullptr; }
    const ZVirtualMemory cached = satisfied_from_cache_vmem(allocation);
    if (!cached.is_null()) { return create_page(allocation, cached); }
    const ZVirtualMemory vmem = claim_virtual_memory(allocation);
    if (vmem.is_null()) {
        VLOG(REPORT, "Out of address space");
        free_after_alloc_page_failed(allocation);
        return nullptr;
    }
    claim_physical_for_increased_capacity(allocation->single_partition_allocation()->allocation(), vmem);
    if (!commit_and_map(allocation, vmem)) {
        free_after_alloc_page_failed(allocation);
        goto retry;
    }
    return create_page(allocation, vmem);
}

ZPage* RegionManager::create_page(ZPageAllocation* allocation, const ZVirtualMemory& vmem)
{
    // Cangjie headerless records use a reverse granule metadata index.
    // ZGC zPageAllocator.cpp:2053-2075 creates the page and accounts generation usage here.
    ZPage* const page = ZPage::InitRegion(IndexOf(vmem), vmem.size(),
                                        static_cast<ZPageType>(allocation->GetRole()), allocation->Age());
    NoteUsedGenerationDelta(page->GetOwnerGeneration(), static_cast<ssize_t>(vmem.size()));
    return page;
}

void RegionManager::DumpRegionStats(const char* msg) const
{
    // zPageAllocator.cpp:1363-1366 stats(): census is the allocator counters,
    // not a page-table walk over ZPageRole buckets.
    VLOG(REPORT, "%s", msg);
    VLOG(REPORT, "heap max_capacity %zu capacity %zu used %zu young %zu old %zu",
         GetHeapCapacity(), GetCommittedCapacity(), GetAllocatedSize(),
         used_generation(ZGenerationId::young), used_generation(ZGenerationId::old));
    if (Heap::heap() != nullptr) {
        const ZPageAllocatorStats young = Stats(&Heap::GetHeap().GetZGeneration(ZGenerationId::young));
        const ZPageAllocatorStats old = Stats(&Heap::GetHeap().GetZGeneration(ZGenerationId::old));
        VLOG(REPORT,
             "stats young used=%zu used_generation=%zu used_high=%zu used_low=%zu freed=%zu promoted=%zu compacted=%zu stalls=%zu",
             young.used(), young.used_generation(), young.used_high(), young.used_low(),
             young.freed(), young.promoted(), young.compacted(), young.allocation_stalls());
        VLOG(REPORT,
             "stats old used=%zu used_generation=%zu used_high=%zu used_low=%zu freed=%zu promoted=%zu compacted=%zu stalls=%zu",
             old.used(), old.used_generation(), old.used_high(), old.used_low(),
             old.freed(), old.promoted(), old.compacted(), old.allocation_stalls());
    }
    PrintCacheOn();
}


} // namespace MapleRuntime


// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/RegionSpace.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Base/TimeUtils.h"
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Common/ColourEncoding.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
RegionManager::MetadataMapping::~MetadataMapping()
{
    if (base != nullptr) {
#ifdef _WIN64
        (void)VirtualFree(base, 0, MEM_RELEASE);
#else
        (void)munmap(base, size);
#endif
    }
}

RegionManager::~RegionManager()
{
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
    if (reservedEnd > reservedStart) {
        Sanitizer::OnHeapDeallocated(reinterpret_cast<void*>(reservedStart), reservedEnd - reservedStart);
    }
#endif
}

RegionManager::RegionManager(const HeapParam& vmHeapParam, double garbageThreshold) : RegionManager()
{
    const size_t alignedHeapSize = RegionManager::GetAlignedHeapSize(ZHeuristics::max_heap_size());
    const size_t maxCapacity = alignedHeapSize;

    // ZPageAllocator::ZPageAllocator (zPageAllocator.cpp:1201-1260): the
    // virtual memory manager reserves ZVirtualToPhysicalRatio times the max
    // capacity, the physical memory manager creates the backing file.
    virtualMemory.reset(new ZVirtualMemoryManager(maxCapacity));
    CHECK_DETAIL(virtualMemory->is_initialized(), "failed to reserve %zu bytes of heap address space", maxCapacity);
    physicalMemory.reset(new ZPhysicalMemoryManager(maxCapacity));
    CHECK_DETAIL(physicalMemory->is_initialized(), "failed to create heap backing for %zu bytes", maxCapacity);
    physicalMemory->warn_commit_limits(maxCapacity);
    physicalMemory->try_enable_uncommit(0, maxCapacity);

    const ZVirtualMemory span = RegionManager::ReservedAddressSpan(*virtualMemory);
    reservedStart = untype(ZOffset::address_unsafe(span.start()));
    reservedEnd = reservedStart + span.size();
    CHECK_DETAIL(IsRepresentableLow48Range(reservedStart, span.size()),
                 "heap reservation exceeds the 48-bit HeapSlot address carrier: start=%#zx size=%zu",
                 static_cast<size_t>(reservedStart), span.size());
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
    for (const auto& segment : RegionManager::ReservedSegments(*virtualMemory)) {
        Sanitizer::OnHeapAllocated(reinterpret_cast<void*>(segment.start), segment.size);
    }
#endif
    // Metadata remains a contiguous reverse-indexed ABI array, independent of
    // the payload reservations (zPage metadata lives outside virtual memory).
    // It is committed lazily by the kernel: only pageBytes that ever become pages
    // touch their descriptor.
    const std::vector<ZPage::ReservedSegment> segments = RegionManager::ReservedSegments(*virtualMemory);
    metadata.size = RegionManager::GetMetadataSize();
#ifdef _WIN64
    void* const metadataBase = VirtualAlloc(nullptr, metadata.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK_DETAIL(metadataBase != nullptr, "failed to map %zu bytes of region metadata", metadata.size);
#else
    void* const metadataBase =
        mmap(nullptr, metadata.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK_DETAIL(metadataBase != MAP_FAILED, "failed to map %zu bytes of region metadata", metadata.size);
#endif
    metadata.base = metadataBase;
    MAddress metadataAddress = reinterpret_cast<MAddress>(metadata.base);
    CHECK(IsRepresentableLow48Range(metadataAddress, metadata.size));
    Initialize(alignedHeapSize, metadataAddress, *virtualMemory, *physicalMemory, vmHeapParam,
                             garbageThreshold);
    // ZHeap::ZHeap, zHeap.cpp:77-82: prime after allocator initialization.
    // HeapParam has no InitialHeapSize; use four granules (8 MB), bounded
    // by the configured maximum heap for small heaps.
    constexpr size_t initialHeapSize = 4 * ZGranuleSize;
    CHECK_DETAIL(PrimeCache(std::min(initialHeapSize, maxCapacity)),
                 "failed to allocate initial heap");
#if defined(MRT_DUMP_ADDRESS)
    VLOG(REPORT, "region metadata@%zx, heap @[0x%zx+%zu, 0x%zx)", metadataAddress, reservedStart, reservedEnd - reservedStart,
         reservedEnd);
#endif
    std::vector<HeapSlotAddressRange> heapReservations;
    for (const auto& segment : segments) {
        heapReservations.push_back({ segment.start, segment.End() });
    }
    Heap::OnHeapCreated(reservedStart, heapReservations);
    Heap::OnHeapExtended(reservedEnd);
}


// zPageAllocator.cpp:1332-1373 — the allocator account feeding ZStatHeap.
void RegionManager::UpdateCollectionStats(ZGenerationId id)
{
    const size_t i = id == ZGenerationId::young ? 0 : 1;
    collectionUsedHigh[i] = pageAllocatorUsed;
    collectionUsedLow[i] = pageAllocatorUsed;
}

size_t RegionManager::AllocationStallsNow() const
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    return stalled.size();
}

ZPageAllocatorStats RegionManager::Stats(const ZGeneration* generation) const
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    return StatsInner(generation);
}

ZPageAllocatorStats RegionManager::StatsInner(const ZGeneration* generation) const
{
    const ZGenerationId id = generation->id();
    const size_t i = id == ZGenerationId::young ? 0 : 1;
    return ZPageAllocatorStats(0 /* min_capacity: host HeapParam has no min-heap-size */,
                               GetHeapCapacity(),
                               soft_max_capacity(),
                               GetCommittedCapacity(),
                               GetAllocatedSize(),
                               collectionUsedHigh[i],
                               collectionUsedLow[i],
                               UsedGeneration(id),
                               generation->freed(),
                               generation->promoted(),
                               generation->compacted(),
                               stalled.size());
}

ZPageAllocatorStats RegionManager::UpdateAndStats(const ZGeneration* generation)
{
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    UpdateCollectionStats(generation->id());
    return StatsInner(generation);
}

size_t RegionManager::GetAllocatedSize() const
{
        // zPageAllocator.cpp:1311 ZPageAllocator::used: page-granular committed
        // counter maintained at allocation and free_page, not a
        // list sum. Pages count as used until free_page.
        return pageAllocatorUsed;
    }


}
