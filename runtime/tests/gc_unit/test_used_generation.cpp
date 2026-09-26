// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// used_generation(id) equals the page-table sum of non-free pages of that
// generation (zPageAllocator.cpp:1375-1390).

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zPageTable.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

static size_t PageTableUsed(ZGenerationId id)
{
    size_t sum = 0;
    ZPageTableIterator iter(&Heap::page_table());
    for (ZPage* page; iter.next(&page);) {
        if (page == nullptr || page->GetRegionRole() == ZPageRole::None) {
            continue;
        }
        if (page->generation_id() == id) {
            sum += page->size();
        }
    }
    return sum;
}

GC_TEST(UsedGeneration, CounterMatchesPageTableNonFreeSum)
{
    GcHeapFixture fx;
    RegionManager& manager = Heap::GetHeap().page_allocator();
    fx.region0()->SetRegionRole(ZPageRole::RecentFull);
    const size_t young = manager.used_generation(ZGenerationId::young);
    const size_t table = PageTableUsed(ZGenerationId::young);
    std::printf("USED_GENERATION young_counter=%zu page_table=%zu region0_size=%zu\n",
                young, table, fx.region0()->size());
    GC_EXPECT_EQ(young, table);
}

GC_TEST(UsedGeneration, DecreaseMovesCounter)
{
    GcHeapFixture fx;
    RegionManager& manager = Heap::GetHeap().page_allocator();
    const size_t before = manager.used_generation(ZGenerationId::young);
    manager.increase_used_generation(ZGenerationId::young, fx.region0()->size());
    GC_EXPECT_EQ(manager.used_generation(ZGenerationId::young), before + fx.region0()->size());
    manager.decrease_used_generation(ZGenerationId::young, fx.region0()->size());
    GC_EXPECT_EQ(manager.used_generation(ZGenerationId::young), before);
}
