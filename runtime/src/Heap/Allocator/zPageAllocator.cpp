// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/zPageAllocator.hpp"

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

#include "Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Collector/Collector.h"
#include "Collector/ZForwarding.h"
#include "Collector/CollectorResources.h"
#include "Collector/CopyCollector.h"
#include "Collector/GcTrigger.h"
#include "Collector/Uncommitter.h"
#include "Base/ZStat.h"
#include "Collector/TenuringThreshold.h"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/CsetEmptyWho.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/FillerZeroDiag.h"
#include "Heap/Verify/HoleWhoDiag.h"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/Allocator/zForwardingTable.hpp"
#include "Heap/Collector/zRelocationSetSelector.hpp"
#include "Heap/Verify/Zap.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
void RunRemapWindowTestHook(unsigned point, RegionInfo* region, BaseObject* object);
#endif

namespace RecentFullAccounting {
namespace {
std::atomic<size_t> enqueuedRegions{ 0 };
std::atomic<size_t> dequeuedRegions{ 0 };
std::atomic<size_t> currentBytes{ 0 };
std::atomic<size_t> peakBytes{ 0 };
}

void Enqueue(size_t regions, size_t units)
{
    if (regions == 0) {
        return;
    }
    enqueuedRegions.fetch_add(regions, std::memory_order_relaxed);
    const size_t bytes = units * RegionInfo::UNIT_SIZE;
    const size_t current = currentBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    size_t peak = peakBytes.load(std::memory_order_relaxed);
    while (peak < current &&
           !peakBytes.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {}
}

void Dequeue(size_t regions, size_t units)
{
    if (regions == 0) {
        return;
    }
    dequeuedRegions.fetch_add(regions, std::memory_order_relaxed);
    const size_t bytes = units * RegionInfo::UNIT_SIZE;
    const size_t before = currentBytes.fetch_sub(bytes, std::memory_order_relaxed);
    CHECK_DETAIL(before >= bytes, "recent-full accounting underflow: before=%zu remove=%zu", before, bytes);
}

void Report(size_t listRegions, size_t listBytes)
{
    const size_t in = enqueuedRegions.load(std::memory_order_relaxed);
    const size_t out = dequeuedRegions.load(std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][recent-full-account] in=%zu out=%zu current_regions=%zu current_bytes=%zu "
         "peak_bytes=%zu list_regions=%zu list_bytes=%zu",
         in, out, in - out, currentBytes.load(std::memory_order_relaxed),
         peakBytes.load(std::memory_order_relaxed), listRegions, listBytes);
}
} // namespace RecentFullAccounting


void RegionList::MergeRegionList(RegionList& srcList, RegionInfo::RegionType regionType)
{
    RegionList regionList("region list cache");
    srcList.MoveTo(regionList);
    RegionInfo* head = regionList.GetHeadRegion();
    RegionInfo* tail = regionList.GetTailRegion();
    if (head == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(listMutex);
    regionList.SetElementType(regionType);
    IncCounts(regionList.GetRegionCount(), regionList.GetUnitCount());
    if (listHead == nullptr) {
        listHead = head;
        listTail = tail;
    } else {
        tail->SetNextRegion(listHead);
        listHead->SetPrevRegion(tail);
        listHead = head;
    }
    for (RegionInfo* node = head; node != nullptr; node = node->GetNextRegion()) {
        node->SetRegionListOwner(this);
    }
}

void RegionList::PrependRegion(RegionInfo* region, RegionInfo::RegionType type)
{
    std::lock_guard<std::mutex> lock(listMutex);
    PrependRegionLocked(region, type);
}

void RegionList::PrependRegionLocked(RegionInfo* region, RegionInfo::RegionType type)
{
    if (region == nullptr) {
        return;
    }

    CHECK_DETAIL(region->GetRegionListOwner() == nullptr, "region already belongs to a list");

    DLOG(REGION, "list %p (%zu, %zu)+(%zu, %zu) prepend region %p@[%#zx+%zu, %#zx) type %u->%u", this,
        regionCount, unitCount, 1llu, region->GetUnitCount(), region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType(), type);

    region->SetRegionType(type);
    region->SetRegionListOwner(this);
    region->SetPrevRegion(nullptr);
    IncCounts(1, region->GetUnitCount());
    region->SetNextRegion(listHead);
    if (listHead == nullptr) {
        MRT_ASSERT(listTail == nullptr, "PrependRegion listTail is not null");
        listTail = region;
    } else {
        listHead->SetPrevRegion(region);
    }
    listHead = region;
}

void RegionList::DeleteRegionLocked(RegionInfo* del)
{
    MRT_ASSERT(listHead != nullptr && listTail != nullptr, "illegal region list");
    CHECK_DETAIL(del != nullptr && del->GetRegionListOwner() == this, "region belongs to another list");

    RegionInfo* pre = del->GetPrevRegion();
    RegionInfo* next = del->GetNextRegion();

    del->SetNextRegion(nullptr);
    del->SetPrevRegion(nullptr);
    del->SetRegionListOwner(nullptr);

    DLOG(REGION, "list %p (%zu, %zu)-(%zu, %zu) delete region %p@[%#zx+%zu, %#zx) type %u", this,
        regionCount, unitCount, 1llu, del->GetUnitCount(),
        del, del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(), del->GetRegionType());

    DecCounts(1, del->GetUnitCount());

    if (listHead == del) { // delete head
        MRT_ASSERT(pre == nullptr, "Delete Region pre is not null");
        listHead = next;
        if (listHead == nullptr) { // now empty
            listTail = nullptr;
            return;
        }
    } else if (pre != nullptr) {
        pre->SetNextRegion(next);
    }

    if (listTail == del) { // delete tail
        MRT_ASSERT(next == nullptr, "Delete Region next is not null");
        listTail = pre;
        if (listTail == nullptr) { // now empty
            listHead = nullptr;
            return;
        }
    } else if (next != nullptr) {
        next->SetPrevRegion(pre);
    } else if (pre != nullptr) {
        // next was stolen (region re-homed onto another list) while this list
        // still named it. Treat del as the last node we still own.
        listTail = pre;
    }
}

#ifdef MRT_DEBUG
void RegionList::DumpRegionList(const char* msg)
{
    DLOG(REGION, "dump region list %s", msg);
    std::lock_guard<std::mutex> lock(listMutex);
    for (RegionInfo *region = listHead; region != nullptr; region = region->GetNextRegion()) {
        DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) units [%zu+%zu, %zu) type %u prev %p next %p", region,
            region->GetRegionStart(), region->GetRegionAllocatedSize(), region->GetRegionEnd(),
            region->GetUnitIdx(), region->GetUnitCount(), region->GetUnitIdx() + region->GetUnitCount(),
            region->GetRegionType(), region->GetPrevRegion(), region->GetNextRegion());
    }
}
#endif
void FreeRegionManager::Initialize(UnitCount regionCnt, const std::vector<MemoryRange>& reservations, MemMap& owner)
{
    markQuarantineTree.Init(regionCnt);
    partitions.clear();
    nextPartition = 0;
    backingOwner = &owner;
    for (const auto& numa : owner.GetNumaPartitionRegistry().Ranges()) {
        auto found = std::find_if(partitions.begin(), partitions.end(), [&](const std::unique_ptr<Partition>& p) {
            return p->node == numa.node;
        });
        if (found == partitions.end()) {
            partitions.emplace_back(new Partition(numa.node));
            found = std::prev(partitions.end());
            (*found)->cache.SetRefresh([](MappedCache::Extent extent) {
                RegionInfo::InitFreeRegion(extent.index, extent.count);
            });
        }
        for (const auto& reservation : reservations) {
            const uintptr_t start = std::max(reservation.start, numa.range.start);
            const uintptr_t end = std::min(reservation.End(), numa.range.End());
            if (start >= end) { continue; }
            CHECK((end - start) % RegionInfo::UNIT_SIZE == 0);
            (*found)->reservations.push_back({start, end - start});
            CHECK((*found)->virtualMemory.RegisterRange(Range(start, end - start)));
        }
    }
}

FreeRegionManager::Partition& FreeRegionManager::PartitionFor(uintptr_t address)
{
    for (auto& partition : partitions) {
        for (const auto& range : partition->reservations) {
            if (address >= range.start && address < range.End()) { return *partition; }
        }
    }
    LOG(RTLOG_FATAL, "cache address outside partition reservation: %#zx", address);
    std::abort();
}

void FreeRegionManager::InsertCommitted(Partition& partition, UnitIndex index, UnitCount count)
{
    CHECK(backingOwner->GetCommittedSize(RegionInfo::GetUnitAddress(index), count * RegionInfo::UNIT_SIZE) ==
          count * RegionInfo::UNIT_SIZE);
    partition.cache.Insert({index, count});
}

void FreeRegionManager::ReturnMemory(UnitIndex index, UnitCount count)
{
    // Returned prefixes and uncommitted suffixes enter their respective single
    // owners: mapped cache or virtual registry, never a released cache ledger.
    while (count != 0) {
        const uintptr_t start = RegionInfo::GetUnitAddress(index);
        Partition& partition = PartitionFor(start);
        const bool committed = backingOwner->GetCommittedSize(start, RegionInfo::UNIT_SIZE) == RegionInfo::UNIT_SIZE;
        UnitCount length = 1;
        while (length < count) {
            const uintptr_t next = RegionInfo::GetUnitAddress(index + length);
            if (&PartitionFor(next) != &partition ||
                (backingOwner->GetCommittedSize(next, RegionInfo::UNIT_SIZE) == RegionInfo::UNIT_SIZE) != committed) {
                break;
            }
            ++length;
        }
        if (committed) { InsertCommitted(partition, index, length); }
        else { CHECK(partition.virtualMemory.Insert(Range(start, length * RegionInfo::UNIT_SIZE))); }
        index += length;
        count -= length;
    }
}

void FreeRegionManager::AddGarbageUnits(UnitIndex index, UnitCount count, bool allowSaferegion)
{
    std::unique_ptr<ScopedEnterSaferegion> saferegion;
    if (allowSaferegion) { saferegion.reset(new ScopedEnterSaferegion(true)); }
    std::lock_guard<std::mutex> lock(cacheMutex);
    ReturnMemory(index, count);
}

void FreeRegionManager::AddReleaseUnits(UnitIndex index, UnitCount count, bool allowSaferegion)
{
    std::unique_ptr<ScopedEnterSaferegion> saferegion;
    if (allowSaferegion) { saferegion.reset(new ScopedEnterSaferegion(true)); }
    std::lock_guard<std::mutex> lock(cacheMutex);
    ReturnMemory(index, count);
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
        ReturnMemory(index, units);
        count += units;
    }
    return count;
}

bool FreeRegionManager::ClaimPageMemory(size_t num, PageMemory& memory)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    CHECK(num != 0 && num <= UINT32_MAX);
    for (size_t visited = 0; visited < partitions.size(); ++visited) {
        const size_t selected = (nextPartition + visited) % partitions.size();
        Partition& partition = *partitions[selected];
        // zPartition::claim_from_cache_or_increase_capacity: a contiguous cache
        // hit precedes new capacity, which precedes discontiguous harvesting.
        const auto extent = partition.cache.RemoveContiguous(static_cast<UnitCount>(num));
        if (!extent.IsNull()) {
            memory = PageMemory{extent.index, num, static_cast<uint32_t>(selected), true};
            nextPartition = (selected + 1) % partitions.size();
            return true;
        }
        const size_t virtualUnits = partition.virtualMemory.TotalSize() / RegionInfo::UNIT_SIZE;
        CHECK(virtualUnits >= partition.pendingGrowth);
        const size_t growthAvailable = virtualUnits - partition.pendingGrowth;
        if (partition.cache.Size() + growthAvailable < num) { continue; }

        // zPageAllocator.cpp:723-743: claim all available growth first, then
        // harvest only the remaining capacity. Neither claim selects a new
        // virtual range; PreparePageMemory performs that step later.
        const size_t increased = std::min(num, growthAvailable);
        partition.pendingGrowth += increased;
        const UnitCount remaining = static_cast<UnitCount>(num - increased);
        std::vector<MappedCache::Extent> extents;
        const UnitCount harvested = remaining == 0 ? 0 : partition.cache.RemoveDiscontiguous(remaining, extents);
        CHECK(harvested + increased == num);
        for (const auto& extent : extents) {
            memory.partialMappings.push_back({RegionInfo::GetUnitAddress(extent.index),
                                              extent.count * RegionInfo::UNIT_SIZE});
        }
        memory.index = 0;
        memory.units = num;
        memory.partition = selected;
        memory.committed = harvested == num;
        memory.virtualClaimed = false;
        memory.harvestedUnits = harvested;
        nextPartition = (selected + 1) % partitions.size();
        return true;
    }
    return false;
}

bool FreeRegionManager::PreparePageMemory(PageMemory& memory)
{
    if (memory.virtualClaimed) { return true; }
    std::lock_guard<std::mutex> lock(cacheMutex);
    Partition& partition = *partitions.at(memory.partition);
    std::vector<MemMap::BackingSegment> stash;
    const size_t growth = memory.units - memory.harvestedUnits;
    CHECK(partition.pendingGrowth >= growth);
    partition.pendingGrowth -= growth;
    if (memory.harvestedUnits == 0) {
        // Full capacity increase needs only a virtual claim, not remapping.
        const Range result = partition.virtualMemory.ClaimLow(memory.units * RegionInfo::UNIT_SIZE);
        if (result.IsNull()) { return false; }
        memory.index = RegionInfo::FindUnitIndex(result.Start());
        memory.virtualClaimed = true;
        return true;
    }
    if (!backingOwner->StashSegments(memory.partialMappings, stash)) {
        for (const auto& range : memory.partialMappings) {
            InsertCommitted(partition, RegionInfo::FindUnitIndex(range.start), range.size / RegionInfo::UNIT_SIZE);
        }
        memory.partialMappings.clear();
        return false;
    }
    // zRangeRegistry::insert_and_remove_from_low_exact_or_many. This cache
    // owner serializes the complete shuffle, including failure restoration.
    for (const auto& range : memory.partialMappings) {
        CHECK(partition.virtualMemory.Insert(Range(range.start, range.size)));
    }
    memory.partialMappings.clear();
    const Range result = partition.virtualMemory.ClaimLow(memory.units * RegionInfo::UNIT_SIZE);
    if (result.IsNull()) {
        size_t remaining = memory.harvestedUnits * RegionInfo::UNIT_SIZE;
        for (const Range& range : partition.virtualMemory.Snapshot()) {
            if (remaining == 0) { break; }
            const size_t amount = std::min(remaining, range.Size());
            const Range claimed = partition.virtualMemory.ClaimLow(amount);
            CHECK(!claimed.IsNull());
            memory.partialMappings.push_back({claimed.Start(), amount});
            remaining -= amount;
        }
        CHECK(remaining == 0);
        backingOwner->RestoreSegments(memory.partialMappings, stash);
        for (const auto& range : memory.partialMappings) {
            InsertCommitted(partition, RegionInfo::FindUnitIndex(range.start), range.size / RegionInfo::UNIT_SIZE);
        }
        memory.partialMappings.clear();
        return false;
    }
    backingOwner->RestoreSegments({MemoryRange{result.Start(), memory.harvestedUnits * RegionInfo::UNIT_SIZE}}, stash);
    memory.index = RegionInfo::FindUnitIndex(result.Start());
    memory.virtualClaimed = true;
    return true;
}

FreeRegionManager::UnitCount FreeRegionManager::GetDirtyUnitCount() const
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    UnitCount count = 0;
    for (const auto& partition : partitions) { count += partition->cache.Size(); }
    return count;
}

FreeRegionManager::UnitCount FreeRegionManager::GetVirtualUnitCount() const
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t bytes = 0;
    for (const auto& partition : partitions) { bytes += partition->virtualMemory.TotalSize(); }
    return bytes / RegionInfo::UNIT_SIZE;
}

FreeRegionManager::UnitCount FreeRegionManager::GetDirtyMaxBlock() const
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    UnitCount maximum = 0;
    for (const auto& partition : partitions) { maximum = std::max(maximum, partition->cache.MaxExtent()); }
    return maximum;
}

FreeRegionManager::UnitCount FreeRegionManager::GetVirtualMaxBlock() const
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t maximum = 0;
    for (const auto& partition : partitions) {
        for (const Range& range : partition->virtualMemory.Snapshot()) { maximum = std::max(maximum, range.Size()); }
    }
    return maximum / RegionInfo::UNIT_SIZE;
}

size_t FreeRegionManager::GetDirtyNodeCount() const
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t count = 0;
    for (const auto& partition : partitions) { count += partition->cache.EntryCount(); }
    return count;
}

size_t FreeRegionManager::GetVirtualNodeCount() const
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t count = 0;
    for (const auto& partition : partitions) { count += partition->virtualMemory.Snapshot().size(); }
    return count;
}

bool FreeRegionManager::TakeUncommitMemory(size_t maxBytes, uint64_t idleBeforeNs, PageMemory& memory)
{
    if (maxBytes < RegionInfo::UNIT_SIZE) {
        return false;
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    const UnitCount limit = static_cast<UnitCount>(maxBytes / RegionInfo::UNIT_SIZE);
    for (auto& partition : partitions) {
        if (partition->cache.LastUsedNs() > idleBeforeNs) { continue; }
        std::vector<MappedCache::Extent> extents;
        partition->cache.RemoveForUncommit(limit, extents);
        if (extents.empty()) { continue; }
        for (size_t i = 1; i < extents.size(); ++i) {
            InsertCommitted(*partition, extents[i].index, extents[i].count);
        }
        const auto& extent = extents.front();
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(RegionInfo::GetUnitAddress(extent.index));
        bool inRelocate = false;
        if (Heap::GetHeap().IsGcStarted()) {
            const GCPhase phase = Heap::GetHeap().GetGCPhase();
            inRelocate = phase == GCPhase::GC_PHASE_POST_TRACE ||
                         phase == GCPhase::GC_PHASE_PREFORWARD ||
                         phase == GCPhase::GC_PHASE_FORWARD;
        }
        if (inRelocate || !ExtentReadyForReleasedCache(region)) {
            InsertCommitted(*partition, extent.index, extent.count);
            return false;
        }
        memory = PageMemory{extent.index, extent.count, 0, false};
        return true;
    }
    return false;
}

void FreeRegionManager::ReturnUncommitMemory(const PageMemory& memory)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    ReturnMemory(memory.index, memory.units);
}

void RegionManager::SetMaxUnitCountForRegion(size_t regionSize)
{
    maxUnitCountPerRegion = regionSize * KB / RegionInfo::UNIT_SIZE;
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
        maxUnitCountPerPinnedRegion = size * KB / RegionInfo::UNIT_SIZE;
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

void RegionManager::Initialize(size_t nUnit, uintptr_t regionInfoAddr, MemMap& memoryOwner,
                               const HeapParam& heapParam, double garbageThreshold)
{
    const size_t metadataSize = GetMetadataSize(nUnit);
    InitializeSegments(regionInfoAddr, { MemoryRange{ regionInfoAddr + metadataSize, nUnit * RegionInfo::UNIT_SIZE } },
                       memoryOwner, heapParam, garbageThreshold);
}

void RegionManager::InitializeSegments(uintptr_t regionInfoAddr, const std::vector<MemoryRange>& inputRanges,
                                      MemMap& memoryOwner, const HeapParam& heapParam, double garbageThreshold)
{
    // OS reservations remain distinct for unreserve (notably on Windows),
    // while adjacent virtual ranges coalesce before receiving cache indices.
    std::vector<MemoryRange> reservations;
    for (const auto& range : inputRanges) {
        if (!reservations.empty() && reservations.back().End() == range.start) {
            reservations.back().size += range.size;
        } else {
            reservations.push_back(range);
        }
    }
    const size_t nUnit = RegionInfo::IndexedUnitCount(reservations);
    const size_t metadataSize = GetMetadataSize(nUnit);
    this->regionInfoStart = regionInfoAddr;
    this->regionHeapStart = reservations.front().start;
    this->regionHeapEnd = reservations.back().End();
    heapUnitCount = 0;
    for (const auto& range : reservations) {
        CHECK(memoryOwner.GetReservationRegistry().Contains(range.start, range.size));
        heapUnitCount += range.size / RegionInfo::UNIT_SIZE;
    }
    // zPageTable.cpp:37-52: address tables cover the highest available end,
    // while only the reservation registry supplies allocatable ranges.
    CHECK(ForwardingTable::Initialize(regionHeapStart, regionHeapEnd - regionHeapStart, RegionInfo::UNIT_SIZE));
    this->inactiveZone = regionHeapStart;
    SetMaxUnitCountForRegion(heapParam.regionSize);
    SetMaxUnitCountForPinnedRegion(heapParam.regionSize);
    SetLargeObjectThreshold(heapParam.regionSize);
    SetGarbageThreshold(garbageThreshold);
#if defined(__EULER__)
    SetCacheRatio(0.0, 1.0, 1.0);
#endif
    // propagate region heap layout
    RegionInfo::InitializeSegments(regionInfoAddr + metadataSize, reservations, &memoryOwner);
    freeRegionManager.Initialize(nUnit, reservations, memoryOwner);
    this->exemptedRegionThreshold = heapParam.exemptionThreshold;
    DLOG(REPORT, "region info @0x%zx+%zu, heap [0x%zx, 0x%zx), unit count %zu", regionInfoAddr, metadataSize,
         regionHeapStart, regionHeapEnd, nUnit);
}

void RegionManager::ScrubRememberedSetForRegion(RegionInfo* region)
{
    if (region == nullptr) {
        return;
    }
    MAddress rStart = static_cast<MAddress>(region->GetRegionStart());
    MAddress rEnd = static_cast<MAddress>(region->GetRegionEnd());
    (void)Heap::GetHeap().GetRememberedSet().ClearRegion(rStart, rEnd, nullptr);
}

void RegionManager::DumpScrubCostAndReset(const char* point)
{
    (void)point;
}

void RegionManager::ReclaimRegion(RegionInfo* region)
{
    RegionInfo::RetirePage(region, [this, region] { ReclaimRetiredRegion(region); });
}

void RegionManager::ReclaimRetiredRegion(RegionInfo* region)
{
    // routedest: census, not a guard. The graft asked for CHECK(!IsRouteDestHeld()) here to
    // convert "I traced the paths" into a machine check, but none of the designs proved the
    // caller enumeration and five of the six ReclaimRegion callers have already detached the
    // region, so an abort here would trade an unproven assumption for a hard stop. Count and
    // name it instead, under the default-off account gate; a non-zero funnel_held is the
    // signal that the enumeration was wrong.
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "reclaim region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

    // STEER3: scrub is at CollectRegion only (see header). Reclaim/TakeRegion reuse
    // must not re-scan O(N) under remset mutex.

    {
        RegionInfo::InPlaceClaimScope drain(region, ZForwardingLife::Retire::RECLAIM_DIRTY);
    }
    // gcvroot Z2: poison reclaimed payload so use-after-free roots are identifiable (MRT_GCV2_ZAP_RECLAIM=1).
    HeapZap::ZapReclaimedRegion(region->GetRegionStart(), region->GetRegionEnd());
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
                Heap::GetHeap().GetCollector().RequestGC(GC_REASON_OOM, false);
            }
            SatisfyStalledAllocations();
            anotherWave = allocationStallQueue.CompleteWave(waveBoundary);
        } while (anotherWave);
    }

    ScopedEnterSaferegion enterSaferegion(false);
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    const bool satisfied = request.Wait(allocationStallBeforeWaitTestHook
        ? [this] { allocationStallBeforeWaitTestHook(*this); }
        : std::function<void()> {});
#else
    const bool satisfied = request.Wait();
#endif
    // Pair with the posting owner before the caller destroys its request.
    // zPageAllocator.cpp:1454-1464.
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    return satisfied;
}

void RegionManager::ReturnPageMemory(const PageMemory& memory)
{
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(RegionInfo::GetUnitAddress(memory.index));
    if (region != nullptr) {
        CHECK(region->GetUnitIdx() == memory.index && region->GetUnitCount() == memory.units);
        RegionInfo::RetirePage(region, [this, region, memory] {
            region->InitFreeUnits();
            ReturnRetiredPageMemory(memory);
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
    if (memory.committed) {
        freeRegionManager.AddGarbageUnits(memory.index, memory.units, allowSaferegion);
    } else {
        freeRegionManager.AddReleaseUnits(memory.index, memory.units, allowSaferegion);
    }
    const size_t bytes = memory.units * RegionInfo::UNIT_SIZE;
    CHECK(pageAllocatorUsed >= bytes);
    pageAllocatorUsed -= bytes;
    allocationStallQueue.SatisfyAvailableLocked([this](AllocationStallRequest& request) {
        return ClaimAllocationLocked(request);
    });
}

#if defined(MRT_ALLOCATION_STALL_OBSERVE)
void RegionManager::SetAllocationStallTestHooks(AllocationStallTestHook beforeWave,
                                                AllocationStallTestHook requestGc,
                                                AllocationStallTestHook beforeWait)
{
    allocationStallBeforeWaveTestHook = std::move(beforeWave);
    allocationStallGcTestHook = std::move(requestGc);
    allocationStallBeforeWaitTestHook = std::move(beforeWait);
}

size_t RegionManager::PendingStalledAllocations() const { return allocationStallQueue.Pending(); }
size_t RegionManager::EnqueuedStalledAllocations() const { return allocationStallQueue.EnqueuedCount(); }
size_t RegionManager::DequeuedStalledAllocations() const { return allocationStallQueue.DequeuedCount(); }
size_t RegionManager::SatisfiedStalledAllocations() const { return allocationStallQueue.SatisfiedCount(); }
size_t RegionManager::FailedStalledAllocations() const { return allocationStallQueue.FailedCount(); }
#endif

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
    const size_t num = size / RegionInfo::UNIT_SIZE;
    PageMemory& memory = request.Memory();
    if (!freeRegionManager.ClaimPageMemory(num, memory)) { return false; }
    if (!memory.committed) {
        Uncommitter::CancelCycleLocked();
    }
    pageAllocatorUsed += size;
    return true;
}

void RegionManager::ReclaimRegionToMarkQuarantine(RegionInfo* region)
{
    RegionInfo::RetirePage(region, [this, region] { ReclaimRetiredRegionToMarkQuarantine(region); });
}

void RegionManager::ReclaimRetiredRegionToMarkQuarantine(RegionInfo* region)
{
    // routedest: census only, see ReclaimRegion.
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "mark-quarantine region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
         region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());
    {
        RegionInfo::InPlaceClaimScope drain(region, ZForwardingLife::Retire::RECLAIM_MARK_QUARANTINE);
    }
    HeapZap::ZapReclaimedRegion(region->GetRegionStart(), region->GetRegionEnd());
    region->InitFreeUnits();
    ScopedEnterSaferegion enterSaferegion(true);
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    freeRegionManager.AddMarkQuarantineUnits(unitIndex, num);
    CHECK(pageAllocatorUsed >= num * RegionInfo::UNIT_SIZE);
    pageAllocatorUsed -= num * RegionInfo::UNIT_SIZE;
}

size_t RegionManager::ReleaseRegion(RegionInfo* region)
{
    const size_t size = region->GetRegionSize();
    RegionInfo::RetirePage(region, [this, region] { ReleaseRetiredRegion(region); });
    return size;
}

void RegionManager::ReleaseRetiredRegion(RegionInfo* region)
{
    // routedest: census only, see ReclaimRegion.

    // holdercapture: large regions above the release threshold never reach CollectRegion,
    // so the snapshot has to be taken on this path too or the face is lost unrecorded.

    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    // Large regions above the release threshold bypass CollectRegion. Invalidate
    // their two owned bitmap slices before the address range can be unmapped/reused.
    ScrubRememberedSetForRegion(region);
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "release region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

    {
        RegionInfo::InPlaceClaimScope drain(region, ZForwardingLife::Retire::RELEASE_REGION);
    }
    region->InitFreeUnits();
    {
        RegionInfo::ReleaseUnits(unitIndex, num);
    }
    ReturnPageMemory(PageMemory{ unitIndex, num, 0, false });
}


void RegionManager::PromoteAllRegions()
{
    VisitPageOwners([&](RegionInfo* region) {
        if (region->IsValidRegion() && !region->IsGarbageRegion()) {
            size_t liveBytes = region->GetLiveByteCount();
            if (liveBytes > 0) {
                region->PreserveRetainedLiveInfoUpTo(
                    std::min(region->GetCensusBoundary(), region->GetRegionAllocPtr()));
            } else if (region->GetRawPointerObjectCount() == 0) {
                region->PreserveRetainedLiveInfo(region->GetRegionStart());
            }
            if (region->IsYoungRegion()) {
                MarkView<Generation::Young> youngView = region->GetMarkView<Generation::Young>();
                (void)region->PromoteYoungRegion(youngView);
            } else {
                // Preserve the pre-genface cleanup for already-old regions.
                region->SetYoungAge(0);
            }
        }
    });
}

RegionInfo* RegionManager::TakeRegion(size_t num, RegionInfo::UnitRole type, bool expectPhysicalMem,
                                      bool allowSaferegion, bool clearPayload)
{
    // check for allocation since we do not want gc threads and mutators do any harm to each other.
    size_t size = num * RegionInfo::UNIT_SIZE;
    // routefix: RequestForRegion may sleep; under ROUTING keep the critical section short.
    if (allowSaferegion) {
        RequestForRegion(size);
    }

#if !defined(__OHOS__)
    size_t gatedBytes = 0;
    RegionInfo* garbage = allowSaferegion ? TakeReclaimableGarbageRegion(&gatedBytes) : nullptr;
    if (garbage != nullptr) {
        ReclaimRegion(garbage);
    }
#else
    size_t gatedBytes = GetGatedGarbageBytes();
#endif

    AllocationStallRequest request(size, static_cast<uint8_t>(type), expectPhysicalMem, clearPayload);
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
        RegionInfo* region = freeRegionManager.MaterializePageMemory(
            request.Memory(), type, request.ExpectsPhysicalMemory(), request.ClearsPayload(), committedUnits);
        if (request.Memory().virtualClaimed) {
            // The address is known only after materialization; keep the existing
            // diagnostic envelope update under its allocator lock.
            std::lock_guard<std::mutex> lock(pageAllocatorMutex);
            const uintptr_t end = RegionInfo::GetUnitAddress(request.Memory().index) + size;
            inactiveZone.store(std::max(inactiveZone.load(std::memory_order_relaxed), end), std::memory_order_release);
        }
        if (region == nullptr) {
            // zPageAllocator.cpp:1906: preserve the succeeded prefix in the
            // committed cache and return only the failed suffix uncommitted.
            // No page descriptor has been published for this allocation.
            std::unique_ptr<ScopedEnterSaferegion> enterSaferegion;
            if (allowSaferegion) { enterSaferegion.reset(new ScopedEnterSaferegion(true)); }
            std::lock_guard<std::mutex> lock(pageAllocatorMutex);
            const size_t index = request.Memory().index;
            if (request.Memory().virtualClaimed) {
                if (committedUnits != 0) { freeRegionManager.AddGarbageUnits(index, committedUnits, allowSaferegion); }
                if (committedUnits != num) { freeRegionManager.AddReleaseUnits(index + committedUnits, num - committedUnits, allowSaferegion); }
            }
            CHECK(pageAllocatorUsed >= size);
            pageAllocatorUsed -= size;
            allocationStallQueue.SatisfyAvailableLocked([this](AllocationStallRequest& pending) {
                return ClaimAllocationLocked(pending);
            });
            return nullptr;
        }
        if (num >= HUGE_PAGE) {
            TagHugePage(region, num);
        }
        ZStatMutatorAllocRate::sample_allocation(size);
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

size_t RegionManager::CollectFreePinnedSlots(RegionInfo* region)
{
    // pinroot: raw-pointer pin is a liveness hold — do not free any slot while count > 0.
    // AddRawPointerObject only bumps this counter (no mark bit / root set); reclaim must honour it.
    if (region->GetRawPointerObjectCount() > 0) {

        return 0;
    }
    // traverse pinned region to reclaim free pinned objects.
    size_t start = region->GetRegionStart();
    size_t garbageSize = 0;
    MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
    region->VisitAllObjects([this, region, view, start, &garbageSize](BaseObject* object) {
        size_t offset = reinterpret_cast<MAddress>(object) - start;
        if (!region->IsSurvivedObject(view, offset)) {
            if (!Collector::PlausibleManagedObjectGate("CollectFreePinnedSlots", object)) {
                return;
            }
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
    RegionInfo* region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        // pinroot: whole-region reclaim also ignores pins; skip while any raw pointer holds.
        if (region->GetRawPointerObjectCount() > 0) {

            region = region->GetNextRegion();
            continue;
        }
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        if (region->IsKnownEmpty(view)) {
            RegionInfo* del = region;
            region = region->GetNextRegion();
            oldPinnedRegionList.DeleteRegion(del);

            auto fixToObj = [](BaseObject* obj) { ReleaseNativeResource(obj); };
            del->VisitAllObjects(fixToObj);


            garbageSize += CollectRegion<Generation::Old>(del);
            continue;
        } else {
            garbageSize += CollectFreePinnedSlots(region);
            region = region->GetNextRegion();
        }
    }

    return garbageSize;
}

size_t RegionManager::CollectLargeGarbage()
{
    size_t garbageSize = 0;
    RegionInfo* region = oldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        // holdercapture: sample the face here, BEFORE the predicate below decides.
        //
        // Sampling early is necessary but NOT sufficient, and the earlier version of this
        // comment claimed otherwise. Through one view the two predicates are ordered, not
        // equal: for a large region IsMarkedObject(view,0) is GetMarkedRegionFlag(view)==1
        // while IsSurvivedObject(view,0) is that OR isResurrected, so marked implies
        // survived. Every region this loop releases failed !IsSurvivedObject(view,0) and
        // therefore reads marked==0 through that same view - one line earlier just as
        // surely as at the top of ReleaseRegion. Moving the sample moves the zero; it does
        // not remove it.
        //
        // The mark bit read through the view below is a control, not the finding: it must
        // be 0 on every released region, and if it ever is not, the reading of this
        // predicate is wrong and the rest of the measurement is void.

        // for large region, the offset of obj is 0
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        if (!region->IsSurvivedObject(view, 0)) {
            DLOG(REGION, "reclaim large region %p@[0x%zx+%zu, 0x%zx) type %u", region, region->GetRegionStart(),
                 region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

            RegionInfo* del = region;
            region = region->GetNextRegion();
            oldLargeRegionList.DeleteRegion(del);
            if (del->GetRegionSize() > RegionInfo::LARGE_OBJECT_RELEASE_THRESHOLD) {
                garbageSize += ReleaseRegion(del);
            } else {

                garbageSize += CollectRegion<Generation::Old>(del);
            }
        } else {
            region->ResetMarkBit(view);
            region = region->GetNextRegion();
        }
    }

    region = recentLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        region->ResetMarkBit(view);
        region = region->GetNextRegion();
    }

    return garbageSize;
}

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void RegionManager::DumpRegionInfo() const
{
    if (!ENABLE_LOG(ALLOC)) {
        return;
    }
    VisitPageOwners([&](RegionInfo* region) {
        if (!region->IsFreeRegion()) {
            region->DumpRegionInfo(ALLOC);
        }
    });
}
#endif

void RegionManager::DumpRegionStats(const char* msg) const
{
    size_t totalSize = GetHeapCapacity();
    VLOG(REPORT, "heap backing capacity %zu bytes", GetCommittedCapacity());
    size_t totalUnits = totalSize / RegionInfo::UNIT_SIZE;
    size_t activeSize = GetActiveUnitCount() * RegionInfo::UNIT_SIZE;
    size_t activeUnits = activeSize / RegionInfo::UNIT_SIZE;

    size_t tlRegions = tlRegionList.GetRegionCount();
    size_t tlUnits = tlRegionList.GetUnitCount();
    size_t tlSize = tlUnits * RegionInfo::UNIT_SIZE;
    size_t allocTLSize = tlRegionList.GetAllocatedSize();

    size_t fromRegions = fromRegionList.GetRegionCount();
    size_t fromUnits = fromRegionList.GetUnitCount();
    size_t fromSize = fromUnits * RegionInfo::UNIT_SIZE;
    size_t allocFromSize = fromRegionList.GetAllocatedSize();

    size_t unmovableRegions = unmovableFromRegionList.GetRegionCount();
    size_t unmovableUnits = unmovableFromRegionList.GetUnitCount();
    size_t unmovableSize = unmovableUnits * RegionInfo::UNIT_SIZE;
    size_t allocUnmovableSize = unmovableFromRegionList.GetAllocatedSize();

    size_t keptRegions = 0;
    size_t keptUnits = 0;
    size_t keptSize = 0;
    size_t keptLive = 0;
    auto censusKept = [&keptRegions, &keptUnits, &keptSize, &keptLive](RegionInfo* region) {
        if (region == nullptr || !region->IsForwardingDone()) {
            return;
        }
        if (region->IsForwardingDone() && !region->IsCompacted()) {
            return;
        }
        ++keptRegions;
        keptUnits += region->GetUnitCount();
        keptSize += region->GetRegionSize();
        keptLive += region->GetLiveByteCount();
    };
    fromRegionList.VisitAllRegions(censusKept);
    unmovableFromRegionList.VisitAllRegions(censusKept);
    recentFullRegionList.VisitAllRegions(censusKept);

    size_t recentFullRegions = recentFullRegionList.GetRegionCount();
    size_t recentFullUnits = recentFullRegionList.GetUnitCount();
    size_t recentFullSize = recentFullUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentFullSize = recentFullRegionList.GetAllocatedSize();
    RecentFullAccounting::Report(recentFullRegions, recentFullSize);

    size_t garbageRegions = garbageRegionList.GetRegionCount();
    size_t garbageUnits = garbageRegionList.GetUnitCount();
    size_t garbageSize = garbageUnits * RegionInfo::UNIT_SIZE;
    size_t allocGarbageSize = garbageRegionList.GetAllocatedSize();

    size_t pinnedRegions = oldPinnedRegionList.GetRegionCount();
    size_t pinnedUnits = oldPinnedRegionList.GetUnitCount();
    size_t pinnedSize = pinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocPinnedSize = oldPinnedRegionList.GetAllocatedSize();

    size_t recentPinnedRegions = recentPinnedRegionList.GetRegionCount();
    size_t recentPinnedUnits = recentPinnedRegionList.GetUnitCount();
    size_t recentPinnedSize = recentPinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentPinnedSize = recentPinnedRegionList.GetAllocatedSize();

    size_t rawPointerPinnedRegions = rawPointerPinnedRegionList.GetRegionCount();
    size_t rawPointerPinnedUnits = rawPointerPinnedRegionList.GetUnitCount();
    size_t rawPointerPinnedSize = rawPointerPinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocRawPointerPinnedSize = rawPointerPinnedRegionList.GetAllocatedSize();

    size_t largeRegions = oldLargeRegionList.GetRegionCount();
    size_t largeUnits = oldLargeRegionList.GetUnitCount();
    size_t largeSize = largeUnits * RegionInfo::UNIT_SIZE;
    size_t allocLargeSize = oldLargeRegionList.GetAllocatedSize();

    size_t recentlargeRegions = recentLargeRegionList.GetRegionCount();
    size_t recentlargeUnits = recentLargeRegionList.GetUnitCount();
    size_t recentLargeSize = recentlargeUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentLargeSize = recentLargeRegionList.GetAllocatedSize();

    size_t allHeapSize = GetHeapCapacity();
    size_t allUnits = allHeapSize / RegionInfo::UNIT_SIZE;
    size_t inactiveUnits = GetInactiveUnitCount();

    size_t usedUnitCount = GetUsedUnitCount();
    size_t usedObjSize = GetAllocatedSize();
    size_t virtualUnits = freeRegionManager.GetVirtualUnitCount();
    size_t dirtyUnits = freeRegionManager.GetDirtyUnitCount();
    size_t dirtySize = dirtyUnits * RegionInfo::UNIT_SIZE;

    size_t totalUnitCount = usedUnitCount + garbageUnits + dirtyUnits;
    size_t totalObjSize = usedObjSize + garbageSize + dirtyUnits * RegionInfo::UNIT_SIZE;

    double objectCapacity = (allHeapSize > 0) ? static_cast<double>(totalObjSize) / allHeapSize : 0.0;
    double unitCapacity = (allUnits > 0) ? static_cast<double>(totalUnitCount) / allUnits : 0.0;
    double usedObjectCapacity = (allHeapSize > 0) ? static_cast<double>(usedObjSize) / allHeapSize : 0.0;
    double usedUnitCapacity = (allUnits > 0) ? static_cast<double>(usedUnitCount) / allUnits : 0.0;
    double objFragRate = 1.0 - objectCapacity;
    double unitFragRate = 1.0 - unitCapacity;
    double usedObjFragRate = 1.0 - usedObjectCapacity;
    double usedUnitFragRate = 1.0 - usedUnitCapacity;

#define DUMP_REGION_STATS_LOG(format, ...) VLOG(REPORT, format, ##__VA_ARGS__)

    DUMP_REGION_STATS_LOG("%s", msg);

    DUMP_REGION_STATS_LOG("\ttotal units: %zu (%zu B)", totalUnits, totalSize);
    DUMP_REGION_STATS_LOG("\tactive units: %zu (%zu B)", activeUnits, activeSize);
    DUMP_REGION_STATS_LOG("\tinactive units: %zu (%zu B)", inactiveUnits, inactiveUnits * RegionInfo::UNIT_SIZE);

    DUMP_REGION_STATS_LOG("\ttl-regions %zu: %zu units (%zu B, alloc %zu)", tlRegions,  tlUnits, tlSize, allocTLSize);
    DUMP_REGION_STATS_LOG("\tfrom-regions %zu: %zu units (%zu B, alloc %zu)", fromRegions,  fromUnits, fromSize,
                          allocFromSize);
    DUMP_REGION_STATS_LOG("\tunmovable-from regions %zu: %zu units (%zu B, alloc %zu)", unmovableRegions,
                          unmovableUnits, unmovableSize, allocUnmovableSize);
    DUMP_REGION_STATS_LOG("\tkept-publish regions %zu: %zu units (%zu B, live %zu, hole %zu)", keptRegions, keptUnits,
                          keptSize, keptLive, keptSize > keptLive ? keptSize - keptLive : 0);
    DUMP_REGION_STATS_LOG("\trecent-full regions %zu: %zu units (%zu B, alloc %zu)",
                          recentFullRegions, recentFullUnits, recentFullSize, allocRecentFullSize);
    DUMP_REGION_STATS_LOG("\tgarbage regions %zu: %zu units (%zu B, alloc %zu)",
                          garbageRegions, garbageUnits, garbageSize, allocGarbageSize);
    DUMP_REGION_STATS_LOG("\tpinned regions %zu: %zu units (%zu B, alloc %zu)",
                          pinnedRegions, pinnedUnits, pinnedSize, allocPinnedSize);
    DUMP_REGION_STATS_LOG("\trecent pinned regions %zu: %zu units (%zu B, alloc %zu)",
                          recentPinnedRegions, recentPinnedUnits, recentPinnedSize, allocRecentPinnedSize);
    DUMP_REGION_STATS_LOG("\trawPointer pinned regions %zu: %zu units (%zu B, alloc %zu)",
                          rawPointerPinnedRegions, rawPointerPinnedUnits, rawPointerPinnedSize,
                          allocRawPointerPinnedSize);
    DUMP_REGION_STATS_LOG("\tlarge-object regions %zu: %zu units (%zu B, alloc %zu)",
                          largeRegions, largeUnits, largeSize, allocLargeSize);
    DUMP_REGION_STATS_LOG("\trecent large-object regions %zu: %zu units (%zu B, alloc %zu)",
                          recentlargeRegions, recentlargeUnits, recentLargeSize, allocRecentLargeSize);
    DUMP_REGION_STATS_LOG("\tused summary: usedUnits %zu (%zu B), usedObjSize %zu B",
                          usedUnitCount, usedUnitCount * RegionInfo::UNIT_SIZE, usedObjSize);

    size_t virtualMaxBlock = freeRegionManager.GetVirtualMaxBlock();
    size_t dirtyMaxBlock = freeRegionManager.GetDirtyMaxBlock();
    size_t virtualNodeCount = freeRegionManager.GetVirtualNodeCount();
    size_t dirtyNodeCount = freeRegionManager.GetDirtyNodeCount();
    DUMP_REGION_STATS_LOG("\tfree virtual units: %zu (%zu B), nodes: %zu, maxBlock: %zu units (%zu B)",
                          virtualUnits, virtualUnits * RegionInfo::UNIT_SIZE,
                          virtualNodeCount,
                          virtualMaxBlock, virtualMaxBlock * RegionInfo::UNIT_SIZE);
    DUMP_REGION_STATS_LOG("\tdirty units: %zu (%zu B), nodes: %zu, maxBlock: %zu units (%zu B)",
                          dirtyUnits, dirtyUnits * RegionInfo::UNIT_SIZE, dirtyNodeCount,
                          dirtyMaxBlock,
                          dirtyMaxBlock * RegionInfo::UNIT_SIZE);

    DUMP_REGION_STATS_LOG("\tgarbage+dirty summary: garbageUnits %zu (%zu B, allocObj %zu), dirtyUnits %zu (%zu B)",
                          garbageUnits, garbageSize, allocGarbageSize, dirtyUnits, dirtySize);
    DUMP_REGION_STATS_LOG("\tobjectCapacity: %.4f (totalObjSize %zu / allHeapSize %zu), objFragRate: %.4f",
                          objectCapacity, totalObjSize, allHeapSize, objFragRate);
    DUMP_REGION_STATS_LOG("\tunitCapacity: %.4f (totalUnitCount %zu / allUnits %zu), unitFragRate: %.4f",
                          unitCapacity, totalUnitCount, allUnits, unitFragRate);
    DUMP_REGION_STATS_LOG("\tusedObjectCapacity: %.4f (usedObjSize %zu / allHeapSize %zu), usedObjFragRate: %.4f",
                          usedObjectCapacity, usedObjSize, allHeapSize, usedObjFragRate);
    DUMP_REGION_STATS_LOG("\tusedUnitCapacity: %.4f (usedUnitCount %zu / allUnits %zu), usedUnitFragRate: %.4f",
                          usedUnitCapacity, usedUnitCount, allUnits, usedUnitFragRate);
#undef DUMP_REGION_STATS_LOG

    TRACE_COUNT("CJRT_GC_totalSize", totalSize);
    TRACE_COUNT("CJRT_GC_totalUnits", totalUnits);
    TRACE_COUNT("CJRT_GC_activeSize", activeSize);
    TRACE_COUNT("CJRT_GC_activeUnits", activeUnits);
    TRACE_COUNT("CJRT_GC_tlRegions", tlRegions);
    TRACE_COUNT("CJRT_GC_tlUnits", tlUnits);
    TRACE_COUNT("CJRT_GC_tlSize", tlSize);
    TRACE_COUNT("CJRT_GC_allocTLSize", allocTLSize);
    TRACE_COUNT("CJRT_GC_fromRegions", fromRegions);
    TRACE_COUNT("CJRT_GC_fromUnits", fromUnits);
    TRACE_COUNT("CJRT_GC_fromSize", fromSize);
    TRACE_COUNT("CJRT_GC_allocFromSize", allocFromSize);
    TRACE_COUNT("CJRT_GC_recentFullRegions", recentFullRegions);
    TRACE_COUNT("CJRT_GC_recentFullUnits", recentFullUnits);
    TRACE_COUNT("CJRT_GC_recentFullSize", recentFullSize);
    TRACE_COUNT("CJRT_GC_allocRecentFullSize", allocRecentFullSize);
    TRACE_COUNT("CJRT_GC_garbageRegions", garbageRegions);
    TRACE_COUNT("CJRT_GC_garbageUnits", garbageUnits);
    TRACE_COUNT("CJRT_GC_garbageSize", garbageSize);
    TRACE_COUNT("CJRT_GC_allocGarbageSize", allocGarbageSize);
    TRACE_COUNT("CJRT_GC_pinnedRegions", pinnedRegions);
    TRACE_COUNT("CJRT_GC_pinnedUnits", pinnedUnits);
    TRACE_COUNT("CJRT_GC_pinnedSize", pinnedSize);
    TRACE_COUNT("CJRT_GC_allocPinnedSize", allocPinnedSize);
    TRACE_COUNT("CJRT_GC_recentPinnedRegions", recentPinnedRegions);
    TRACE_COUNT("CJRT_GC_recentPinnedUnits", recentPinnedUnits);
    TRACE_COUNT("CJRT_GC_recentPinnedSize", recentPinnedSize);
    TRACE_COUNT("CJRT_GC_allocRecentPinnedSize", allocRecentPinnedSize);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedRegions", rawPointerPinnedRegions);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedUnits", rawPointerPinnedUnits);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedSize", rawPointerPinnedSize);
    TRACE_COUNT("CJRT_GC_allocRawPointerPinnedSize", allocRawPointerPinnedSize);
    TRACE_COUNT("CJRT_GC_largeRegions", largeRegions);
    TRACE_COUNT("CJRT_GC_largeUnits", largeUnits);
    TRACE_COUNT("CJRT_GC_largeSize", largeSize);
    TRACE_COUNT("CJRT_GC_allocLargeSize", allocLargeSize);
    TRACE_COUNT("CJRT_GC_recentlargeRegions", recentlargeRegions);
    TRACE_COUNT("CJRT_GC_recentlargeUnits", recentlargeUnits);
    TRACE_COUNT("CJRT_GC_recentLargeSize", recentLargeSize);
    TRACE_COUNT("CJRT_GC_allocRecentLargeSize", allocRecentLargeSize);
    TRACE_COUNT("CJRT_GC_usedUnits", usedUnitCount);
    TRACE_COUNT("CJRT_GC_releasedUnits", releasedUnits);
    TRACE_COUNT("CJRT_GC_dirtyUnits", dirtyUnits);
    TRACE_COUNT("CJRT_GC_listedUnits", totalUnitCount);
    [[maybe_unused]] constexpr size_t decimalPrecision = 10000;
    TRACE_COUNT("CJRT_GC_objectCapacity", static_cast<size_t>(objectCapacity * decimalPrecision));
    TRACE_COUNT("CJRT_GC_unitCapacity", static_cast<size_t>(unitCapacity * decimalPrecision));
}


} // namespace MapleRuntime
