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
#include "gc_verify_fixture.hpp"
#include "Heap/z/zCPU.inline.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zObjectAllocator.hpp"
#include "Mutator/MutatorManager.h"
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
    VerifyRuntime runtime;
    ZStat::Initialize();

    auto& allocator = *Heap::GetHeap().object_allocator().allocator(PageAge::eden);
    GC_EXPECT_EQ(allocator.sharedSmallPage.count(), ZCPU::count());
    for (uint32_t cpu = 0; cpu < allocator.sharedSmallPage.count(); ++cpu) {
        GC_EXPECT_TRUE(allocator.sharedSmallPage.get(cpu) == nullptr);
    }
    const uint32_t expectedCpu = ZHeuristics::use_per_cpu_shared_small_pages() ? ZCPU::id() : 0;
    GC_EXPECT_TRUE(allocator.shared_small_page_addr() == allocator.sharedSmallPage.addr(expectedCpu));

    // An allocation installs the current CPU's slot and no other slot.
    const uintptr_t first = Heap::GetHeap().object_allocator().alloc_for_relocation(16, PageAge::eden);
    GC_EXPECT_TRUE(first != 0);
    const uint32_t cpu = ZHeuristics::use_per_cpu_shared_small_pages() ? ZCPU::id() : 0;
    GC_EXPECT_TRUE(allocator.sharedSmallPage.get(cpu) == Heap::page(first));
    for (uint32_t other = 0; other < allocator.sharedSmallPage.count(); ++other) {
        if (other != cpu) {
            GC_EXPECT_TRUE(allocator.sharedSmallPage.get(other) == nullptr);
        }
    }

    // retire_pages: every slot of the age is cleared.
    {
        ScopedStopTheWorld stopped("object allocator retirement");
        Heap::GetHeap().object_allocator().retire_pages(PageAgeRange::create<PageAge::eden, PageAge::survivor1>());
    }
    for (uint32_t other = 0; other < allocator.sharedSmallPage.count(); ++other) {
        GC_EXPECT_TRUE(allocator.sharedSmallPage.get(other) == nullptr);
    }
}
#endif

namespace {
struct MutableValueStorage : ZValueStorage<MutableValueStorage> {
    static uint32_t slots;
    static size_t alignment() { return sizeof(uintptr_t); }
    static uint32_t count() { return slots; }
    static uint32_t id() { return 0; }
};
uint32_t MutableValueStorage::slots = 4;
}

// Shrinking the logical domain keeps every accessed slot inside the original
// allocation while distinguishing live S::count() from a construction cache.
GC_TEST(ZValue, iterator_observes_current_storage_count)
{
    MutableValueStorage::slots = 4;
    ZValue<MutableValueStorage, int> value(17);
    ZValueIterator<MutableValueStorage, int> iter(&value);
    ZValueIterator<MutableValueStorage, int> ids(&value);
    ZValueConstIterator<MutableValueStorage, int> constIter(&value);
    int* slot;
    const int* constSlot;
    uint32_t id;
    iter.next(&slot);
    ids.next(&slot, &id);
    constIter.next(&constSlot);
    MutableValueStorage::slots = 2;
    unsigned plain = 1, indexed = 1, immutable = 1;
    while (iter.next(&slot)) { ++plain; }
    while (ids.next(&slot, &id)) { ++indexed; }
    while (constIter.next(&constSlot)) { ++immutable; }
    const auto count = value.count();
    MutableValueStorage::slots = 4;
    std::fprintf(stderr, "VALUE1331 count=%u plain=%u indexed=%u const=%u expected=2\n",
                 count, plain, indexed, immutable);
    GC_EXPECT_TRUE(count == 2 && plain == 2 && indexed == 2 && immutable == 2);
}

GC_TEST(ZValue, storage_reuses_existing_block_after_count_change)
{
    MutableValueStorage::slots = 4;
    const uintptr_t first = MutableValueStorage::alloc(sizeof(uintptr_t));
    MutableValueStorage::slots = 2;
    const uintptr_t second = MutableValueStorage::alloc(sizeof(uintptr_t));
    MutableValueStorage::slots = 4;
    std::fprintf(stderr, "VALUE1331 allocation_stride=%zu expected=%zu\n",
                 size_t(second - first), sizeof(uintptr_t));
    GC_EXPECT_EQ(second - first, sizeof(uintptr_t));
}
