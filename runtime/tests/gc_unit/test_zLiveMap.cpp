// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Port of OpenJDK test/hotspot/gtest/gc/z/test_zLiveMap.cpp onto ZLiveMap
// (zLiveMap.hpp:35-101), plus the page-level consumers of the livemap
// (zPage.inline.hpp:223-331) exercised through the product SO:
// WCollector::MarkObject -> RegionInfo::mark_object / inc_live,
// RegionInfo::CloneForPromotion, ZLiveMap::reset / reset_segment.

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

// gc_heap_fixture.hpp first: its access-unlocking window must see zPage.hpp.
#include "gc_heap_fixture.hpp"
#include "Heap/WCollector/WCollector.h"
#include "Heap/z/zLiveMap.inline.hpp"
#include "gc_unittest.hpp"

namespace MapleRuntime {

// Named friend of ZLiveMap (zLiveMap.hpp:36), as in the HotSpot gtest.
class ZLiveMapTest {
public:
    static BitMap::idx_t index_to_segment(const ZLiveMap& livemap, BitMap::idx_t index)
    {
        return livemap.index_to_segment(index);
    }

    static uint32_t segment_size(const ZLiveMap& livemap) { return livemap._segment_size; }

    static void strongly_live_for_large_zpage()
    {
        // Large ZPages only have room for one object.
        ZLiveMap livemap(1);

        bool inc_live;
        BitMap::idx_t object_index = BitMap::idx_t(0);

        // Mark the object strong.
        livemap.set(ZGenerationId::old, object_index, false /* finalizable */, inc_live);

        // Check that both bits are in the same segment.
        GC_EXPECT_EQ(livemap.index_to_segment(0), livemap.index_to_segment(1));

        // Check that the object was marked.
        GC_EXPECT_TRUE(livemap.get(ZGenerationId::old, 0));

        // Check that the object was strongly marked.
        GC_EXPECT_TRUE(livemap.get(ZGenerationId::old, 1));

        GC_EXPECT_TRUE(inc_live);
    }
};

} // namespace MapleRuntime

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

// ZLiveMapTest::SetUp gives the two generations distinct non-zero seqnums
// (old = 1, young = 2); the fixture advances the product generation cycles
// until the same holds.
struct LiveMapGenerations {
    GcHeapFixture fx;
    LiveMapGenerations()
    {
        while (ZLiveMap::generation_seqnum(ZGenerationId::young) == ZLiveMap::generation_seqnum(ZGenerationId::old)) {
            GcHeapFixture::AdvanceGeneration(Generation::Young);
        }
    }
};

} // namespace

GC_TEST(ZLiveMapTest, strongly_live_for_large_zpage)
{
    LiveMapGenerations gens;
    ZLiveMapTest::strongly_live_for_large_zpage();
}

// zLiveMap.inline.hpp:93-98 get() and zBitMap.inline.hpp:55-59: a finalizable
// mark sets only the live bit of the pair; a later strong mark completes the
// pair without a second live claim.
GC_TEST(ZLiveMapTest, finalizable_then_strong_pair)
{
    LiveMapGenerations gens;
    ZLiveMap livemap(512);
    bool inc_live = false;
    const BitMap::idx_t index = 2 * 17;

    GC_EXPECT_TRUE(livemap.set(ZGenerationId::old, index, true /* finalizable */, inc_live));
    GC_EXPECT_TRUE(inc_live);
    GC_EXPECT_TRUE(livemap.get(ZGenerationId::old, index));      // live
    GC_EXPECT_FALSE(livemap.get(ZGenerationId::old, index + 1)); // not strong

    inc_live = true;
    GC_EXPECT_TRUE(livemap.set(ZGenerationId::old, index, false /* strong */, inc_live));
    GC_EXPECT_FALSE(inc_live);
    GC_EXPECT_TRUE(livemap.get(ZGenerationId::old, index));
    GC_EXPECT_TRUE(livemap.get(ZGenerationId::old, index + 1));

    // Neighbouring pairs stay clear.
    GC_EXPECT_FALSE(livemap.get(ZGenerationId::old, index - 2));
    GC_EXPECT_FALSE(livemap.get(ZGenerationId::old, index + 2));
}

// zLiveMap.inline.hpp:41-43: is_marked compares the livemap seqnum with the
// generation seqnum read at call time. Advancing the generation retires every
// bit without touching the map; the next set() resets it (zLiveMap.cpp:53-107).
GC_TEST(ZLiveMapTest, is_marked_reads_generation_seqnum_live)
{
    LiveMapGenerations gens;
    ZLiveMap livemap(512);
    bool inc_live = false;
    GC_EXPECT_FALSE(livemap.is_marked(ZGenerationId::young));
    GC_EXPECT_TRUE(livemap.set(ZGenerationId::young, 4, false, inc_live));
    livemap.inc_live(1, 16);
    GC_EXPECT_TRUE(livemap.is_marked(ZGenerationId::young));
    GC_EXPECT_TRUE(livemap.get(ZGenerationId::young, 4));
    GC_EXPECT_EQ(livemap.live_objects(), 1u);
    GC_EXPECT_EQ(livemap.live_bytes(), 16u);
    // Not marked for the other generation: its seqnum is a different number.
    GC_EXPECT_FALSE(livemap.is_marked(ZGenerationId::old));
    GC_EXPECT_FALSE(livemap.get(ZGenerationId::old, 4));

    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GC_EXPECT_FALSE(livemap.is_marked(ZGenerationId::young));
    GC_EXPECT_FALSE(livemap.get(ZGenerationId::young, 4));

    // First mark of the new cycle resets counters and segments.
    GC_EXPECT_TRUE(livemap.set(ZGenerationId::young, 8, false, inc_live));
    GC_EXPECT_TRUE(inc_live);
    GC_EXPECT_TRUE(livemap.is_marked(ZGenerationId::young));
    GC_EXPECT_EQ(livemap.live_objects(), 0u);
    GC_EXPECT_EQ(livemap.live_bytes(), 0u);
    GC_EXPECT_FALSE(livemap.get(ZGenerationId::young, 4));
    GC_EXPECT_TRUE(livemap.get(ZGenerationId::young, 8));
}

// zLiveMap.inline.hpp:141-158 iterate visits only the live bit of each pair in
// live segments; zLiveMap.inline.hpp:181-222 find_base_bit returns the pair
// start at or below an index, searching earlier segments.
GC_TEST(ZLiveMapTest, iterate_and_find_base_bit)
{
    LiveMapGenerations gens;
    ZLiveMap livemap(4096);
    const uint32_t segment = ZLiveMapTest::segment_size(livemap);
    GC_EXPECT_TRUE(segment >= 2);
    bool inc_live = false;
    const BitMap::idx_t indices[] = { 0, 6, segment + 2, 3 * segment + 10 };
    for (BitMap::idx_t index : indices) {
        GC_EXPECT_TRUE(livemap.set(ZGenerationId::old, index, index == 6 /* finalizable */, inc_live));
    }
    std::vector<BitMap::idx_t> visited;
    livemap.iterate(ZGenerationId::old, [&](BitMap::idx_t index) -> bool {
        visited.push_back(index);
        return true;
    });
    GC_EXPECT_EQ(visited.size(), 4u);
    GC_EXPECT_EQ(visited[0], 0u);
    GC_EXPECT_EQ(visited[1], 6u);
    GC_EXPECT_EQ(visited[2], segment + 2);
    GC_EXPECT_EQ(visited[3], 3 * segment + 10);

    GC_EXPECT_EQ(livemap.find_base_bit(7), 6u);           // strong bit of the pair at 6 aligns down
    GC_EXPECT_EQ(livemap.find_base_bit(segment - 1), 6u); // same segment, earlier pair
    GC_EXPECT_EQ(livemap.find_base_bit(2 * segment + 5), segment + 2); // earlier segment
    GC_EXPECT_EQ(livemap.find_base_bit(4 * segment + 1), 3 * segment + 10);
    GC_EXPECT_EQ(livemap.find_base_bit(0), 0u);

    // Not marked in the other generation: iterate visits nothing.
    size_t visits = 0;
    livemap.iterate(ZGenerationId::young, [&](BitMap::idx_t) -> bool { ++visits; return true; });
    GC_EXPECT_EQ(visits, 0u);
}

// zLiveMap.cpp:53-107: the first marker of a cycle resets the map while its
// peers busy-wait; the seqnum is published only after counters, segment bits
// and the lazily initialized bitmap are reset. Stale bits from the previous
// cycle must never survive into the new one, and no new mark may be lost.
GC_TEST(ZLiveMapTest, concurrent_first_mark_resets_once)
{
    LiveMapGenerations gens;
    ZLiveMap livemap(4096);
    constexpr size_t kThreads = 8;
    constexpr size_t kPerThread = 64;
    constexpr size_t kRounds = 40;
    for (size_t round = 0; round < kRounds; ++round) {
        // Previous cycle: every pair used below plus a stale neighbour is set.
        bool inc_live = false;
        for (size_t t = 0; t < kThreads; ++t) {
            for (size_t i = 0; i < kPerThread; ++i) {
                (void)livemap.set(ZGenerationId::old, 2 * (t * kPerThread + i), false, inc_live);
            }
        }
        (void)livemap.set(ZGenerationId::old, 2 * (kThreads * kPerThread + 3), false, inc_live);
        livemap.inc_live(kThreads * kPerThread + 1, 8 * (kThreads * kPerThread + 1));
        GC_EXPECT_TRUE(livemap.is_marked(ZGenerationId::old));

        GcHeapFixture::AdvanceGeneration(Generation::Old);
        GC_EXPECT_FALSE(livemap.is_marked(ZGenerationId::old));

        std::atomic<bool> go{ false };
        std::atomic<size_t> claims{ 0 };
        std::vector<std::thread> threads;
        for (size_t t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                while (!go.load(std::memory_order_acquire)) {}
                for (size_t i = 0; i < kPerThread; ++i) {
                    bool first = false;
                    const bool marked = livemap.set(ZGenerationId::old, 2 * (t * kPerThread + i), false, first);
                    if (marked && first) {
                        claims.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& thread : threads) {
            thread.join();
        }
        GC_EXPECT_TRUE(livemap.is_marked(ZGenerationId::old));
        // Every new mark is visible and was a first live claim exactly once.
        GC_EXPECT_EQ(claims.load(), kThreads * kPerThread);
        for (size_t t = 0; t < kThreads; ++t) {
            for (size_t i = 0; i < kPerThread; ++i) {
                GC_EXPECT_TRUE(livemap.get(ZGenerationId::old, 2 * (t * kPerThread + i)));
                GC_EXPECT_TRUE(livemap.get(ZGenerationId::old, 2 * (t * kPerThread + i) + 1));
            }
        }
        // The stale neighbour and the counters were reset by the cycle change.
        GC_EXPECT_FALSE(livemap.get(ZGenerationId::old, 2 * (kThreads * kPerThread + 3)));
        GC_EXPECT_EQ(livemap.live_objects(), 0u);
        GC_EXPECT_EQ(livemap.live_bytes(), 0u);
    }
}

// ---- page consumers (zPage.inline.hpp:223-331) through the product SO ----

// WCollector::MarkObject (product SO) -> RegionInfo::mark_object + inc_live.
// The page's live bytes are what ZRelocationSetSelector consumes
// (zRelocationSetSelector.cpp: liveBytes = is_marked() ? live_bytes() : 0).
GC_TEST(ZLiveMapPage, collector_mark_object_accounts_live_once)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    GC_EXPECT_TRUE(region->IsRelocatable());
    GC_EXPECT_FALSE(region->is_marked());
    GC_EXPECT_FALSE(region->is_object_live(from_object(fx.obj0)));

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    GC_EXPECT_FALSE(collector.MarkObject(fx.obj0)); // false = newly marked
    GC_EXPECT_TRUE(region->is_marked());
    GC_EXPECT_TRUE(region->is_object_live(from_object(fx.obj0)));
    GC_EXPECT_TRUE(region->is_object_strongly_live(from_object(fx.obj0)));
    GC_EXPECT_TRUE(region->is_object_marked(from_object(fx.obj0), false));
    GC_EXPECT_EQ(region->live_objects(), 1u);
    GC_EXPECT_EQ(region->live_bytes(), fx.obj0->GetSize());
    GC_EXPECT_FALSE(region->IsKnownEmpty());

    GC_EXPECT_TRUE(collector.MarkObject(fx.obj0)); // already marked: no second claim
    GC_EXPECT_EQ(region->live_objects(), 1u);
    GC_EXPECT_EQ(region->live_bytes(), fx.obj0->GetSize());

    // An unmarked page has no live bytes to offer the selector.
    GC_EXPECT_FALSE(fx.region1->is_marked());
    GC_EXPECT_FALSE(fx.region1->is_object_live(from_object(fx.obj1)));
}

// WCollector::ResurrectObject (product SO) -> mark_object(finalizable = true):
// the object is live but not strongly live (zPage.inline.hpp:254-260).
GC_TEST(ZLiveMapPage, resurrect_is_live_not_strong)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_FALSE(collector.ResurrectObject(fx.obj0, offset, region));
    GC_EXPECT_TRUE(region->is_object_live(from_object(fx.obj0)));
    GC_EXPECT_FALSE(region->is_object_strongly_live(from_object(fx.obj0)));
    GC_EXPECT_TRUE(region->is_object_marked(from_object(fx.obj0), true));
    GC_EXPECT_FALSE(region->is_object_marked(from_object(fx.obj0), false));
    GC_EXPECT_TRUE(RegionSpace::IsResurrectedObject(fx.obj0));
    GC_EXPECT_EQ(region->live_bytes(), fx.obj0->GetSize());

    // The strong mark completes the pair without another live claim.
    GC_EXPECT_FALSE(collector.MarkObject(fx.obj0));
    GC_EXPECT_TRUE(region->is_object_strongly_live(from_object(fx.obj0)));
    GC_EXPECT_FALSE(RegionSpace::IsResurrectedObject(fx.obj0));
    GC_EXPECT_EQ(region->live_objects(), 1u);
    GC_EXPECT_EQ(region->live_bytes(), fx.obj0->GetSize());
}

// ZPage::object_iterate (zPage.inline.hpp:319-331) visits object starts only.
GC_TEST(ZLiveMapPage, object_iterate_visits_object_starts)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    BaseObject* second = fx.PlaceObject(region->GetRegionStart() + 256);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + second->GetSize());
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, fx.obj0));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, second));
    std::vector<BaseObject*> visited;
    region->object_iterate([&](BaseObject* obj) { visited.push_back(obj); });
    GC_EXPECT_EQ(visited.size(), 2u);
    GC_EXPECT_TRUE(visited[0] == fx.obj0);
    GC_EXPECT_TRUE(visited[1] == second);
    GC_EXPECT_EQ(region->live_objects(), 2u);
    GC_EXPECT_EQ(region->live_bytes(), fx.obj0->GetSize() + second->GetSize());
}

// ZPage::find_base (zPage.inline.hpp:371-392): an interior field address
// resolves to the nearest marked object start at or below it.
GC_TEST(ZLiveMapPage, find_base_resolves_interior_field)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    BaseObject* second = fx.PlaceObject(region->GetRegionStart() + 256);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + second->GetSize());
    // Nothing marked yet: no base (zaddress_unsafe::null).
    GC_EXPECT_EQ(region->find_base_unsafe(reinterpret_cast<MAddress>(second) + 8), 0u);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, fx.obj0));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, second));
    GC_EXPECT_EQ(region->find_base(reinterpret_cast<MAddress>(second) + 8), reinterpret_cast<MAddress>(second));
    GC_EXPECT_EQ(region->find_base(reinterpret_cast<MAddress>(second)), reinterpret_cast<MAddress>(second));
    GC_EXPECT_EQ(region->find_base(reinterpret_cast<MAddress>(fx.obj0) + 24), reinterpret_cast<MAddress>(fx.obj0));
    // Below the first marked object there is no base.
    GC_EXPECT_EQ(region->find_base(region->GetRegionStart() + 8), 0u);
}

// ZPage::clone_for_promotion / ZPage::object_iterate: the promoted slot gets a
// fresh livemap while the original young page keeps its own map.
GC_TEST(ZLiveMapPage, clone_for_promotion_keeps_original_livemap)
{
    for (bool large : { false, true }) {
        const auto role = large ? RegionInfo::UnitRole::LARGE_SIZED_UNITS
                                : RegionInfo::UnitRole::SMALL_SIZED_UNITS;
        GcHeapFixture fx(false, role);
        RegionInfo* region = fx.region0;
        region->SetYoungRegionFlag(1);
        region->SetYoungAge(1);
        BaseObject* object = large ? fx.PlaceObject(region->GetRegionStart()) : fx.obj0;
        ZLiveMap* original = region->livemap();
        GC_EXPECT_TRUE(original != nullptr);
        GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, object));
        GC_EXPECT_TRUE(region->is_object_live(from_object(object)));
        auto originalPage = region->CloneForPromotion();
        GC_EXPECT_FALSE(region->IsYoungRegion());
        GC_EXPECT_EQ(originalPage->Age(), 1u);
        GC_EXPECT_TRUE(region->livemap() != original);
        GC_EXPECT_TRUE(region->livemap() != nullptr);
        GC_EXPECT_FALSE(region->livemap()->is_marked(ZGenerationId::old));
        std::vector<BaseObject*> visited;
        originalPage->ObjectIterate([&](BaseObject* obj) { visited.push_back(obj); });
        GC_EXPECT_EQ(visited.size(), 1u);
        GC_EXPECT_TRUE(visited[0] == object);
        // Advancing the young generation retires the original page's marks.
        GcHeapFixture::AdvanceGeneration(Generation::Young);
        visited.clear();
        originalPage->ObjectIterate([&](BaseObject* obj) { visited.push_back(obj); });
        GC_EXPECT_EQ(visited.size(), 0u);
    }
}

// ZPage::is_object_live on an allocating page is implicitly true
// (zPage.inline.hpp:254-256); after the generation advances the page is
// relocatable and only marked objects are live.
GC_TEST(ZLiveMapPage, allocating_page_is_implicitly_live)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->ResetPageSequence();
    GC_EXPECT_TRUE(region->IsAllocating());
    GC_EXPECT_TRUE(region->is_object_live(from_object(fx.obj0)));
    GC_EXPECT_TRUE(region->is_object_strongly_live(from_object(fx.obj0)));
    GC_EXPECT_FALSE(region->IsKnownEmpty());
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    GC_EXPECT_TRUE(region->IsRelocatable());
    GC_EXPECT_FALSE(region->is_object_live(from_object(fx.obj0)));
    GC_EXPECT_FALSE(region->is_marked());
}

// ZPage.cpp:33-42 constructs the live map after setting the page type. Exercise
// the same InitRegion entry used by MaterializePageMemory, including reuse.
GC_TEST(ZLiveMapPage, initialization_uses_current_page_role)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    const uint32_t smallSegment = ZLiveMapTest::segment_size(*region->livemap());
    GC_EXPECT_TRUE(smallSegment > 2u);
    for (auto role : {RegionInfo::UnitRole::LARGE_SIZED_UNITS,
                      RegionInfo::UnitRole::SMALL_SIZED_UNITS,
                      RegionInfo::UnitRole::LARGE_SIZED_UNITS}) {
        RegionInfo::RetirePage(region, [] {});
        region = RegionInfo::InitRegion(0, 1, role);
        fx.region0 = region;
        const uint32_t actual = ZLiveMapTest::segment_size(*region->livemap());
        const uint32_t expected = role == RegionInfo::UnitRole::LARGE_SIZED_UNITS ? 2u : smallSegment;
        std::fprintf(stderr, "P02_PAGE_GEOMETRY role=%u segment=%u expected=%u\n",
                     static_cast<unsigned>(role), actual, expected);
        GC_EXPECT_EQ(actual, expected);
    }
}

// ZGeneration.cpp:137 starts at one; ZLiveMap's zero denotes never marked.
// Read the actual product collector before GcHeapFixture can advance a cycle.
GC_TEST(ZLiveMapTest, initial_generation_does_not_match_unmarked_map)
{
    ZLiveMap map(1);
    for (auto id : {ZGenerationId::young, ZGenerationId::old}) {
        const uint64_t sequence = ZLiveMap::generation_seqnum(id);
        const bool marked = map.is_marked(id);
        std::fprintf(stderr, "P02_INITIAL_LIVEMAP generation=%u sequence=%llu marked=%d\n",
                     static_cast<unsigned>(id), static_cast<unsigned long long>(sequence), marked);
        // The target is the live-map reader's result, not just the input sequence.
        GC_EXPECT_FALSE(marked);
        GC_EXPECT_TRUE(sequence != 0);
    }
    // No AdvanceGeneration: this is the first mark in the old generation.
    bool incLive = false;
    GC_EXPECT_TRUE(map.set(ZGenerationId::old, 0, true, incLive));
    GC_EXPECT_TRUE(map.get(ZGenerationId::old, 0));
    GC_EXPECT_FALSE(map.get(ZGenerationId::old, 1));
    GC_EXPECT_TRUE(incLive);
}

namespace {

// Keep the two retired tests' invariant while using the current public product
// entry. ZGC zBitMap.inline.hpp:61-81 elects one successful strong marker;
// zMark.cpp:405-425 accounts the first live result once.
void ConcurrentSameObjectMark(bool large, bool initiallyFinalizable)
{
    GcHeapFixture fx(false, large ? RegionInfo::UnitRole::LARGE_SIZED_UNITS
                                  : RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* region = fx.region0;
    BaseObject* object = large ? fx.PlaceObject(region->GetRegionStart()) : fx.obj0;
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(object) + object->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    if (initiallyFinalizable) {
        GC_EXPECT_FALSE(collector.ResurrectObject(object, region->GetAddressOffset(
            reinterpret_cast<MAddress>(object)), region));
    }
    std::atomic<unsigned> ready{0};
    bool already[2] = {true, true};
    auto mark = [&](unsigned worker) {
        ready.fetch_add(1, std::memory_order_release);
        while (ready.load(std::memory_order_acquire) != 2) {
            std::this_thread::yield();
        }
        already[worker] = collector.MarkObject(object);
    };
    std::thread first(mark, 0);
    std::thread second(mark, 1);
    first.join();
    second.join();
    const unsigned claims = unsigned(!already[0]) + unsigned(!already[1]);
    const uint32_t liveObjects = region->live_objects();
    const size_t liveBytes = region->live_bytes();
    const bool live = region->is_object_live(from_object(object));
    const bool strong = region->is_object_strongly_live(from_object(object));
    std::fprintf(stderr, "P02_SAME_OBJECT large=%d upgrade=%d claims=%u objects=%u bytes=%zu live=%d strong=%d\n",
                 large, initiallyFinalizable, claims, liveObjects, liveBytes, live, strong);
    GC_EXPECT_EQ(claims, 1u);
    GC_EXPECT_EQ(liveObjects, 1u);
    GC_EXPECT_EQ(liveBytes, object->GetSize());
    GC_EXPECT_TRUE(live);
    GC_EXPECT_TRUE(strong);
}

} // namespace

GC_TEST(LiveMap, LargeFirstPaintHasSingleWinner)
{
    ConcurrentSameObjectMark(true, false);
}

GC_TEST(ZLiveMapPort, ConcurrentDuplicateSatbPublicationHasOneStrongReceipt)
{
    ConcurrentSameObjectMark(false, false);
}

GC_TEST(ZLiveMapPage, concurrent_strong_upgrade_counts_live_once)
{
    ConcurrentSameObjectMark(false, true);
    ConcurrentSameObjectMark(true, true);
}
