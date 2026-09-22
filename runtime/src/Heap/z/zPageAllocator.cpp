// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zPageAllocator.hpp"
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
#include "Common/ScopedObjectAccess.h"
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




void FreeRegionManager::Initialize(ZVirtualMemoryManager& virtualMemoryManager,
                                   ZPhysicalMemoryManager& physicalMemoryManager, size_t maxCapacity)
{
    markQuarantineMemory.clear();
    partitions.clear();
    nextPartition = 0;
    virtualMemory = &virtualMemoryManager;
    physicalMemory = &physicalMemoryManager;
    // ZPageAllocator::ZPageAllocator (zPageAllocator.cpp:1230-1236): one
    // ZPartition per NUMA id, each with max_capacity's share.
    const uint32_t numaCount = ZPerNUMAStorage::count();
    for (uint32_t numaId = 0; numaId < numaCount; ++numaId) {
        partitions.emplace_back(new Partition(numaId));
        Partition& partition = *partitions.back();
        partition.currentMaxCapacity =
            NumaTopology::calculate_share(numaId, maxCapacity, ZGranuleSize);
    }
}

ZVirtualMemory FreeRegionManager::VirtualMemoryOf(size_t index, size_t count)
{
    const uintptr_t address = ZPage::GranuleAddress(index);
    return ZVirtualMemory(ZAddress::offset(to_zaddress_unsafe(address)), count);
}

size_t FreeRegionManager::IndexOf(const ZVirtualMemory& vmem)
{
    const size_t index = ZPage::GranuleIndex(untype(ZOffset::address_unsafe(vmem.start())));
    CHECK(index != std::numeric_limits<uint32_t>::max());
    return static_cast<size_t>(index);
}

// ZPartition thin functions, zPageAllocator.cpp:790-920.
ZVirtualMemory FreeRegionManager::claim_virtual(size_t size, uint32_t partition_id)
{
    return virtualMemory->remove_from_low(size, partitions.at(partition_id)->numaId);
}

size_t FreeRegionManager::claim_virtual(size_t size, uint32_t partition_id, ZArray<ZVirtualMemory>* vmems_out)
{
    return virtualMemory->remove_from_low_many_at_most(size, partitions.at(partition_id)->numaId, vmems_out);
}

void FreeRegionManager::free_virtual(const ZVirtualMemory& vmem, uint32_t partition_id)
{
    virtualMemory->insert(vmem, partitions.at(partition_id)->numaId);
}

ZVirtualMemory FreeRegionManager::free_and_claim_virtual_from_low_exact_or_many(size_t size, uint32_t partition_id,
                                                                                ZArray<ZVirtualMemory>* vmems_in_out)
{
    return virtualMemory->insert_and_remove_from_low_exact_or_many(size, partitions.at(partition_id)->numaId,
                                                                   vmems_in_out);
}

void FreeRegionManager::claim_physical(const ZVirtualMemory& vmem, uint32_t partition_id)
{
    physicalMemory->alloc(vmem, partitions.at(partition_id)->numaId);
}

void FreeRegionManager::free_physical(const ZVirtualMemory& vmem, uint32_t partition_id)
{
    physicalMemory->free(vmem, partitions.at(partition_id)->numaId);
}

size_t FreeRegionManager::commit_physical(const ZVirtualMemory& vmem, uint32_t partition_id)
{
    return physicalMemory->commit(vmem, partitions.at(partition_id)->numaId);
}

size_t FreeRegionManager::uncommit_physical(const ZVirtualMemory& vmem)
{
    return physicalMemory->uncommit(vmem);
}

void FreeRegionManager::map_virtual(const ZVirtualMemory& vmem, uint32_t partition_id)
{
    physicalMemory->map(vmem, partitions.at(partition_id)->numaId);
}

void FreeRegionManager::unmap_virtual(const ZVirtualMemory& vmem)
{
    physicalMemory->unmap(vmem);
}

void FreeRegionManager::sort_segments_physical(const ZVirtualMemory& vmem)
{
    physicalMemory->sort_segments_physical(vmem);
}

void FreeRegionManager::stash_segments(const ZArraySlice<const ZVirtualMemory>& vmems, ZArray<zbacking_index>* stash_out) const
{
    physicalMemory->stash_segments(vmems, stash_out);
}

void FreeRegionManager::restore_segments(const ZVirtualMemory& vmem, const ZArray<zbacking_index>& stash)
{
    physicalMemory->restore_segments(vmem, stash);
}

void FreeRegionManager::restore_segments(const ZArraySlice<const ZVirtualMemory>& vmems, const ZArray<zbacking_index>& stash)
{
    physicalMemory->restore_segments(vmems, stash);
}

// ZPartition::increase_capacity / decrease_capacity, zPageAllocator.cpp:648-676.
size_t FreeRegionManager::increase_capacity(uint32_t partition_id, size_t size)
{
    Partition& partition = *partitions.at(partition_id);
    const size_t increased = std::min(size, partition.currentMaxCapacity - partition.capacity);
    if (increased > 0) {
        partition.capacity += increased;
        Uncommitter::CancelCycleLocked();
    }
    return increased;
}

void FreeRegionManager::decrease_capacity(uint32_t partition_id, size_t size, bool set_max_capacity)
{
    Partition& partition = *partitions.at(partition_id);
    CHECK(partition.capacity >= size);
    partition.capacity -= size;
    if (set_max_capacity) {
        VLOG(REPORT, "Forced to lower max partition (%u) capacity from %zuM to %zuM", partition.numaId,
             partition.currentMaxCapacity / MB, partition.capacity / MB);
        partition.currentMaxCapacity = partition.capacity;
    }
}

size_t FreeRegionManager::current_max_capacity() const
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t total = 0;
    for (const auto& partition : partitions) {
        total += partition->currentMaxCapacity;
    }
    return total;
}

size_t FreeRegionManager::capacity() const
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t total = 0;
    for (const auto& partition : partitions) { total += partition->capacity; }
    return total;
}

void FreeRegionManager::InsertCommitted(Partition& partition, size_t index, size_t count)
{
    partition.cache.insert(VirtualMemoryOf(index, count));
}

// ZPartition::free_memory (zPageAllocator.cpp:690-697): a freed page's vmem
// goes back to the mapped cache of the partition that owns its address.
void FreeRegionManager::FreeMemory(size_t index, size_t count)
{
    const ZVirtualMemory vmem = VirtualMemoryOf(index, count);
    const uint32_t partitionId = virtualMemory->lookup_partition_id(vmem);
    Partition& partition = *partitions.at(partitionId);
    InsertCommitted(partition, index, count);
    // decrease_used
    CHECK(partition.used >= vmem.size());
    partition.used -= vmem.size();
}

void FreeRegionManager::AddGarbageMemory(size_t index, size_t count, bool allowSaferegion)
{
    std::unique_ptr<ScopedEnterSaferegion> saferegion;
    if (allowSaferegion) { saferegion.reset(new ScopedEnterSaferegion(true)); }
    std::lock_guard<std::mutex> lock(cacheMutex);
    FreeMemory(index, count);
}

size_t FreeRegionManager::ReleaseMarkQuarantineToDirty()
{
    ScopedEnterSaferegion saferegion(true);
    std::lock_guard<std::mutex> quarantineLock(markQuarantineTreeMutex);
    std::lock_guard<std::mutex> cacheLock(cacheMutex);
    size_t count = 0;
    for (const auto& memory : markQuarantineMemory) {
        FreeMemory(IndexOf(memory), memory.size());
        count += memory.size();
    }
    markQuarantineMemory.clear();
    return count;
}

// ZGC zPageAllocator.cpp:764-785: no virtual-memory or physical-memory work.
bool FreeRegionManager::ZPartition::claim_capacity_fast_medium(PageMemory& memory)
{
    CHECK(ZPageSizeMediumEnabled);
    const ZVirtualMemory vmem = cache.remove_contiguous_power_of_2(ZPageSizeMediumMin, ZPageSizeMediumMax);
    if (vmem.is_null()) { return false; }
    memory.index = FreeRegionManager::IndexOf(vmem);
    memory.size = vmem.size();
    memory.partition = numaId;
    memory.committed = true;
    memory.partialMappings.clear();
    memory.virtualClaimed = true;
    memory.harvestedBytes = 0;
    used += memory.size;
    return true;
}

// ZPartition::claim_capacity / claim_from_cache_or_increase_capacity,
// zPageAllocator.cpp:702-762.
bool FreeRegionManager::ClaimPageMemory(size_t num, PageMemory& memory, ZAllocationFlags flags)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    CHECK(num != 0 && num % ZGranuleSize == 0);
    const size_t size = num;
    for (size_t visited = 0; visited < partitions.size(); ++visited) {
        const size_t selected = (nextPartition + visited) % partitions.size();
        Partition& partition = *partitions[selected];
        if (flags.fast_medium()) {
            if (!partition.claim_capacity_fast_medium(memory)) { continue; }
            nextPartition = (selected + 1) % partitions.size();
            return true;
        }
        if (partition.available() < size) {
            // Out of memory in this partition
            continue;
        }

        // Try to allocate one contiguous vmem
        const ZVirtualMemory vmem = partition.cache.remove_contiguous(size);
        if (!vmem.is_null()) {
            memory.index = IndexOf(vmem);
            memory.size = num;
            memory.partition = static_cast<uint32_t>(selected);
            memory.committed = true;
            memory.partialMappings.clear();
            memory.virtualClaimed = true;
            memory.harvestedBytes = 0;
            partition.used += size;
            nextPartition = (selected + 1) % partitions.size();
            return true;
        }

        // Try increase capacity
        const size_t increased = increase_capacity(static_cast<uint32_t>(selected), size);
        // Could not increase capacity enough to satisfy the allocation completely.
        // Try removing multiple vmems from the mapped cache.
        const size_t remaining = size - increased;
        const size_t harvested = remaining == 0 ? 0 : partition.cache.remove_discontiguous(remaining, &memory.partialMappings);
        CHECK(harvested + increased == size);
        memory.index = 0;
        memory.size = num;
        memory.partition = static_cast<uint32_t>(selected);
        memory.committed = harvested == size;
        memory.virtualClaimed = false;
        memory.harvestedBytes = harvested;
        // increase_used
        partition.used += size;
        nextPartition = (selected + 1) % partitions.size();
        return true;
    }
    return false;
}

// ZPageAllocator::claim_virtual_memory_single_partition (zPageAllocator.cpp:1689-1701)
// and ZPartition::prepare_harvested_and_claim_virtual (:1001-1046).
bool FreeRegionManager::PreparePageMemory(PageMemory& memory)
{
    if (memory.virtualClaimed) { return true; }
    std::lock_guard<std::mutex> lock(cacheMutex);
    const uint32_t partitionId = memory.partition;
    const size_t size = memory.size;
    const size_t harvested = memory.harvestedBytes;
    if (harvested == 0) {
        // Just try to claim virtual memory
        const ZVirtualMemory vmem = claim_virtual(size, partitionId);
        if (vmem.is_null()) { return false; }
        memory.index = IndexOf(vmem);
        memory.virtualClaimed = true;
        return true;
    }

    // Unmap virtual memory
    for (const ZVirtualMemory vmem : memory.partialMappings) {
        unmap_virtual(vmem);
    }

    // Stash segments
    ZArray<zbacking_index> stash;
    stash_segments(memory.partialMappings, &stash);

    // Shuffle virtual memory. We attempt to allocate enough memory to cover the
    // entire allocation size, not just for the harvested memory.
    const ZVirtualMemory result =
        free_and_claim_virtual_from_low_exact_or_many(size, partitionId, &memory.partialMappings);

    // Restore segments
    if (!result.is_null()) {
        // Got exact match. Restore stashed physical segments for the harvested part.
        restore_segments(result.first_part(harvested), stash);
    } else {
        // Got many partial vmems
        restore_segments(memory.partialMappings, stash);
    }

    if (result.is_null()) {
        // Before returning harvested memory to the cache it must be mapped.
        for (const ZVirtualMemory vmem : memory.partialMappings) {
            map_virtual(vmem, partitionId);
        }
        return false;
    }

    memory.index = IndexOf(result);
    memory.virtualClaimed = true;
    return true;
}

// alloc_page_inner (zPageAllocator.cpp:1470-1515): claim_physical_for_increased_capacity
// (:1761-1780), commit_and_map_single_partition (:1792-1806), map_committed
// (:1878-1887), cleanup_failed_commit_single_partition (:1906-1932), create_page.
ZPage* FreeRegionManager::MaterializePageMemory(PageMemory& memory, ZPageType role,
                                                     bool expectPhysicalMem, bool clearPayload, size_t& committedBytes,
                                                     PageAge age)
{
    (void)expectPhysicalMem;
    committedBytes = 0;
    // satisfied_from_cache_vmem: a contiguous cache hit is already committed and mapped.
    const bool fromCache = memory.virtualClaimed;
    if (!PreparePageMemory(memory)) { return nullptr; }
    const size_t idx = memory.index;
    const size_t num = memory.size;
    const uint32_t partitionId = memory.partition;
    committedBytes = fromCache ? num : 0;
    if (!fromCache) {
        // The vmem was built from harvested memory and/or increased capacity.
        const ZVirtualMemory vmem = VirtualMemoryOf(idx, num);
        const size_t alreadyCommitted = memory.harvestedBytes;
        const ZVirtualMemory nonCommitted = vmem.last_part(alreadyCommitted);

        size_t committed = 0;
        if (nonCommitted.size() > 0) {
            // Claim physical memory for the increased capacity
            claim_physical(nonCommitted, partitionId);

            // Commit memory for the increased capacity
            committed = commit_physical(nonCommitted, partitionId);
            CHECK(committed <= nonCommitted.size() && committed % ZGranuleSize == 0);
        }
        const size_t totalCommitted = alreadyCommitted + committed;
        committedBytes = totalCommitted;

        // Map all the committed memory
        const ZVirtualMemory committedVmem = vmem.first_part(totalCommitted);
        if (committedVmem.size() > 0) {
            sort_segments_physical(committedVmem);
            map_virtual(committedVmem, partitionId);
        }

        if (committed != nonCommitted.size()) {
            // Commit failed: keep the committed and mapped prefix for the
            // cache, free the virtual and physical memory of the failed part.
            memory.partialMappings.clear();
            if (committedVmem.size() > 0) {
                memory.partialMappings.append(committedVmem);
            }
            const ZVirtualMemory failedVmem = vmem.last_part(totalCommitted);
            std::lock_guard<std::mutex> lock(cacheMutex);
            free_physical(failedVmem, partitionId);
            free_virtual(failedVmem, partitionId);
            return nullptr;
        }
        memory.committed = true;
    }
    if ((fromCache || memory.harvestedBytes != 0) && clearPayload) {
        ZPage::ClearPageMemory(idx, num);
    }
    ZPage* region = ZPage::InitRegion(idx, num, role, age);
    if (!fromCache) {
        ClearReleasedMemory(clearPayload, idx, num);
    }
    return region;
}

// ZPartition::free_memory_alloc_failed (zPageAllocator.cpp:1079-1101): the
// committed and mapped parts return to the cache; the capacity that never got
// committed is given back, lowering max capacity after a commit failure.
void FreeRegionManager::FreeMemoryAllocFailed(PageMemory& memory)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    Partition& partition = *partitions.at(memory.partition);
    const size_t size = memory.size;
    // Only decrease the overall used and not the generation used,
    // since the allocation failed and generation used wasn't bumped.
    CHECK(partition.used >= size);
    partition.used -= size;
    size_t freed = 0;
    for (const ZVirtualMemory vmem : memory.partialMappings) {
        freed += vmem.size();
        InsertCommitted(partition, IndexOf(vmem), static_cast<size_t>(vmem.size()));
    }
    memory.partialMappings.clear();
    const size_t remaining = size - freed;
    if (remaining > 0) {
        // Only a failed commit leaves capacity behind that could not be backed.
        const bool commitFailed = memory.virtualClaimed;
        decrease_capacity(memory.partition, remaining, commitFailed);
    }
}

size_t FreeRegionManager::GetCachedBytes() const
{
    // capacity == used + cached + claimed (ZPartition accounting).
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t bytes = 0;
    for (const auto& partition : partitions) {
        bytes += partition->capacity - partition->used - partition->claimed;
    }
    return static_cast<size_t>(bytes);
}

void FreeRegionManager::PrintCacheOn() const
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    for (const auto& partition : partitions) {
        VLOG(REPORT, "Partition %u    used %zuM, capacity %zuM, max capacity %zuM", partition->numaId,
             partition->used / MB, partition->capacity / MB, partition->currentMaxCapacity / MB);
        partition->cache.print_on();
    }
}

// zUncommitter.cpp:392-403.
size_t FreeRegionManager::RemoveForUncommit(size_t flush, ZArray<ZVirtualMemory>* out)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t flushed = 0;
    for (auto& partition : partitions) {
        if (flush <= flushed) { break; }
        const size_t partitionFlushed = partition->cache.remove_for_uncommit(flush - flushed, out);
        // Record flushed memory as claimed
        partition->claimed += partitionFlushed;
        flushed += partitionFlushed;
    }
    return flushed;
}

// zUncommitter.cpp:414-420.
void FreeRegionManager::UncommitFlushed(size_t flushed)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t remaining = flushed;
    for (auto& partition : partitions) {
        const size_t part = std::min(remaining, partition->claimed);
        if (part == 0) { continue; }
        partition->claimed -= part;
        decrease_capacity(partition->numaId, part, false /* set_max_capacity */);
        remaining -= part;
    }
    CHECK(remaining == 0);
}

void RegionManager::SetGarbageThreshold(double garbageThreshold)
{
    fromSpaceGarbageThreshold = garbageThreshold;
}

#if defined(__EULER__)
void RegionManager::SetCacheRatio(double minSize, double maxSize, double defaultParam)
{
    auto env = std::getenv("cjCacheRatio");
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

// The host configures SoftMaxHeapSize through cjSoftMaxHeapSize, rather than
// HotSpot's flag table. Capacity clamping belongs to the allocator in both.
static std::atomic<size_t> softMaxHeapSize{0};

size_t RegionManager::soft_max_capacity() const
{
    return std::min(softMaxHeapSize.load(std::memory_order_acquire),
                    freeRegionManager.current_max_capacity());
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
    size_t soft = pageSize;
    if (const char* env = std::getenv("cjSoftMaxHeapSize")) {
        const size_t parsedKb = CString::ParseSizeFromEnv(env);
        if (parsedKb > 0) {
            soft = parsedKb * KB;
        }
    }
    softMaxHeapSize.store(soft, std::memory_order_release);
    CHECK(pageSize <= span.size());
    this->inactiveZone = regionHeapStart;
    SetGarbageThreshold(garbageThreshold);
#if defined(__EULER__)
    SetCacheRatio(0.0, 1.0, 1.0);
#endif
    // propagate region heap layout
    ZPage::InitializeSegments(regionInfoAddr + metadataSize, segments);
    freeRegionManager.Initialize(virtualMemory, physicalMemory, pageSize);
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
    ZPage::RetireDescriptor(page);
}

void RegionManager::free_page(ZPage* page)
{
    // ZGC zPageAllocator.cpp:2253-2266: extract ownership before destroying
    // the descriptor. Forwarding remains owned by the relocation set.
    const ZGenerationId id = page->generation_id();
    const size_t size = page->size();
    const PageMemory memory{page->granule_index(), size, 0, true};
    safe_destroy_page(page);
    decrease_used_generation(id, size);
    ReturnRetiredPageMemory(memory);
}

void RegionManager::ReclaimRegion(ZPage* region)
{
    // zPageAllocator.cpp:2263,2280: per-generation used, region-granular.
    NoteUsedGenerationDelta(region->GetOwnerGeneration(), -static_cast<ssize_t>(region->GetRegionSize()));
    ZPage::RetirePage(region, [this, region] { ReclaimRetiredRegion(region); });
}

void RegionManager::ReclaimRetiredRegion(ZPage* region)
{
    // convert "I traced the paths" into a machine check, but none of the designs proved the
    // caller enumeration and five of the six ReclaimRegion callers have already detached the
    // region, so an abort here would trade an unproven assumption for a hard stop. Count and
    // name it instead, under the default-off account gate; a non-zero funnel_held is the
    // signal that the enumeration was wrong.
    size_t num = region->GetRegionSize();
    size_t unitIndex = region->granule_index();
    DLOG(REGION, "reclaim region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), 0u);

    // STEER3: scrub is at CollectRegion only (see header). Reclaim/TakeRegion reuse
    // must not re-scan O(N) under remset mutex.

    {
        ZPage::InPlaceClaimScope drain(region, ZForwarding::Retire::RECLAIM_DIRTY);
    }
    region->RetirePageMemory();
    ReturnPageMemory(PageMemory{ unitIndex, num, 0, true });
}

// ZGC zPageAllocator.cpp:426-440: capture generation epochs at request construction.
ZPageAllocation::ZPageAllocation(size_t size, uint8_t role, bool physical, bool clear, ZAllocationFlags flags)
    : size(size), youngSeqnum(ZGeneration::young()->seqnum()), oldSeqnum(ZGeneration::old()->seqnum()),
      role(role), physical(physical), clear(clear), flags(flags) {}

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
    // ZGC zPageAllocator.cpp:1443-1448: asynchronous minor request, then one wait.
    ZCollectedHeap::heap()->driver_minor()->collect(ZDriverRequest(GC_REASON_ALLOCATION_STALL, 0, 0));

    // zFuture.inline.hpp:47-53: a Java thread waits with a safepoint check;
    // here the mutator enters its saferegion before ZFuture::get (I3/I4).
    ScopedEnterSaferegion enterSaferegion(false);
    const bool satisfied = request.Wait();
    // Pair with the posting owner before the caller destroys its request.
    // zPageAllocator.cpp:1454-1464.
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    return satisfied;
}

void RegionManager::ReturnPageMemory(const PageMemory& memory)
{
    ZPage* region = Heap::page(ZPage::GranuleAddress(memory.index));
    if (region != nullptr) {
        CHECK(region->granule_index() == memory.index && region->GetRegionSize() == memory.size);
        // Only materialized page geometry is retired. Its allocation-time
        // partial mappings have already been consumed; ZArray is non-copyable.
        const size_t index = memory.index;
        const size_t pageBytes = memory.size;
        const uint32_t partition = memory.partition;
        const bool committed = memory.committed;
        ZPage::RetirePage(region, [this, region, index, pageBytes, partition, committed] {
            region->RetirePageMemory();
            ReturnRetiredPageMemory(PageMemory{index, pageBytes, partition, committed});
        });
        return;
    }
    // An allocation cancelled before materialization has no page descriptor.
    // Reclaim/Release also arrive here after completing descriptor retirement.
    ReturnRetiredPageMemory(memory);
}

void RegionManager::ReturnRetiredPageMemory(const PageMemory& memory, bool allowSaferegion)
{
    // zPageAllocator.cpp:1999 / 2150: hand back memory, decrease used and
    // satisfy the FIFO in one allocator-owner critical section. Enter the
    // saferegion before the owner, including nested cache hand-back calls.
    // A shared-page publication loser already has an object allocated in
    // the winner. It must return its unused page without a safepoint.
    std::unique_ptr<ScopedEnterSaferegion> enterSaferegion;
    if (allowSaferegion) { enterSaferegion.reset(new ScopedEnterSaferegion(true)); }
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    CHECK(memory.committed);
    freeRegionManager.AddGarbageMemory(memory.index, memory.size, allowSaferegion);
    const size_t bytes = memory.size;
    CHECK(pageAllocatorUsed >= bytes);
    pageAllocatorUsed -= bytes;
    TrackUsedPeakLocked();
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
        ZCollectedHeap::heap()->driver_minor()->collect(ZDriverRequest(GC_REASON_ALLOCATION_STALL, 0, 0));
    } else {
        ZCollectedHeap::heap()->driver_major()->collect(ZDriverRequest(GC_REASON_ALLOCATION_STALL, 0, 0));
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
    const size_t size = request.GetSize();
    const size_t num = size;
    PageMemory& memory = request.Memory();
    if (!freeRegionManager.ClaimPageMemory(num, memory, request.Flags())) { return false; }
    pageAllocatorUsed += memory.size;
    TrackUsedPeakLocked();
    return true;
}

void RegionManager::ReclaimRegionToMarkQuarantine(ZPage* region)
{
    NoteUsedGenerationDelta(region->GetOwnerGeneration(), -static_cast<ssize_t>(region->GetRegionSize()));
    ZPage::RetirePage(region, [this, region] { ReclaimRetiredRegionToMarkQuarantine(region); });
}

void RegionManager::ReclaimRetiredRegionToMarkQuarantine(ZPage* region)
{
    // routedest: census only, see ReclaimRegion.
    size_t num = region->GetRegionSize();
    size_t unitIndex = region->granule_index();
    DLOG(REGION, "mark-quarantine region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
         region->GetRegionAllocatedSize(), region->GetRegionEnd(), 0u);
    {
        ZPage::InPlaceClaimScope drain(region, ZForwarding::Retire::RECLAIM_MARK_QUARANTINE);
    }
    region->RetirePageMemory();
    ScopedEnterSaferegion enterSaferegion(true);
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    freeRegionManager.AddMarkQuarantineMemory(unitIndex, num);
    CHECK(pageAllocatorUsed >= num);
    pageAllocatorUsed -= num;
    TrackUsedPeakLocked();
}

size_t RegionManager::ReleaseRegion(ZPage* region)
{
    const size_t size = region->GetRegionSize();
    NoteUsedGenerationDelta(region->GetOwnerGeneration(), -static_cast<ssize_t>(size));
    ZPage::RetirePage(region, [this, region] { ReleaseRetiredRegion(region); });
    return size;
}

void RegionManager::ReleaseRetiredRegion(ZPage* region)
{
    // routedest: census only, see ReclaimRegion.

    // holdercapture: large regions above the release threshold never reach CollectRegion,
    // so the snapshot has to be taken on this path too or the face is lost unrecorded.

    size_t num = region->GetRegionSize();
    size_t unitIndex = region->granule_index();
    // Large regions above the release threshold bypass CollectRegion. Invalidate
    DLOG(REGION, "release region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), 0u);

    {
        ZPage::InPlaceClaimScope drain(region, ZForwarding::Retire::RELEASE_REGION);
    }
    region->RetirePageMemory();
    // ZPageAllocator::free_page (zPageAllocator.cpp:2083-2165): freed memory
    // enters the mapped cache; only ZUncommitter uncommits.
    ReturnPageMemory(PageMemory{ unitIndex, num, 0, true });
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

ZPage* RegionManager::TakeRegion(size_t num, ZPageType type, bool expectPhysicalMem,
                                       bool allowSaferegion, bool clearPayload, PageAge age, ZAllocationFlags flags)
{
    allowSaferegion = allowSaferegion && !flags.non_blocking();
    if (!allowSaferegion || IsGcThread()) { flags.set_non_blocking(); }
    size_t size = num;

#if !defined(__OHOS__)
    size_t gatedBytes = 0;
    ZPage* garbage = allowSaferegion ? TakeReclaimableGarbageRegion(&gatedBytes) : nullptr;
    if (garbage != nullptr) {
        ReclaimRegion(garbage);
    }
#else
    size_t gatedBytes = GetGatedGarbageBytes();
#endif

retry:
    ZPageAllocation request(size, static_cast<uint8_t>(type), expectPhysicalMem, clearPayload, flags);
    const bool claimed = ClaimCapacityOrStall(request);
    if (claimed) {
        size = request.Memory().size;
        size_t committedBytes = 0;
        ZPage* region = freeRegionManager.MaterializePageMemory(
            request.Memory(), type, request.ExpectsPhysicalMemory(), request.ClearsPayload(), committedBytes, age);
        if (request.Memory().virtualClaimed) {
            std::lock_guard<std::mutex> lock(pageAllocatorMutex);
            const uintptr_t end = ZPage::GranuleAddress(request.Memory().index) + size;
            inactiveZone.store(std::max(inactiveZone.load(std::memory_order_relaxed), end), std::memory_order_release);
        }
        if (region == nullptr) {
            std::unique_ptr<ScopedEnterSaferegion> enterSaferegion;
            if (allowSaferegion) { enterSaferegion.reset(new ScopedEnterSaferegion(true)); }
            std::lock_guard<std::mutex> lock(pageAllocatorMutex);
            (void)committedBytes;
            freeRegionManager.FreeMemoryAllocFailed(request.Memory());
            CHECK(pageAllocatorUsed >= size);
            pageAllocatorUsed -= size;
            TrackUsedPeakLocked();
            SatisfyStalledAllocations();
            if (allowSaferegion) {
                goto retry;
            }
            return nullptr;
        }
        // ZGC zPageAllocator.cpp:1414-1418: relocation is not mutator allocation.
        if (!flags.gc_relocation() && ConcurrentGCThread::IsRuntimeInitialized()) {
            ZStatInc(ZStatMutatorAllocRate::counter(), size);
            ZStatMutatorAllocRate::sample_allocation(size);
        }
        // zPageAllocator.cpp:2065: per-generation used, region-granular.
        NoteUsedGenerationDelta(region->GetOwnerGeneration(), static_cast<ssize_t>(size));
        return region;
    }

    if (gatedBytes > 0) {
        static std::atomic<size_t> supplyGatedPressureCount { 0 };
        size_t n = supplyGatedPressureCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0) {
            VLOG(REPORT, "[Alloc] supply_gated_pressure gated_bytes=%zu n=%zu", gatedBytes, n);
        }
    }
    return nullptr;
}

size_t RegionManager::CollectLargeGarbage()
{
    size_t garbageSize = 0;
    std::vector<ZPage*> largePages;
    {
        ZPage::SafeDestroyScope scope;
        ZPageTableIterator iter(&ZPageTable::heap_table());
        for (ZPage* region; iter.next(&region);) {
            if (region->GetRegionRole() == ZPageRole::OldLarge) {
                largePages.push_back(region);
            }
        }
    }
    for (ZPage* region : largePages) {
        // for large region, the object is the page start (zPage.inline.hpp:254-256).
        if (!region->is_object_live(to_zaddress(region->GetRegionStart()))) {
            DLOG(REGION, "reclaim large region %p@[0x%zx+%zu, 0x%zx) type %u", region, region->GetRegionStart(),
                 region->GetRegionAllocatedSize(), region->GetRegionEnd(), 0u);

            ZPage* del = region;
            del->SetRegionRole(ZPageRole::None);
            if (del->GetRegionSize() > ZPage::LARGE_OBJECT_RELEASE_THRESHOLD) {
                garbageSize += ReleaseRegion(del);
            } else {

                garbageSize += CollectRegion<Generation::Old>(del);
            }
        }
    }

    return garbageSize;
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
    freeRegionManager.PrintCacheOn();
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
#include "Common/ScopedObjectAccess.h"
#include "Common/ColourEncoding.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
RegionManager::MetadataMapping::~MetadataMapping()
{
    if (base != nullptr) {
        (void)munmap(base, size);
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
    size_t heapSize = 0;
    CHECK_DETAIL(CheckedMulSize(vmHeapParam.heapSize, size_t{1024}, heapSize),
                 "heap size overflows bytes before reservation: heapSizeKB=%zu", vmHeapParam.heapSize);
    const size_t alignedHeapSize = RegionManager::GetAlignedHeapSize(heapSize);
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
    void* const metadataBase =
        mmap(nullptr, metadata.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK_DETAIL(metadataBase != MAP_FAILED, "failed to map %zu bytes of region metadata", metadata.size);
    metadata.base = metadataBase;
    MAddress metadataAddress = reinterpret_cast<MAddress>(metadata.base);
    CHECK(IsRepresentableLow48Range(metadataAddress, metadata.size));
    Initialize(alignedHeapSize, metadataAddress, *virtualMemory, *physicalMemory, vmHeapParam,
                             garbageThreshold);
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

ZPageAllocatorStats RegionManager::Stats(const ZGeneration* generation) const
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
                               AllocationStallsNow());
}

ZPageAllocatorStats RegionManager::UpdateAndStats(const ZGeneration* generation)
{
    UpdateCollectionStats(generation->id());
    return Stats(generation);
}

size_t RegionManager::GetAllocatedSize() const
{
        // zPageAllocator.cpp:1311 ZPageAllocator::used: page-granular committed
        // counter maintained at TakeRegion/ReturnPageMemory/reclaim, not a
        // list sum. Garbage-pending pages count as used until reclaim, as
        // ZGC's _used does until free_page.
        return pageAllocatorUsed;
    }


void FreeRegionManager::AddMarkQuarantineMemory(size_t idx, size_t num)
{
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(markQuarantineTreeMutex);
        markQuarantineMemory.push_back(VirtualMemoryOf(idx, num));
    }


size_t RegionManager::GetLargeObjectSize() const
{
    size_t bytes = 0;
    ZPage::SafeDestroyScope scope;
    ZPageTableIterator iter(&ZPageTable::heap_table());
    for (ZPage* region; iter.next(&region);) {
        const ZPageRole role = region->GetRegionRole();
        if (role == ZPageRole::OldLarge || role == ZPageRole::RecentLarge || role == ZPageRole::LargeTrace) {
            bytes += region->GetRegionSize();
        }
    }
    return bytes;
}

}
