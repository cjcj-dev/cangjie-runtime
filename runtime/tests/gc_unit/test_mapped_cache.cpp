// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"
#include "Heap/Allocator/CartesianTree.h"
#include "Heap/Allocator/MemMap.h"
#include "Heap/Allocator/RegionManager.h"

namespace MapleRuntime {
namespace GcUnit {

// ZGC has no test_zMappedCache.cpp in the frozen reference. These cases port
// the range geometries from ZVirtualMemoryManagerTest::test_remove_whole,
// test_remove_from_low and test_remove_from_high to the mapped-cache owner.
GC_TEST(MappedCache, CoalesceAndRemoveWhole)
{
    MappedCache cache;
    cache.Insert({0, 2});
    cache.Insert({4, 2});
    cache.Insert({2, 2});
    GC_EXPECT_EQ(cache.EntryCount(), 1U);
    GC_EXPECT_EQ(cache.Size(), 6U);
    const auto whole = cache.RemoveContiguous(6);
    GC_EXPECT_EQ(whole.index, 0U);
    GC_EXPECT_EQ(whole.count, 6U);
    GC_EXPECT_EQ(cache.Size(), 0U);
}

GC_TEST(MappedCache, SmallPagesUseLowestAddress)
{
    MappedCache cache;
    cache.Insert({10, 8});
    cache.Insert({0, 2});
    const auto first = cache.RemoveContiguous(1);
    const auto second = cache.RemoveContiguous(1);
    GC_EXPECT_EQ(first.index, 0U);
    GC_EXPECT_EQ(second.index, 1U);
    GC_EXPECT_EQ(cache.Size(), 8U);
}

GC_TEST(MappedCache, EqualCapacityContiguousAndFragmented)
{
    MappedCache contiguous;
    MappedCache fragmented;
    contiguous.Insert({0, 6});
    fragmented.Insert({0, 2});
    fragmented.Insert({4, 2});
    fragmented.Insert({8, 2});
    contiguous.ResetMinSizeWatermark();
    fragmented.ResetMinSizeWatermark();
    GC_EXPECT_EQ(contiguous.Size(), fragmented.Size());
    const auto whole = contiguous.RemoveContiguous(6);
    GC_EXPECT_EQ(whole.count, 6U);
    GC_EXPECT_TRUE(fragmented.RemoveContiguous(6).IsNull());
    GC_EXPECT_EQ(fragmented.Size(), 6U);
    std::vector<MappedCache::Extent> harvested;
    GC_EXPECT_EQ(fragmented.RemoveDiscontiguous(5, harvested), 5U);
    size_t owned = 0;
    for (const auto& range : harvested) { owned += range.count; }
    GC_EXPECT_EQ(owned + fragmented.Size(), whole.count);
    GC_EXPECT_EQ(fragmented.MinSizeWatermark(), 1U);
    for (const auto& range : harvested) { fragmented.Insert(range); }
    GC_EXPECT_EQ(fragmented.Size(), 6U);
    GC_EXPECT_EQ(fragmented.EntryCount(), 3U);
}

GC_TEST(MappedCache, UncommitUsesHighestAddress)
{
    MappedCache cache;
    cache.Insert({0, 4});
    cache.Insert({8, 4});
    std::vector<MappedCache::Extent> extents;
    GC_EXPECT_EQ(cache.RemoveForUncommit(2, extents), 2U);
    GC_EXPECT_EQ(extents.size(), 1U);
    GC_EXPECT_EQ(extents[0].index, 10U);
    GC_EXPECT_EQ(extents[0].count, 2U);
    GC_EXPECT_EQ(cache.Size(), 6U);
}

#if defined(__linux__)
// Additional owner-conservation coverage for zPageAllocator.cpp:999-1036 /
// zPhysicalMemoryManager.cpp:369-398, using the product native backing owner.
GC_TEST(MappedCache, NativeBackingSurvivesVirtualShuffle)
{
    const size_t unit = ALLOC_UTIL_PAGE_SIZE;
    MemMap* map = MemMap::MapMemory(8 * unit, 0, MemMap::DEFAULT_OPTIONS,
                                   AddressSpaceBudget::Seal(64 * unit), NumaTopology::Seal({0}));
    GC_EXPECT_TRUE(map != nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(map->GetBaseAddr());
    GC_EXPECT_EQ(map->CommitMemory(reinterpret_cast<void*>(base), unit), unit);
    GC_EXPECT_EQ(map->CommitMemory(reinterpret_cast<void*>(base + 4 * unit), unit), unit);
    *reinterpret_cast<uint64_t*>(base) = 0x12345678;
    *reinterpret_cast<uint64_t*>(base + 4 * unit) = 0xabcdef;
    const size_t capacity = map->GetCommittedSize();
    std::vector<MemMap::BackingSegment> stash;
    GC_EXPECT_TRUE(map->StashSegments({{base, unit}, {base + 4 * unit, unit}}, stash));
    GC_EXPECT_EQ(map->GetCommittedSize(), capacity);
    GC_EXPECT_EQ(map->GetCommittedSize(base, unit), 0U);
    map->RestoreSegments({{base + 2 * unit, 2 * unit}}, stash);
    GC_EXPECT_EQ(map->GetCommittedSize(), capacity);
    GC_EXPECT_EQ(map->GetCommittedSize(base + 2 * unit, 2 * unit), capacity);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(base + 2 * unit), 0x12345678U);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(base + 3 * unit), 0xabcdefU);
    GC_EXPECT_EQ(map->ReleaseMemory(reinterpret_cast<void*>(base + 2 * unit), unit), unit);
    GC_EXPECT_EQ(map->GetCommittedSize(), unit);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(base + 3 * unit), 0xabcdefU);
    GC_EXPECT_EQ(map->CommitMemory(reinterpret_cast<void*>(base), unit), unit);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(base), 0U);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(base + 3 * unit), 0xabcdefU);
    MemMap::DestroyMemMap(map);
}

static void ProductFragmentedAllocation(bool provideContiguousVirtual)
{
    const size_t unit = RegionInfo::UNIT_SIZE;
    const size_t metadata = RegionManager::GetMetadataSize(8);
    MemMap* map = MemMap::MapMemory(metadata + 8 * unit, metadata, MemMap::DEFAULT_OPTIONS,
                                   AddressSpaceBudget::Seal(8 * (metadata + 8 * unit)), NumaTopology::Seal({0}));
    GC_EXPECT_TRUE(map != nullptr);
    {
        RegionManager manager;
        HeapParam parameters{};
        parameters.regionSize = unit / 1024;
        parameters.exemptionThreshold = 0.8;
        manager.Initialize(8, reinterpret_cast<uintptr_t>(map->GetBaseAddr()), *map, parameters, 0.5);
        const auto role = RegionInfo::UnitRole::SMALL_SIZED_UNITS;
        RegionInfo* first = manager.TakeRegion(2, role, false, false, false);
        RegionInfo* second = manager.TakeRegion(2, role, false, false, false);
        RegionInfo* third = manager.TakeRegion(2, role, false, false, false);
        RegionInfo* fourth = manager.TakeRegion(2, role, false, false, false);
        GC_EXPECT_TRUE(first != nullptr && second != nullptr && third != nullptr && fourth != nullptr);
        const uintptr_t firstAddress = first->GetRegionStart();
        const uintptr_t fourthAddress = fourth->GetRegionStart();
        manager.ReclaimRegion(first);
        manager.ReclaimRegion(third);
        GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 4U);
        if (provideContiguousVirtual) { manager.ReleaseRegion(second); }
        const size_t capacity = map->GetCommittedSize();
        RegionInfo* result = manager.TakeRegion(provideContiguousVirtual ? 6 : 4, role, false, true, false);
        if (provideContiguousVirtual) {
            GC_EXPECT_TRUE(result != nullptr);
            GC_EXPECT_EQ(result->GetRegionStart(), firstAddress);
            GC_EXPECT_EQ(result->GetUnitCount(), 6U);
            GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 0U);
            GC_EXPECT_EQ(map->GetCommittedSize(), capacity + 2 * unit);
            GC_EXPECT_EQ(map->GetCommittedSize(firstAddress, 6 * unit), 6 * unit);
        } else {
            GC_EXPECT_TRUE(result == nullptr);
            GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 4U);
            GC_EXPECT_EQ(map->GetCommittedSize(), capacity);
            RegionInfo* recovered = manager.TakeRegion(2, role, false, false, false);
            GC_EXPECT_TRUE(recovered != nullptr);
            GC_EXPECT_EQ(map->GetCommittedSize(), capacity);
        }
        GC_EXPECT_TRUE(RegionInfo::TryGetRegionInfoAt(fourthAddress) == fourth);
    }
    MemMap::DestroyMemMap(map);
}

GC_OTHER_VM_TEST(MappedCache, ProductHarvestRemapsAndCommitsOnlySuffix)
{
    ProductFragmentedAllocation(true);
}

GC_OTHER_VM_TEST(MappedCache, ProductFailedVirtualClaimRestoresOwners)
{
    ProductFragmentedAllocation(false);
}


// TestMappedCacheHarvest.java: keep allocations between reclaimed extents.
// zPageAllocator.cpp:723-743 additionally fixes the owner split: with growth
// room of two units, a four-unit request harvests only two of six cached units.
static void ProductPartialGrowth(bool provideContiguousVirtual)
{
    const size_t unit = RegionInfo::UNIT_SIZE;
    const size_t metadata = RegionManager::GetMetadataSize(12);
    MemMap* map = MemMap::MapMemory(metadata + 12 * unit, metadata, MemMap::DEFAULT_OPTIONS,
                                   AddressSpaceBudget::Seal(8 * (metadata + 12 * unit)), NumaTopology::Seal({0}));
    GC_EXPECT_TRUE(map != nullptr);
    {
        RegionManager manager;
        HeapParam parameters{};
        parameters.regionSize = unit / 1024;
        parameters.exemptionThreshold = 0.8;
        manager.Initialize(12, reinterpret_cast<uintptr_t>(map->GetBaseAddr()), *map, parameters, 0.5);
        const auto role = RegionInfo::UnitRole::SMALL_SIZED_UNITS;
        RegionInfo* regions[6];
        for (auto& region : regions) {
            region = manager.TakeRegion(2, role, false, false, false);
            GC_EXPECT_TRUE(region != nullptr);
        }
        const uintptr_t expectedStart = regions[4]->GetRegionStart();
        // Same-class cache entries are harvested most recently inserted first.
        manager.ReclaimRegion(regions[0]);
        manager.ReclaimRegion(regions[2]);
        manager.ReclaimRegion(regions[4]);
        manager.ReleaseRegion(regions[provideContiguousVirtual ? 5 : 1]);
        GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 6U);
        const size_t capacity = map->GetCommittedSize();
        RegionInfo* result = manager.TakeRegion(4, role, false, false, false);
        if (provideContiguousVirtual) {
            GC_EXPECT_TRUE(result != nullptr);
            GC_EXPECT_EQ(result->GetRegionStart(), expectedStart);
            GC_EXPECT_EQ(result->GetUnitCount(), 4U);
            GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 4U);
            GC_EXPECT_EQ(map->GetCommittedSize(), capacity + 2 * unit);
            GC_EXPECT_EQ(map->GetCommittedSize(expectedStart, 4 * unit), 4 * unit);
        } else {
            GC_EXPECT_TRUE(result == nullptr);
            GC_EXPECT_EQ(manager.GetDirtyUnitCount(), 6U);
            GC_EXPECT_EQ(map->GetCommittedSize(), capacity);
            // Failure must return the pending capacity as well as the backing.
            // Drain all cached units, then use the two units of growth again.
            for (size_t i = 0; i < 3; ++i) {
                GC_EXPECT_TRUE(manager.TakeRegion(2, role, false, false, false) != nullptr);
                GC_EXPECT_EQ(map->GetCommittedSize(), capacity);
            }
            GC_EXPECT_TRUE(manager.TakeRegion(2, role, false, false, false) != nullptr);
            GC_EXPECT_EQ(map->GetCommittedSize(), capacity + 2 * unit);
        }
    }
    MemMap::DestroyMemMap(map);
}

GC_OTHER_VM_TEST(MappedCache, ProductPartialGrowthHarvestsOnlyRemainder)
{
    ProductPartialGrowth(true);
}

GC_OTHER_VM_TEST(MappedCache, ProductPartialGrowthFailureReturnsBothOwners)
{
    ProductPartialGrowth(false);
}

#endif

} // namespace GcUnit
} // namespace MapleRuntime
