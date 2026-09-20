// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// ZValue storage contract (zValue.inline.hpp:45-239). ZGC's gtest tree has no
// test_zValue; these cover spec invariant 1: the same id always names the same
// slot, ZPerWorker has ConcGCThreads slots and ZPerCPU has ZCPU::count() slots,
// and the product per-CPU shared-small-page slot is such a ZPerCPU value.

#include <thread>
#include <vector>

// gc_heap_fixture.hpp opens the product privates for the harness; it has to
// come before every product header this file names.
#include "gc_heap_fixture.hpp"
#include "Heap/z/zCPU.inline.hpp"
#include "Heap/z/zObjectAllocator.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zValue.inline.hpp"
#include "gc_unittest.hpp"
#include "zunittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ZValue, per_cpu_slot_count_and_identity)
{
    ZPerCPU<int> value(7);
    GC_EXPECT_EQ(value.count(), ZCPU::count());
    GC_EXPECT_TRUE(value.count() >= 1u);

    // Each id has its own slot; the same id always returns the same slot.
    for (uint32_t id = 0; id < value.count(); ++id) {
        GC_EXPECT_TRUE(value.addr(id) == value.addr(id));
        GC_EXPECT_EQ(value.get(id), 7);
        value.set(static_cast<int>(id) + 100, id);
    }
    for (uint32_t id = 0; id < value.count(); ++id) {
        GC_EXPECT_EQ(value.get(id), static_cast<int>(id) + 100);
        if (id > 0) {
            GC_EXPECT_TRUE(value.addr(id) != value.addr(id - 1));
        }
    }

    // Slots of one value are ZValueStorage::Offset apart (zValue.inline.hpp:118).
    if (value.count() > 1) {
        GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(value.addr(1)) - reinterpret_cast<uintptr_t>(value.addr(0)),
                     ZPerCPUStorage::Offset);
    }

    // Default id is the current CPU, which is a valid slot.
    const uint32_t cpu = ZCPU::id();
    GC_EXPECT_TRUE(cpu < value.count());
    GC_EXPECT_TRUE(value.addr() == value.addr(cpu));

    // Iterator visits every slot exactly once in id order.
    uint32_t visited = 0;
    ZPerCPUIterator<int> iter(&value);
    uint32_t id;
    for (int* slot; iter.next(&slot, &id);) {
        GC_EXPECT_EQ(id, visited);
        GC_EXPECT_TRUE(slot == value.addr(id));
        ++visited;
    }
    GC_EXPECT_EQ(visited, value.count());

    value.set_all(3);
    for (uint32_t i = 0; i < value.count(); ++i) {
        GC_EXPECT_EQ(value.get(i), 3);
    }
}

GC_TEST(ZValue, per_worker_slot_count_follows_conc_gc_threads)
{
    const uint32_t saved = ConcGCThreads;
    ConcGCThreads = 5;
    ZPerWorker<uint32_t> value(ZValueIdTagType{});
    GC_EXPECT_EQ(value.count(), 5u);
    for (uint32_t id = 0; id < 5; ++id) {
        GC_EXPECT_EQ(value.get(id), id);
    }
    GC_EXPECT_EQ(ZContendedStorage::count(), 1u);
    GC_EXPECT_EQ(ZPerNUMAStorage::count(), 1u);
    ConcGCThreads = saved;
}

#if defined(__linux__)
// Product wiring: PerAgeObjectAllocator::sharedSmallPage is a ZPerCPU<ZPage*>
// (zObjectAllocator.hpp:41); retire_pages clears every CPU slot (set_all,
// zObjectAllocator.cpp:198-203).
GC_OTHER_VM_TEST(ZValue, shared_small_page_is_per_cpu_storage)
{
    ZStat::Initialize();
    constexpr size_t units = 64;
    HeapParam params{};
    params.regionSize = ZGranuleSize / KB;
    params.exemptionThreshold = 0.8;
    std::unique_ptr<ZTestRegionHeap> heap;
    RegionManager manager;
    heap.reset(new ZTestRegionHeap(units, manager, params, 0.5));
    BindFixturePageTable(manager, units);

    auto& allocator = *Heap::GetHeap().object_allocator().allocator(PageAge::eden);
    GC_EXPECT_EQ(allocator.sharedSmallPage.count(), ZCPU::count());
    for (uint32_t cpu = 0; cpu < allocator.sharedSmallPage.count(); ++cpu) {
        GC_EXPECT_TRUE(allocator.sharedSmallPage.get(cpu) == nullptr);
    }
    GC_EXPECT_TRUE(allocator.shared_small_page_addr() == allocator.sharedSmallPage.addr(ZCPU::id()));

    // An allocation installs the current CPU's slot and no other slot.
    const uintptr_t first = Heap::GetHeap().object_allocator().alloc(16, PageAge::eden, true);
    GC_EXPECT_TRUE(first != 0);
    const uint32_t cpu = ZCPU::id();
    GC_EXPECT_TRUE(allocator.sharedSmallPage.get(cpu) == Heap::page(first));
    for (uint32_t other = 0; other < allocator.sharedSmallPage.count(); ++other) {
        if (other != cpu) {
            GC_EXPECT_TRUE(allocator.sharedSmallPage.get(other) == nullptr);
        }
    }

    // retire_pages: every slot of the age is cleared.
    Heap::GetHeap().object_allocator().retire_pages(PageAgeRange::create<PageAge::eden, PageAge::survivor1>());
    for (uint32_t other = 0; other < allocator.sharedSmallPage.count(); ++other) {
        GC_EXPECT_TRUE(allocator.sharedSmallPage.get(other) == nullptr);
    }
    Heap::bind_test_page_allocator(nullptr);
}
#endif
