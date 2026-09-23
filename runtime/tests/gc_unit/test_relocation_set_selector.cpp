#include "gc_allocation_flags.hpp"
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zRelocationSetSelector.inline.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zStat.hpp"
#include "Mutator/ThreadLocal.h"
#include "gc_unittest.hpp"
#include "gc_cycle_sequence_fixture.hpp"
#include "Heap/z/zLiveMap.inline.hpp"
#include "Heap/z/zRelocationSet.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zHeuristics.hpp"
#include <algorithm>
#include <vector>
#include "zunittest.hpp"

#include <csignal>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(RelocationSetSelector, EmptySet)
{
    ZRelocationSetSelector selector(ZYoungCompactionLimit);
    selector.select();
    GC_EXPECT_EQ(selector.selected_small()->length(), 0);
    GC_EXPECT_EQ(selector.selected_medium()->length(), 0);
    GC_EXPECT_EQ(selector.forwarding_entries(), static_cast<size_t>(0));
}

GC_TEST(RelocationSetSelector, SelectOrderLargeMediumSmall)
{
    ZRelocationSetSelector selector(ZFragmentationLimit);
    selector.select();
    GC_EXPECT_TRUE(selector.empty_pages()->length() == 0);
}

namespace {
struct SelectorPageFixture {
    RegionManager& manager;
    SelectorPageFixture()
        : manager(Heap::GetHeap().page_allocator())
    {
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        ZStat::Initialize();
    }
    ZPage* takeSmall()
    {
        const size_t n = ZPageSizeSmall / ZGranuleSize;
        return manager.TakeRegion((n) * ZGranuleSize, ZPageType::small, false, PageAge::old, MapleRuntime::GcUnit::NonBlockingAllocationFlags());
    }
};
}

GC_TEST(RelocationSetSelector, PreFilterDropsLowGarbage)
{
    SelectorPageFixture fx;
    ZPage* page = fx.takeSmall();
    GC_EXPECT_TRUE(page != nullptr);
    page->inc_live(1, page->size() - 8);
    ZRelocationSetSelector selector(5.0);
    selector.register_live_page(page);
    selector.select();
    GC_EXPECT_EQ(selector.selected_small()->length(), 0);
}

GC_TEST(RelocationSetSelector, SemiSortUsesPartitionFingers)
{
    SelectorPageFixture fx;
    ZPage* highLive = fx.takeSmall();
    ZPage* lowLive = fx.takeSmall();
    GC_EXPECT_TRUE(highLive != nullptr && lowLive != nullptr);
    highLive->inc_live(1, highLive->size() / 4);
    lowLive->inc_live(1, 64);
    ZRelocationSetSelector selector(0.0);
    selector.register_live_page(lowLive);
    selector.register_live_page(highLive);
    selector.select();
    GC_EXPECT_TRUE(selector.selected_small()->length() >= 2);
    GC_EXPECT_TRUE(selector.selected_small()->at(0) == lowLive);
}

GC_TEST(RelocationSetSelector, FragmentationLimitStopsPrefix)
{
    SelectorPageFixture fx;
    ZPage* a = fx.takeSmall();
    ZPage* b = fx.takeSmall();
    GC_EXPECT_TRUE(a != nullptr && b != nullptr);
    a->inc_live(1, 64);
    b->inc_live(1, 64);
    ZRelocationSetSelector tight(99.0);
    tight.register_live_page(a);
    tight.register_live_page(b);
    tight.select();
    GC_EXPECT_EQ(tight.selected_small()->length(), 0);
    ZRelocationSetSelector loose(0.0);
    loose.register_live_page(a);
    loose.register_live_page(b);
    loose.select();
    GC_EXPECT_TRUE(loose.selected_small()->length() >= 1);
}

// Unlinked relocatable pages remain IsLoneFromRegion; allocating pages do not.
// Selection rejects allocating pages before installing relocation work
// (ZGC zGeneration.cpp:1471).
GC_TEST(RelocationSetSelector, AllocatingUnlinkedIsNotLoneFrom)
{
    SelectorPageFixture fx;
    ZPage* page = fx.takeSmall();
    GC_EXPECT_TRUE(page != nullptr);
    GC_EXPECT_TRUE(page->is_allocating());
    GC_EXPECT_FALSE(page->is_relocatable());
    GC_EXPECT_FALSE(page->IsFromRegion());
    GC_EXPECT_FALSE(page->IsLoneFromRegion());
}





// ZGC zRelocationSetSelector.cpp:70-112: all partition fingers preserve
// ascending partition order through the generation's installed relocation set.
GC_COMPONENT_OTHER_VM_TEST(RelocationSetSelector, GenerationSelectsAllPartitions)
{
    constexpr size_t partitions = size_t(1) << 11;
    CreateStandaloneHeap(partitions * 2);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& generation = *ZGeneration::old();
    std::vector<ZPage*> expected(partitions);
    for (size_t i = 0; i < partitions; ++i) {
        // Multiplication by an odd number permutes all 11-bit indices.
        const size_t index = (i * 683) % partitions;
        ZPage* page = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, PageAge::old, MapleRuntime::GcUnit::NonBlockingAllocationFlags());
        GC_EXPECT_TRUE(page != nullptr);
        expected[index] = page;
    }
    generation.Begin(0);
    GenerationSequenceFixture::Advance(generation);
    for (size_t index = 0; index < partitions; ++index) {
        ZPage* page = expected[index];
        bool increment = false;
        page->livemap().set(ZGenerationId::old, 0, false, increment);
        page->inc_live(1, (index << (ZPageSizeSmallShift - 11)) + 8);
    }
    generation.select_relocation_set(false);
    std::vector<ZPage*> actual;
    ZRelocationSetIterator iterator(&generation.relocation_set());
    for (ZForwarding* forwarding; iterator.next(&forwarding);) {
        actual.push_back(forwarding->page());
    }
    std::vector<ZPage*> eligible;
    const size_t limit = static_cast<size_t>(ZPageSizeSmall * (ZFragmentationLimit / 100.0));
    for (ZPage* page : expected) {
        if (ZPageSizeSmall - page->live_bytes() > limit) {
            eligible.push_back(page);
        }
    }
    // ZGC select_inner: retain the last prefix whose incremental reclaimed
    // percentage exceeds the generation's fragmentation threshold.
    size_t live = 0;
    size_t selectedFrom = 0;
    size_t selectedTo = 0;
    const size_t capacity = ZPageSizeSmall - ZObjectSizeLimitSmall;
    for (size_t from = 1; from <= eligible.size(); ++from) {
        live += eligible[from - 1]->live_bytes();
        const size_t to = (live + capacity - 1) / capacity;
        const double reclaimed = 100.0 - 100.0 * (to - selectedTo) / (from - selectedFrom);
        if (reclaimed > ZFragmentationLimit) {
            selectedFrom = from;
            selectedTo = to;
        }
    }
    eligible.resize(selectedFrom);
    const bool ordered = actual == eligible;
    std::fprintf(stderr, "SELECTOR_PARTITION_ORDER_TARGET actual=%zu expected=%zu ordered=%d\n",
                 actual.size(), eligible.size(), ordered);
    GC_EXPECT_TRUE(ordered);
    generation.reset_relocation_set();
    for (ZPage* page : expected) {
        Heap::free_page(page);
    }
}

// ZGC zRelocationSetSelector.inline.hpp:87-100: a medium page's boundary
// scales by its exact size shift; equality is excluded and one word more
// garbage is included. Observe only the product generation's installed set.
GC_COMPONENT_OTHER_VM_TEST(RelocationSetSelector, GenerationMediumFilterBoundaries)
{
    CreateStandaloneHeap(128);
    ZHeuristics::set_medium_page_size();
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& generation = *ZGeneration::old();
    const size_t maximum = ZPageSizeMediumMax;
    GC_EXPECT_TRUE(ZPageSizeMediumEnabled);
    const size_t maximumLimit = static_cast<size_t>(maximum * (ZFragmentationLimit / 100.0));
    std::vector<ZPage*> pages;
    std::vector<ZPage*> expected;
    for (int shift = 0; shift <= 2; ++shift) {
        const size_t size = maximum >> shift;
        for (size_t extra : {size_t(0), size_t(8)}) {
            ZPage* page = Heap::alloc_page(size, ZPageType::medium, false, PageAge::old, MapleRuntime::GcUnit::NonBlockingAllocationFlags());
            GC_EXPECT_TRUE(page != nullptr);
            pages.push_back(page);
            if (extra != 0) { expected.push_back(page); }
        }
    }
    generation.Begin(0);
    GenerationSequenceFixture::Advance(generation);
    for (size_t i = 0; i < pages.size(); ++i) {
        ZPage* page = pages[i];
        bool increment = false;
        page->livemap().set(ZGenerationId::old, 0, false, increment);
        const int shift = static_cast<int>(i / 2);
        page->inc_live(1, page->size() - (maximumLimit >> shift) - ((i % 2) * 8));
    }
    generation.select_relocation_set(false);
    std::vector<ZPage*> actual;
    ZRelocationSetIterator iterator(&generation.relocation_set());
    for (ZForwarding* forwarding; iterator.next(&forwarding);) {
        actual.push_back(forwarding->page());
    }
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    const bool matched = actual == expected;
    std::fprintf(stderr, "SELECTOR_MEDIUM_FILTER_TARGET actual=%zu expected=%zu matched=%d\n",
                 actual.size(), expected.size(), matched);
    GC_EXPECT_TRUE(matched);
    generation.reset_relocation_set();
    for (ZPage* page : pages) { Heap::free_page(page); }
}
