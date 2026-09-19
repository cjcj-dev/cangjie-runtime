// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zPageAllocator.hpp"

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
#include "Heap/Allocator/HeapFiller.h"
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




void FreeRegionManager::Initialize(UnitCount regionCnt, ZVirtualMemoryManager& virtualMemoryManager,
                                   ZPhysicalMemoryManager& physicalMemoryManager, size_t maxCapacity)
{
    markQuarantineTree.Init(regionCnt);
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
            NumaTopology::calculate_share(numaId, maxCapacity, ZPage::UNIT_SIZE);
    }
}

ZVirtualMemory FreeRegionManager::VirtualMemoryOf(UnitIndex index, UnitCount count)
{
    const uintptr_t address = ZPage::GetUnitAddress(index);
    return ZVirtualMemory(ZAddress::offset(to_zaddress_unsafe(address)), count * ZPage::UNIT_SIZE);
}

FreeRegionManager::UnitIndex FreeRegionManager::UnitIndexOf(const ZVirtualMemory& vmem)
{
    const size_t index = ZPage::FindUnitIndex(untype(ZOffset::address_unsafe(vmem.start())));
    CHECK(index != std::numeric_limits<uint32_t>::max());
    return static_cast<UnitIndex>(index);
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

size_t FreeRegionManager::capacity() const
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t total = 0;
    for (const auto& partition : partitions) { total += partition->capacity; }
    return total;
}

void FreeRegionManager::InsertCommitted(Partition& partition, UnitIndex index, UnitCount count)
{
    // The reverse metadata array describes cached units as free (P05 keeps
    // the descriptor state machine; ZGC destroys the ZPage instead).
    ZPage::InitFreeRegion(index, count);
    partition.cache.insert(VirtualMemoryOf(index, count));
}

// ZPartition::free_memory (zPageAllocator.cpp:690-697): a freed page's vmem
// goes back to the mapped cache of the partition that owns its address.
void FreeRegionManager::FreeMemory(UnitIndex index, UnitCount count)
{
    const ZVirtualMemory vmem = VirtualMemoryOf(index, count);
    const uint32_t partitionId = virtualMemory->lookup_partition_id(vmem);
    Partition& partition = *partitions.at(partitionId);
    InsertCommitted(partition, index, count);
    // decrease_used
    CHECK(partition.used >= vmem.size());
    partition.used -= vmem.size();
}

void FreeRegionManager::AddGarbageUnits(UnitIndex index, UnitCount count, bool allowSaferegion)
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
    while (const auto* node = markQuarantineTree.RootNode()) {
        const UnitIndex index = node->GetIndex();
        const UnitCount units = node->GetCount();
        markQuarantineTree.ReleaseRootNode();
        FreeMemory(index, units);
        count += units;
    }
    return count;
}

// ZPartition::claim_capacity / claim_from_cache_or_increase_capacity,
// zPageAllocator.cpp:702-762.
bool FreeRegionManager::ClaimPageMemory(size_t num, PageMemory& memory)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    CHECK(num != 0 && num <= UINT32_MAX);
    const size_t size = num * ZPage::UNIT_SIZE;
    for (size_t visited = 0; visited < partitions.size(); ++visited) {
        const size_t selected = (nextPartition + visited) % partitions.size();
        Partition& partition = *partitions[selected];
        if (partition.available() < size) {
            // Out of memory in this partition
            continue;
        }

        // Try to allocate one contiguous vmem
        const ZVirtualMemory vmem = partition.cache.remove_contiguous(size);
        if (!vmem.is_null()) {
            memory.index = UnitIndexOf(vmem);
            memory.units = num;
            memory.partition = static_cast<uint32_t>(selected);
            memory.committed = true;
            memory.partialMappings.clear();
            memory.virtualClaimed = true;
            memory.harvestedUnits = 0;
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
        memory.units = num;
        memory.partition = static_cast<uint32_t>(selected);
        memory.committed = harvested == size;
        memory.virtualClaimed = false;
        memory.harvestedUnits = harvested / ZPage::UNIT_SIZE;
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
    const size_t size = memory.units * ZPage::UNIT_SIZE;
    const size_t harvested = memory.harvestedUnits * ZPage::UNIT_SIZE;
    if (harvested == 0) {
        // Just try to claim virtual memory
        const ZVirtualMemory vmem = claim_virtual(size, partitionId);
        if (vmem.is_null()) { return false; }
        memory.index = UnitIndexOf(vmem);
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

    memory.index = UnitIndexOf(result);
    memory.virtualClaimed = true;
    return true;
}

// alloc_page_inner (zPageAllocator.cpp:1470-1515): claim_physical_for_increased_capacity
// (:1761-1780), commit_and_map_single_partition (:1792-1806), map_committed
// (:1878-1887), cleanup_failed_commit_single_partition (:1906-1932), create_page.
ZPage* FreeRegionManager::MaterializePageMemory(PageMemory& memory, ZPageType role,
                                                     bool expectPhysicalMem, bool clearPayload, size_t& committedUnits,
                                                     PageAge age)
{
    (void)expectPhysicalMem;
    committedUnits = 0;
    // satisfied_from_cache_vmem: a contiguous cache hit is already committed and mapped.
    const bool fromCache = memory.virtualClaimed;
    if (!PreparePageMemory(memory)) { return nullptr; }
    const size_t idx = memory.index;
    const size_t num = memory.units;
    const uint32_t partitionId = memory.partition;
    committedUnits = fromCache ? num : 0;
    if (!fromCache) {
        // The vmem was built from harvested memory and/or increased capacity.
        const ZVirtualMemory vmem = VirtualMemoryOf(idx, num);
        const size_t alreadyCommitted = memory.harvestedUnits * ZPage::UNIT_SIZE;
        const ZVirtualMemory nonCommitted = vmem.last_part(alreadyCommitted);

        size_t committed = 0;
        if (nonCommitted.size() > 0) {
            // Claim physical memory for the increased capacity
            claim_physical(nonCommitted, partitionId);

            // Commit memory for the increased capacity
            committed = commit_physical(nonCommitted, partitionId);
            CHECK(committed <= nonCommitted.size() && committed % ZPage::UNIT_SIZE == 0);
        }
        const size_t totalCommitted = alreadyCommitted + committed;
        committedUnits = totalCommitted / ZPage::UNIT_SIZE;

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
    if ((fromCache || memory.harvestedUnits != 0) && clearPayload) {
        ZPage::ClearUnits(idx, num);
    }
    ZPage* region = ZPage::InitRegion(idx, num, role, age);
    if (!fromCache) {
        PrehandleReleasedUnit(clearPayload, idx, num);
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
    const size_t size = memory.units * ZPage::UNIT_SIZE;
    // Only decrease the overall used and not the generation used,
    // since the allocation failed and generation used wasn't bumped.
    CHECK(partition.used >= size);
    partition.used -= size;
    size_t freed = 0;
    for (const ZVirtualMemory vmem : memory.partialMappings) {
        freed += vmem.size();
        InsertCommitted(partition, UnitIndexOf(vmem), static_cast<UnitCount>(vmem.granule_count()));
    }
    memory.partialMappings.clear();
    const size_t remaining = size - freed;
    if (remaining > 0) {
        // Only a failed commit leaves capacity behind that could not be backed.
        const bool commitFailed = memory.virtualClaimed;
        decrease_capacity(memory.partition, remaining, commitFailed);
    }
}

FreeRegionManager::UnitCount FreeRegionManager::GetDirtyUnitCount() const
{
    // capacity == used + cached + claimed (ZPartition accounting).
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t bytes = 0;
    for (const auto& partition : partitions) {
        bytes += partition->capacity - partition->used - partition->claimed;
    }
    return static_cast<UnitCount>(bytes / ZPage::UNIT_SIZE);
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

void RegionManager::SetMaxUnitCountForRegion(size_t regionSize)
{
    maxUnitCountPerRegion = regionSize * KB / ZPage::UNIT_SIZE;
}

void RegionManager::SetMaxUnitCountForPinnedRegion(size_t regionSize)
{
    auto env = std::getenv("cjPinnedRegionSize");
    if (env == nullptr) {
        maxUnitCountPerPinnedRegion = maxUnitCountPerRegion;
        return;
    }
    size_t size = CString::ParseSizeFromEnv(env);
    // The minimum region size is system page size, measured in KB.
    size_t minSize = MapleRuntime::MRT_PAGE_SIZE / KB;
    if (size >= minSize && size <= regionSize) {
        maxUnitCountPerPinnedRegion = size * KB / ZPage::UNIT_SIZE;
    } else {
        LOG(RTLOG_ERROR, "Unsupported cjPinnedRegionSize parameter. Valid cjPinnedRegionSize"
            "range is [%zuKB, %zuKB].\n", minSize, regionSize);
    }
}

void RegionManager::SetLargeObjectThreshold(size_t configuredRegionSize)
{
    auto env = std::getenv("cjLargeThresholdSize");
    if (env == nullptr) {
        // default value is 32 KB
        largeObjectThreshold = 32 * KB;
    }
    size_t size = CString::ParseSizeFromEnv(env);
    // The minimum region size is system page size, measured in KB.
    size_t minSize = MapleRuntime::MRT_PAGE_SIZE / KB;
    // 64UL: The maximum region size, measured in KB, the value is 2048 KB.
    size_t maxSize = 10 * 1024UL;
    if (size >= minSize && size <= maxSize) {
        largeObjectThreshold = size * KB;
    } else if (size != 0) {
        LOG(RTLOG_ERROR, "Unsupported cjLargeThresholdSize parameter. Valid cjLargeThresholdSize"
            "range is [%zuKB, 2048KB].\n", minSize);
    }
    size_t regionSize = configuredRegionSize * KB;
    largeObjectThreshold = largeObjectThreshold > regionSize ? regionSize :  largeObjectThreshold;
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
std::vector<ZPage::UnitSegment> RegionManager::ReservedSegments(ZVirtualMemoryManager& virtualMemory)
{
    std::vector<ZPage::UnitSegment> segments;
    for (uint32_t partitionId = 0; partitionId < ZPerNUMAStorage::count(); ++partitionId) {
        ZArray<ZVirtualMemory> ranges;
        virtualMemory.remove_from_low_many_at_most(ZAddressOffsetMax, partitionId, &ranges);
        for (const ZVirtualMemory& range : ranges) {
            segments.push_back({ untype(ZOffset::address_unsafe(range.start())), range.size(), 0 });
            virtualMemory.insert(range, partitionId);
        }
    }
    std::sort(segments.begin(), segments.end(), [](const ZPage::UnitSegment& a,
                                                  const ZPage::UnitSegment& b) {
        return a.start < b.start;
    });
    return segments;
}

void RegionManager::Initialize(size_t nUnit, uintptr_t regionInfoAddr, ZVirtualMemoryManager& virtualMemory,
                               ZPhysicalMemoryManager& physicalMemory, const HeapParam& heapParam,
                               double garbageThreshold)
{
    // nUnit is the max capacity in units; the metadata spans the reserved
    // address range (ZVirtualToPhysicalRatio times larger).
    const ZVirtualMemory span = ReservedAddressSpan(virtualMemory);
    const std::vector<ZPage::UnitSegment> segments = ReservedSegments(virtualMemory);
    const size_t spanUnits = ZPage::IndexedUnitCount(segments);
    const size_t metadataSize = GetMetadataSize(spanUnits);
    this->regionHeapStart = segments.front().start;
    this->regionHeapEnd = segments.back().End();
    heapUnitCount = nUnit;
    CHECK(nUnit * ZPage::UNIT_SIZE <= span.size());
    this->inactiveZone = regionHeapStart;
    SetMaxUnitCountForRegion(heapParam.regionSize);
    SetMaxUnitCountForPinnedRegion(heapParam.regionSize);
    SetLargeObjectThreshold(heapParam.regionSize);
    SetGarbageThreshold(garbageThreshold);
#if defined(__EULER__)
    SetCacheRatio(0.0, 1.0, 1.0);
#endif
    // propagate region heap layout
    ZPage::InitializeSegments(regionInfoAddr + metadataSize, segments);
    freeRegionManager.Initialize(nUnit, virtualMemory, physicalMemory, nUnit * ZPage::UNIT_SIZE);
    this->exemptedRegionThreshold = heapParam.exemptionThreshold;
    DLOG(REPORT, "region info @0x%zx+%zu, heap [0x%zx, 0x%zx), unit count %zu", regionInfoAddr, metadataSize,
         regionHeapStart, regionHeapEnd, nUnit);
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
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    DLOG(REGION, "reclaim region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), 0u);

    // STEER3: scrub is at CollectRegion only (see header). Reclaim/TakeRegion reuse
    // must not re-scan O(N) under remset mutex.

    {
        ZPage::InPlaceClaimScope drain(region, ZForwarding::Retire::RECLAIM_DIRTY);
    }
    region->InitFreeUnits();
    ReturnPageMemory(PageMemory{ unitIndex, num, 0, true });
}

bool RegionManager::StallAllocation(AllocationStallRequest& request, bool requestGc)
{
    if (requestGc) {
        bool anotherWave = false;
        do {
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
            if (allocationStallBeforeWaveTestHook) {
                allocationStallBeforeWaveTestHook(*this);
            }
#endif
            const uint64_t waveBoundary = allocationStallQueue.CaptureWaveBoundary();
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
            if (allocationStallGcTestHook) {
                allocationStallGcTestHook(*this);
            } else
#endif
            {
                Heap::GetHeap().RequestGC(GC_REASON_OOM, false);
            }
            SatisfyStalledAllocations();
            anotherWave = allocationStallQueue.CompleteWave(waveBoundary);
        } while (anotherWave);
    }

    // zFuture.inline.hpp:47-53: a Java thread waits with a safepoint check;
    // here the mutator enters its saferegion before ZFuture::get (I3/I4).
    ScopedEnterSaferegion enterSaferegion(false);
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    if (allocationStallBeforeWaitTestHook) {
        allocationStallBeforeWaitTestHook(*this);
    }
#endif
    const bool satisfied = request.Wait();
    // Pair with the posting owner before the caller destroys its request.
    // zPageAllocator.cpp:1454-1464.
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    return satisfied;
}

void RegionManager::ReturnPageMemory(const PageMemory& memory)
{
    ZPage* region = Heap::page(ZPage::GetUnitAddress(memory.index));
    if (region != nullptr) {
        CHECK(region->GetUnitIdx() == memory.index && region->GetUnitCount() == memory.units);
        // Only materialized page geometry is retired. Its allocation-time
        // partial mappings have already been consumed; ZArray is non-copyable.
        const size_t index = memory.index;
        const size_t units = memory.units;
        const uint32_t partition = memory.partition;
        const bool committed = memory.committed;
        ZPage::RetirePage(region, [this, region, index, units, partition, committed] {
            region->InitFreeUnits();
            ReturnRetiredPageMemory(PageMemory{index, units, partition, committed});
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
    freeRegionManager.AddGarbageUnits(memory.index, memory.units, allowSaferegion);
    const size_t bytes = memory.units * ZPage::UNIT_SIZE;
    CHECK(pageAllocatorUsed >= bytes);
    pageAllocatorUsed -= bytes;
    TrackUsedPeakLocked();
    allocationStallQueue.SatisfyAvailableLocked([this](AllocationStallRequest& request) {
        return ClaimAllocationLocked(request);
    });
}



void RegionManager::SatisfyStalledAllocations()
{
    // zPageAllocator.cpp:2167: claim the actual resource and update used
    // under the ordinary allocation owner, then dequeue and notify.
    allocationStallQueue.SatisfyAvailable([this](AllocationStallRequest& request) {
        return ClaimAllocationLocked(request);
    });
}

bool RegionManager::ClaimAllocationLocked(AllocationStallRequest& request)
{
    const size_t size = request.GetSize();
    const size_t num = size / ZPage::UNIT_SIZE;
    PageMemory& memory = request.Memory();
    if (!freeRegionManager.ClaimPageMemory(num, memory)) { return false; }
    pageAllocatorUsed += size;
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
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    DLOG(REGION, "mark-quarantine region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
         region->GetRegionAllocatedSize(), region->GetRegionEnd(), 0u);
    {
        ZPage::InPlaceClaimScope drain(region, ZForwarding::Retire::RECLAIM_MARK_QUARANTINE);
    }
    region->InitFreeUnits();
    ScopedEnterSaferegion enterSaferegion(true);
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    freeRegionManager.AddMarkQuarantineUnits(unitIndex, num);
    CHECK(pageAllocatorUsed >= num * ZPage::UNIT_SIZE);
    pageAllocatorUsed -= num * ZPage::UNIT_SIZE;
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

    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    // Large regions above the release threshold bypass CollectRegion. Invalidate
    DLOG(REGION, "release region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), 0u);

    {
        ZPage::InPlaceClaimScope drain(region, ZForwarding::Retire::RELEASE_REGION);
    }
    region->InitFreeUnits();
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
                                       bool allowSaferegion, bool clearPayload, PageAge age)
{
    size_t size = num * ZPage::UNIT_SIZE;
    if (allowSaferegion) {
        RequestForRegion(size);
    }

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
    ZPageAllocation request(size, static_cast<uint8_t>(type), expectPhysicalMem, clearPayload);
    bool claimed = false;
    bool requestGc = false;
    {
        std::lock_guard<std::mutex> lock(pageAllocatorMutex);
        claimed = ClaimAllocationLocked(request);
        if (!claimed && allowSaferegion && !IsGcThread()) {
            requestGc = allocationStallQueue.EnqueueLocked(request);
        }
    }
    if (!claimed && allowSaferegion && !IsGcThread()) {
        claimed = StallAllocation(request, requestGc);
    }
    if (claimed) {
        size_t committedUnits = 0;
        ZPage* region = freeRegionManager.MaterializePageMemory(
            request.Memory(), type, request.ExpectsPhysicalMemory(), request.ClearsPayload(), committedUnits, age);
        if (request.Memory().virtualClaimed) {
            std::lock_guard<std::mutex> lock(pageAllocatorMutex);
            const uintptr_t end = ZPage::GetUnitAddress(request.Memory().index) + size;
            inactiveZone.store(std::max(inactiveZone.load(std::memory_order_relaxed), end), std::memory_order_release);
        }
        if (region == nullptr) {
            std::unique_ptr<ScopedEnterSaferegion> enterSaferegion;
            if (allowSaferegion) { enterSaferegion.reset(new ScopedEnterSaferegion(true)); }
            std::lock_guard<std::mutex> lock(pageAllocatorMutex);
            (void)committedUnits;
            freeRegionManager.FreeMemoryAllocFailed(request.Memory());
            CHECK(pageAllocatorUsed >= size);
            pageAllocatorUsed -= size;
            TrackUsedPeakLocked();
            allocationStallQueue.SatisfyAvailableLocked([this](ZPageAllocation& pending) {
                return ClaimAllocationLocked(pending);
            });
            if (allowSaferegion) {
                goto retry;
            }
            return nullptr;
        }
        ZStatInc(ZStatMutatorAllocRate::counter(), size);
    ZStatMutatorAllocRate::sample_allocation(size);
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

size_t RegionManager::CollectFreePinnedSlots(ZPage* region)
{
    // pinroot: raw-pointer pin is a liveness hold — do not free any slot while count > 0.
    // AddRawPointerObject only bumps this counter (no mark bit / root set); reclaim must honour it.
    if (region->GetRawPointerObjectCount() > 0) {

        return 0;
    }
    // traverse pinned region to reclaim free pinned objects.
    size_t garbageSize = 0;
    region->VisitAllObjects([this, region, &garbageSize](BaseObject* object) {
        if (!region->is_object_live(from_object(object))) {
            size_t objSize = object->GetSize();
            DLOG(ALLOC, "reclaim pinned obj %p<%p>(%zu)", object, object->GetTypeInfo(), objSize);
            garbageSize += objSize;
            std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
            ReleaseNativeResource(object);
            freePinnedSlotLists.PushFront(object);
        }
    });
    return garbageSize;
}

size_t RegionManager::CollectPinnedGarbage()
{

    {
        std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
        freePinnedSlotLists.Clear();
    }
    size_t garbageSize = 0;
    // #710: pinned pages are page-table entries with a pinned role
    // (zPageTable.hpp:57-77 walk), not a list.
    std::vector<ZPage*> pinnedPages;
    {
        ZPage::SafeDestroyScope scope;
        ZPageTableIterator iter(&ZPageTable::heap_table());
        for (ZPage* region; iter.next(&region);) {
            if (region->GetRegionRole() == ZPageRole::OldPinned) {
                pinnedPages.push_back(region);
            }
        }
    }
    for (ZPage* region : pinnedPages) {
        // pinroot: whole-region reclaim also ignores pins; skip while any raw pointer holds.
        if (region->GetRawPointerObjectCount() > 0) {
            continue;
        }
        if (region->IsKnownEmpty()) {
            ZPage* del = region;
            del->SetRegionRole(ZPageRole::None);

            auto fixToObj = [](BaseObject* obj) { ReleaseNativeResource(obj); };
            del->VisitAllObjects(fixToObj);


            garbageSize += CollectRegion<Generation::Old>(del);
            continue;
        } else {
            garbageSize += CollectFreePinnedSlots(region);
        }
    }

    return garbageSize;
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

void RegionManager::Init(const HeapParam& vmHeapParam)
{
    size_t heapSize = 0;
    CHECK_DETAIL(CheckedMulSize(vmHeapParam.heapSize, size_t{1024}, heapSize),
                 "heap size overflows bytes before reservation: heapSizeKB=%zu", vmHeapParam.heapSize);
    const size_t unitNum = RegionManager::GetHeapUnitCount(heapSize);
    const size_t maxCapacity = unitNum * ZPage::UNIT_SIZE;

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
    // It is committed lazily by the kernel: only units that ever become pages
    // touch their descriptor.
    const std::vector<ZPage::UnitSegment> segments = RegionManager::ReservedSegments(*virtualMemory);
    metadata.size = RegionManager::GetMetadataSize(ZPage::IndexedUnitCount(segments));
    void* const metadataBase =
        mmap(nullptr, metadata.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK_DETAIL(metadataBase != MAP_FAILED, "failed to map %zu bytes of region metadata", metadata.size);
    metadata.base = metadataBase;
    Logger::GetLogger().SetMinimumLogLevel(CangjieRuntime::GetLogParam().logLevel);
    MAddress metadataAddress = reinterpret_cast<MAddress>(metadata.base);
    CHECK(IsRepresentableLow48Range(metadataAddress, metadata.size));
    Initialize(unitNum, metadataAddress, *virtualMemory, *physicalMemory, vmHeapParam,
                             CangjieRuntime::GetGCParam().garbageThreshold);
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


void RegionManager::AddRawPointerObject(BaseObject* obj)
    {
        // Pin needs a plain load-good address. High colour bits ⇒ missing barrier
        // at the call site (would OOB in GetUnitIdxAt; fail closed here).
        MAddress rawAddr = reinterpret_cast<MAddress>(obj);
        CHECK(rawAddr == 0 || (rawAddr >> 48) == 0);
        ZPage* region = Heap::page(rawAddr);
        region->IncRawPointerObjectCount();

        // CSet empty-free (select_relocation_set) claims FROM under the same
        // role word (zGeneration.cpp:211-221 register_empty_page). Inc first so
        // a GC that already claimed GARBAGE still sees rawPtrCnt>0. The claim
        // is a role CAS; a lost race leaves the page lone, which is the same
        // skip as ZGC's !is_relocatable.
        for (;;) {
            ZPageRole role = region->GetRegionRole();
            if (role == ZPageRole::From || role == ZPageRole::Garbage) {
                ZPageRole expect = role;
                if (region->CASRegionRole(expect, ZPageRole::RawPointerPinned)) {
                    ZGeneration* generation = region->IsYoungRegion()
                        ? static_cast<ZGeneration*>(ZGeneration::young())
                        : static_cast<ZGeneration*>(ZGeneration::old());
                    CHECK(generation == nullptr || !generation->is_phase_relocate());
                    break;
                }
                std::this_thread::yield();
                continue;
            }
            CHECK(!region->IsLoneFromRegion());
            break;
        }
    }

void RegionManager::RemoveRawPointerObject(BaseObject* obj)
    {
        MAddress rawAddr = reinterpret_cast<MAddress>(obj);
        CHECK(rawAddr == 0 || (rawAddr >> 48) == 0);
        ZPage* region = Heap::page(rawAddr);
        region->DecRawPointerObjectCount();
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
                               ZStatMutatorAllocRate::soft_max_heap_size(),
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


void FreeRegionManager::AddMarkQuarantineUnits(UnitIndex idx, UnitCount num)
{
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(markQuarantineTreeMutex);
        if (UNLIKELY(!markQuarantineTree.MergeInsert(idx, num, true))) {
            LOG(RTLOG_FATAL, "tid %d: failed to add mark-quarantine units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }


void RegionManager::MergeRawPointerPinnedRegions()
{
    ZPage::SafeDestroyScope scope;
    ZPageTableIterator iter(&ZPageTable::heap_table());
    for (ZPage* region; iter.next(&region);) {
        if (region->GetRegionRole() == ZPageRole::RawPointerPinned) {
            region->SetRegionRole(ZPageRole::OldPinned);
        }
    }
}

size_t RegionManager::GetLargeObjectSize() const
{
    size_t bytes = 0;
    ZPage::SafeDestroyScope scope;
    ZPageTableIterator iter(&ZPageTable::heap_table());
    for (ZPage* region; iter.next(&region);) {
        const ZPageRole role = region->GetRegionRole();
        if (role == ZPageRole::OldLarge || role == ZPageRole::RecentLarge || role == ZPageRole::LargeTrace) {
            bytes += region->GetUnitCount() * ZPage::UNIT_SIZE;
        }
    }
    return bytes;
}

bool RegionManager::TryStampTraceRegion(ZPage* region, ZPageRole role)
{
    std::lock_guard<std::mutex> lock(pinnedAllocationMutex);
    const bool active = role == ZPageRole::FullTrace ? fullTraceCacheActive : largeTraceCacheActive;
    if (!active) {
        return false;
    }
    region->SetRegionRole(role);
    return true;
}

void RegionManager::LockPageMutexInSaferegion(std::mutex& listMutex)
    {
        while (!listMutex.try_lock()) {
            ScopedEnterSaferegion enterSaferegion(true);
        }
    }

}
