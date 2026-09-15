// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"
#include "Heap/z/zStat.hpp"
#include "Mutator/ThreadLocal.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zVirtualMemoryManager.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "zunittest.hpp"

#include <algorithm>
#include <memory>
#include <vector>

namespace MapleRuntime {
namespace GcUnit {

// ZGC has no test_zMappedCache.cpp in the frozen reference. These cases port
// the range geometries from ZVirtualMemoryManagerTest::test_remove_whole,
// test_remove_from_low and test_remove_from_high to the mapped-cache owner.
// ZMappedCache stores its entries inside the cached memory
// (zMappedCache.cpp:92-119), so the cases run over a committed, mapped
// fixture range and speak in vmems of `unit` granules.
namespace {
struct CacheFixture {
    static constexpr size_t kUnits = 16;
    ZTestHeapMapping mapping{ kUnits * ZBackingGranuleSize };
    ZMappedCache cache;

    ZVirtualMemory vmem(size_t index, size_t count) const
    {
        return ZVirtualMemory(mapping.offset() + index * ZBackingGranuleSize, count * ZBackingGranuleSize);
    }
    size_t index(const ZVirtualMemory& v) const { return (v.start() - mapping.offset()) / ZBackingGranuleSize; }
    size_t count(const ZVirtualMemory& v) const { return v.size() / ZBackingGranuleSize; }
    size_t bytes(size_t count) const { return count * ZBackingGranuleSize; }
};
}

GC_TEST(MappedCache, CoalesceAndRemoveWhole)
{
    EnsureZAddressDomain();
    CacheFixture f;
    f.cache.insert(f.vmem(0, 2));
    f.cache.insert(f.vmem(4, 2));
    f.cache.insert(f.vmem(2, 2));
    const ZVirtualMemory whole = f.cache.remove_contiguous(f.bytes(6));
    GC_EXPECT_FALSE(whole.is_null());
    GC_EXPECT_EQ(f.index(whole), 0U);
    GC_EXPECT_EQ(f.count(whole), 6U);
    GC_EXPECT_TRUE(f.cache.remove_contiguous(f.bytes(1)).is_null());
}

GC_TEST(MappedCache, SmallPagesUseLowestAddress)
{
    EnsureZAddressDomain();
    // remove_contiguous(ZPageSizeSmall) scans from the lowest address
    // (zMappedCache.cpp:642-644); every other size goes through the size
    // classes first. The fixture speaks in small-page multiples.
    const size_t small = ZPageSizeSmall;
    ZTestHeapMapping mapping(18 * small);
    ZMappedCache cache;
    auto vmem = [&](size_t index, size_t count) {
        return ZVirtualMemory(mapping.offset() + index * small, count * small);
    };
    auto index = [&](const ZVirtualMemory& v) { return (v.start() - mapping.offset()) / small; };
    cache.insert(vmem(10, 8));
    cache.insert(vmem(0, 2));
    const ZVirtualMemory first = cache.remove_contiguous(small);
    const ZVirtualMemory second = cache.remove_contiguous(small);
    GC_EXPECT_FALSE(first.is_null());
    GC_EXPECT_FALSE(second.is_null());
    GC_EXPECT_EQ(index(first), 0U);
    GC_EXPECT_EQ(index(second), 1U);
    // A larger request prefers the approximate best-fit size class.
    const ZVirtualMemory rest = cache.remove_contiguous(8 * small);
    GC_EXPECT_FALSE(rest.is_null());
    GC_EXPECT_EQ(index(rest), 10U);
    GC_EXPECT_TRUE(cache.remove_contiguous(small).is_null());
}

GC_TEST(MappedCache, EqualCapacityContiguousAndFragmented)
{
    EnsureZAddressDomain();
    CacheFixture contiguous;
    CacheFixture fragmented;
    contiguous.cache.insert(contiguous.vmem(0, 6));
    fragmented.cache.insert(fragmented.vmem(0, 2));
    fragmented.cache.insert(fragmented.vmem(4, 2));
    fragmented.cache.insert(fragmented.vmem(8, 2));
    contiguous.cache.reset_min_size_watermark();
    fragmented.cache.reset_min_size_watermark();
    GC_EXPECT_EQ(contiguous.cache.min_size_watermark(), fragmented.cache.min_size_watermark());
    const ZVirtualMemory whole = contiguous.cache.remove_contiguous(contiguous.bytes(6));
    GC_EXPECT_EQ(contiguous.count(whole), 6U);
    GC_EXPECT_TRUE(fragmented.cache.remove_contiguous(fragmented.bytes(6)).is_null());
    ZArray<ZVirtualMemory> harvested;
    GC_EXPECT_EQ(fragmented.cache.remove_discontiguous(fragmented.bytes(5), &harvested), fragmented.bytes(5));
    size_t owned = 0;
    for (const auto& range : harvested) { owned += fragmented.count(range); }
    GC_EXPECT_EQ(owned, 5U);
    GC_EXPECT_EQ(fragmented.cache.min_size_watermark(), fragmented.bytes(1));
    for (const auto& range : harvested) { fragmented.cache.insert(range); }
    ZArray<ZVirtualMemory> all;
    GC_EXPECT_EQ(fragmented.cache.remove_discontiguous(fragmented.bytes(6), &all), fragmented.bytes(6));
    GC_EXPECT_EQ(all.size(), 3U);
}

GC_TEST(MappedCache, UncommitUsesHighestAddress)
{
    EnsureZAddressDomain();
    CacheFixture f;
    f.cache.insert(f.vmem(0, 4));
    f.cache.insert(f.vmem(8, 4));
    ZArray<ZVirtualMemory> extents;
    GC_EXPECT_EQ(f.cache.remove_for_uncommit(f.bytes(2), &extents), f.bytes(2));
    GC_EXPECT_EQ(extents.size(), 1U);
    GC_EXPECT_EQ(f.index(extents[0]), 10U);
    GC_EXPECT_EQ(f.count(extents[0]), 2U);
    // What is left: [0,4) and [8,10).
    const ZVirtualMemory low = f.cache.remove_contiguous(f.bytes(4));
    GC_EXPECT_EQ(f.index(low), 0U);
    const ZVirtualMemory high = f.cache.remove_contiguous(f.bytes(2));
    GC_EXPECT_EQ(f.index(high), 8U);
    GC_EXPECT_TRUE(f.cache.remove_contiguous(f.bytes(1)).is_null());
}

// The intrusive red-black tree behind the cache (Base/RBTree.h) against a
// sorted model: random inserts, removals and coalescing keep the in-order
// walk equal to the set of cached ranges.
GC_TEST(MappedCache, TreeMatchesModelUnderRandomChurn)
{
    EnsureZAddressDomain();
    constexpr size_t kUnits = 256;
    ZTestHeapMapping mapping(kUnits * ZBackingGranuleSize);
    ZMappedCache cache;
    std::vector<bool> cached(kUnits, false);
    uint64_t seed = 0x9e3779b97f4a7c15ULL;
    auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    auto vmemOf = [&](size_t index, size_t count) {
        return ZVirtualMemory(mapping.offset() + index * ZBackingGranuleSize, count * ZBackingGranuleSize);
    };
    size_t cachedUnits = 0;
    for (int round = 0; round < 4000; ++round) {
        const size_t index = next() % kUnits;
        const size_t count = 1 + next() % 4;
        if (index + count > kUnits) { continue; }
        bool free = true;
        for (size_t i = index; i < index + count; ++i) { free = free && !cached[i]; }
        if (free && (next() % 3) != 0) {
            cache.insert(vmemOf(index, count));
            for (size_t i = index; i < index + count; ++i) { cached[i] = true; }
            cachedUnits += count;
        } else if (cachedUnits != 0) {
            ZArray<ZVirtualMemory> out;
            const size_t want = std::min(cachedUnits, count) * ZBackingGranuleSize;
            const size_t got = (next() % 2 == 0)
                ? cache.remove_discontiguous(want, &out)
                : cache.remove_for_uncommit(want, &out);
            GC_EXPECT_EQ(got, want);
            for (const ZVirtualMemory& v : out) {
                const size_t start = (v.start() - mapping.offset()) / ZBackingGranuleSize;
                for (size_t i = start; i < start + v.size() / ZBackingGranuleSize; ++i) {
                    GC_EXPECT_TRUE(cached[i]);
                    cached[i] = false;
                }
                cachedUnits -= v.size() / ZBackingGranuleSize;
            }
        }
    }
    // Drain: everything cached comes back exactly once, lowest address first.
    ZArray<ZVirtualMemory> all;
    GC_EXPECT_EQ(cache.remove_discontiguous(cachedUnits * ZBackingGranuleSize, &all), cachedUnits * ZBackingGranuleSize);
    for (const ZVirtualMemory& v : all) {
        const size_t start = (v.start() - mapping.offset()) / ZBackingGranuleSize;
        for (size_t i = start; i < start + v.size() / ZBackingGranuleSize; ++i) {
            GC_EXPECT_TRUE(cached[i]);
            cached[i] = false;
        }
    }
    for (bool c : cached) { GC_EXPECT_FALSE(c); }
    GC_EXPECT_TRUE(cache.remove_contiguous(ZBackingGranuleSize).is_null());
}

#if defined(__linux__)
// zPhysicalMemoryManager.cpp:331-393: a stash carries backing indices only;
// the same segments map at a different virtual address with their contents
// intact (PLAN P04 invariant 2), and stash_segments sorts the indices so the
// restored run maps in backing order.
GC_TEST(ZPhysicalMemoryManager, BackingIndicesSurviveVirtualShuffle)
{
    EnsureZAddressDomain();
    // Keep the granule map small: it covers [0, ZAddressOffsetMax).
    ZAddressOffsetMaxSetter offsetMax(64 * MB);
    ZTest::ZBackingLimitSetter backingLimits;
    const size_t unit = ZBackingGranuleSize;
    ZTest::ZAddressReserver reserver;
    reserver.SetUp(8 * unit);
    GC_EXPECT_TRUE(reserver.reserver()->reserved() == 8 * unit && reserver.registry()->is_contiguous());
    const zoffset base = reserver.registry()->peek_low_address();
    {
        // Exactly two backing indices per partition: the final alloc below
        // only succeeds if free() handed the stashed indices back.
        const size_t partitions = NumaTopology::SealProcessTopology().Count();
        ZPhysicalMemoryManager physical(2 * partitions * unit);
        GC_EXPECT_TRUE(physical.is_initialized());
        const ZVirtualMemory a(base, unit);
        const ZVirtualMemory b(base + 4 * unit, unit);
        physical.alloc(a, 0);
        physical.alloc(b, 0);
        GC_EXPECT_EQ(physical.commit(a, 0), unit);
        GC_EXPECT_EQ(physical.commit(b, 0), unit);
        physical.map(a, 0);
        physical.map(b, 0);
        *reinterpret_cast<uint64_t*>(untype(ZOffset::address_unsafe(a.start()))) = 0x12345678;
        *reinterpret_cast<uint64_t*>(untype(ZOffset::address_unsafe(b.start()))) = 0xabcdef;
        physical.unmap(a);
        physical.unmap(b);
        // Stash in the order b, a: the stash must still come out sorted by index.
        ZArray<ZVirtualMemory> vmems{ b, a };
        ZArray<zbacking_index> stash;
        physical.stash_segments(vmems, &stash);
        GC_EXPECT_EQ(stash.size(), 2U);
        GC_EXPECT_TRUE(stash[0] < stash[1]);
        const ZVirtualMemory c(base + 2 * unit, 2 * unit);
        physical.restore_segments(c, stash);
        physical.map(c, 0);
        const uintptr_t cAddress = untype(ZOffset::address_unsafe(c.start()));
        GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(cAddress), 0x12345678U);
        GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(cAddress + unit), 0xabcdefU);
        physical.unmap(c);
        GC_EXPECT_EQ(physical.uncommit(c), 2 * unit);
        physical.free(c, 0);
        // The freed indices are the lowest again; a fresh commit reads zeros.
        physical.alloc(a, 0);
        GC_EXPECT_EQ(physical.commit(a, 0), unit);
        physical.map(a, 0);
        GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(untype(ZOffset::address_unsafe(a.start()))), 0U);
        physical.unmap(a);
        GC_EXPECT_EQ(physical.uncommit(a), unit);
        physical.free(a, 0);
    }
    reserver.TearDown();
}

namespace {
struct ProductHeapFixture {
    // The RegionManager (and its mapped caches, whose entries live in heap
    // memory) must be destroyed before the mapping: declare it last.
    std::unique_ptr<ZTestRegionHeap> heap;
    RegionManager manager;
    explicit ProductHeapFixture(size_t units)
    {
        // This synthetic allocator runs on a runtime worker, not a CJ scheduler thread.
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        // Match CollectorResources::Init before allocation-rate sampling.
        ZStat::Initialize();
        HeapParam parameters{};
        parameters.regionSize = RegionInfo::UNIT_SIZE / 1024;
        parameters.exemptionThreshold = 0.8;
        heap.reset(new ZTestRegionHeap(units, manager, parameters, 0.5));
        Heap::GetHeap().GetRememberedSet().Initialize(manager.GetRegionHeapStart(),
                                                    units * RegionInfo::UNIT_SIZE * ZVirtualToPhysicalRatio);
    }
};

void Stamp(RegionInfo* region, uint64_t value)
{
    *reinterpret_cast<uint64_t*>(region->GetRegionStart()) = value;
}

uint64_t Read(uintptr_t address)
{
    return *reinterpret_cast<uint64_t*>(address);
}
}

// ZPartition::prepare_harvested_and_claim_virtual (zPageAllocator.cpp:1001-1046):
// with no capacity left to grow, a request larger than any cached run harvests
// the cache, unmaps, shuffles the virtual memory to the lowest free address
// and maps the stashed backing there in backing-index order.
GC_OTHER_VM_TEST(MappedCache, ProductHarvestRemapsToLowestFreeVirtual)
{
    const size_t unit = RegionInfo::UNIT_SIZE;
    ProductHeapFixture fixture(8);
    RegionManager& manager = fixture.manager;
    const auto role = RegionInfo::UnitRole::SMALL_SIZED_UNITS;
    RegionInfo* first = manager.TakeRegion(2, role, false, false, false);
    RegionInfo* second = manager.TakeRegion(2, role, false, false, false);
    RegionInfo* third = manager.TakeRegion(2, role, false, false, false);
    RegionInfo* fourth = manager.TakeRegion(2, role, false, false, false);
    GC_EXPECT_TRUE(first != nullptr && second != nullptr && third != nullptr && fourth != nullptr);
    GC_EXPECT_EQ(manager.GetCommittedCapacity(), 8 * unit);
    const uintptr_t heapStart = manager.GetRegionHeapStart();
    GC_EXPECT_EQ(first->GetRegionStart(), heapStart);
    const uintptr_t fourthAddress = fourth->GetRegionStart();
    Stamp(first, 0x1111);
    Stamp(third, 0x3333);
    manager.ReclaimRegion(first);
    manager.ReclaimRegion(third);
    GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 4U);
    // No growth room: capacity == max capacity, so the request must harvest.
    RegionInfo* result = manager.TakeRegion(4, role, false, false, false);
    GC_EXPECT_TRUE(result != nullptr);
    GC_EXPECT_EQ(result->GetUnitCount(), 4U);
    // Lowest free virtual address after the 8 committed units.
    GC_EXPECT_EQ(result->GetRegionStart(), heapStart + 8 * unit);
    GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 0U);
    GC_EXPECT_EQ(manager.GetCommittedCapacity(), 8 * unit);
    // Backing of first (indices 0,1) precedes backing of third (indices 4,5).
    GC_EXPECT_EQ(Read(result->GetRegionStart()), 0x1111U);
    GC_EXPECT_EQ(Read(result->GetRegionStart() + 2 * unit), 0x3333U);
    GC_EXPECT_TRUE(RegionInfo::TryGetRegionInfoAt(fourthAddress) == fourth);
    GC_EXPECT_TRUE(RegionInfo::TryGetRegionInfoAt(heapStart) == nullptr);
}

// TestMappedCacheHarvest.java / zPageAllocator.cpp:723-743: with growth room
// of two units, a four-unit request increases capacity by two and harvests
// only the remaining two units; the harvested backing lands in the first part
// of the new vmem (commit_increased_capacity commits the last part).
GC_OTHER_VM_TEST(MappedCache, ProductPartialGrowthHarvestsOnlyRemainder)
{
    const size_t unit = RegionInfo::UNIT_SIZE;
    ProductHeapFixture fixture(12);
    RegionManager& manager = fixture.manager;
    const auto role = RegionInfo::UnitRole::SMALL_SIZED_UNITS;
    RegionInfo* regions[5];
    for (auto& region : regions) {
        region = manager.TakeRegion(2, role, false, false, false);
        GC_EXPECT_TRUE(region != nullptr);
    }
    GC_EXPECT_EQ(manager.GetCommittedCapacity(), 10 * unit);
    const uintptr_t heapStart = manager.GetRegionHeapStart();
    Stamp(regions[4], 0x4444);
    // Same-class cache entries are harvested most recently inserted first.
    manager.ReclaimRegion(regions[0]);
    manager.ReclaimRegion(regions[2]);
    manager.ReclaimRegion(regions[4]);
    GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 6U);
    RegionInfo* result = manager.TakeRegion(4, role, false, false, false);
    GC_EXPECT_TRUE(result != nullptr);
    GC_EXPECT_EQ(result->GetUnitCount(), 4U);
    // Two units of growth plus two harvested units; the cache keeps four.
    GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 4U);
    GC_EXPECT_EQ(manager.GetCommittedCapacity(), 12 * unit);
    // insert_and_remove_from_low_exact_or_many: the harvested [8,10) merges
    // with the free virtual space above it, so the run starts at unit 8 and
    // its first part carries the harvested backing.
    GC_EXPECT_EQ(result->GetRegionStart(), heapStart + 8 * unit);
    GC_EXPECT_EQ(Read(result->GetRegionStart()), 0x4444U);
    // Capacity is exhausted now: another request must be satisfied from the cache.
    RegionInfo* cached = manager.TakeRegion(2, role, false, false, false);
    GC_EXPECT_TRUE(cached != nullptr);
    GC_EXPECT_EQ(manager.GetCommittedCapacity(), 12 * unit);
    GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 2U);
}

#endif

} // namespace GcUnit
} // namespace MapleRuntime
