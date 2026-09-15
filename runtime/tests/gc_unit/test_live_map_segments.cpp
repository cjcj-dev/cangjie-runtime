// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <dlfcn.h>
#include <thread>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
using ProductMarkObject = bool (*)(RegionInfo*, MarkView<Generation::Old>, const BaseObject*, size_t, bool);

ProductMarkObject ProductMark()
{
    void* runtime = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
    GC_EXPECT_TRUE(runtime != nullptr);
    auto mark = reinterpret_cast<ProductMarkObject>(dlsym(runtime,
        "_ZN12MapleRuntime10RegionInfo10MarkObjectILNS_10GenerationE1EEEbNS_8MarkViewIXT_EEEPKNS_10BaseObjectEmb"));
    GC_EXPECT_TRUE(mark != nullptr);
    return mark;
}
} // namespace

// Extension of ZLiveMapTest.strongly_live_for_large_zpage: the live/strong pair
// shares one segment; promotion to strong does not claim objects/bytes again.
GC_TEST(ZLiveMapPort, FinalizableUpgradeAccountsOnce)
{
    constexpr size_t pageSize = 4096;
    RegionBitmap* bitmap = GcHeapFixture::AllocPlantedBitmap(pageSize);
    bool incLive = false;
    GC_EXPECT_TRUE(bitmap->MarkFinalizableBits(64, 32, pageSize, incLive));
    GC_EXPECT_TRUE(incLive);
    GC_EXPECT_EQ(bitmap->GetLiveObjects(), size_t(0));
    bitmap->AddLiveCounts(1, 32);
    GC_EXPECT_TRUE(bitmap->IsFinalizable(64));
    GC_EXPECT_TRUE(bitmap->MarkBits(64, 32, pageSize, incLive));
    GC_EXPECT_FALSE(incLive);
    GC_EXPECT_TRUE(bitmap->IsMarked(64));
    GC_EXPECT_EQ(bitmap->GetLiveObjects(), size_t(1));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), size_t(32));
    GcHeapFixture::FreePlantedBitmap(bitmap);
}

// ZLiveMap::reset_segment contention: same segment has one clearer; independent
// segments publish separately. All marks must survive both kinds of first touch.
GC_TEST(ZLiveMapPort, ConcurrentFirstTouchSameAndDifferentSegments)
{
    constexpr size_t pageSize = 16384;
    constexpr size_t workers = 8;
    RegionBitmap* bitmap = GcHeapFixture::AllocPlantedBitmap(pageSize);
    std::atomic<bool> go {false};
    std::thread threads[workers];
    for (size_t worker = 0; worker < workers; ++worker) {
        threads[worker] = std::thread([&, worker]() {
            while (!go.load(std::memory_order_acquire)) {}
            bitmap->MarkBits(worker * 8, 8, pageSize);
            bitmap->MarkBits((worker + 1) * RegionBitmap::kRegionBytesPerWord, 8, pageSize);
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    for (size_t worker = 0; worker < workers; ++worker) {
        GC_EXPECT_TRUE(bitmap->IsMarked(worker * 8));
        GC_EXPECT_TRUE(bitmap->IsMarked((worker + 1) * RegionBitmap::kRegionBytesPerWord));
    }
    GC_EXPECT_EQ(bitmap->GetLiveObjects(), workers * 2);
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), workers * 16);
    GcHeapFixture::FreePlantedBitmap(bitmap);
}

// ZLiveMap::reset does not clear backing words. Unpublished segments are absent
// from both point queries and snapshot iteration until their first mark.
GC_TEST(ZLiveMapPort, MetadataResetHidesUntouchedSegments)
{
    constexpr size_t pageSize = 4096;
    RegionBitmap* bitmap = GcHeapFixture::AllocPlantedBitmap(pageSize);
    bitmap->MarkBits(0, 8, pageSize);
    bitmap->MarkBits(1024, 8, pageSize);
    const uint64_t previous = bitmap->markWords[4].load(std::memory_order_relaxed);
    bitmap->Reset();
    GC_EXPECT_EQ(bitmap->markWords[4].load(std::memory_order_relaxed), previous);
    GC_EXPECT_FALSE(bitmap->IsMarked(1024));
    GC_EXPECT_EQ(bitmap->GetLiveWord(4), uint64_t(0));
    bitmap->MarkBits(0, 8, pageSize);
    GC_EXPECT_TRUE(bitmap->IsMarked(0));
    GC_EXPECT_FALSE(bitmap->IsMarked(1024));
    GC_EXPECT_EQ(bitmap->GetLiveObjects(), size_t(1));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), size_t(8));
    GcHeapFixture::FreePlantedBitmap(bitmap);
}

GC_TEST(ZLiveMapPort, GenerationChangeLazilyInitializesProductMap)
{
    GcHeapFixture fx;
    auto* region = fx.region0;
    auto* object = fx.obj0;
    auto mark = ProductMark();
    auto first = region->GetMarkView<Generation::Old>();
    GC_EXPECT_TRUE(mark(region, first, object, object->GetSize(), true));
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(1));
    GC_EXPECT_EQ(region->GetLiveByteCount(), uint64_t(object->GetSize()));
    RegionBitmap* bitmap = region->GetMarkBitmap(first);
    GC_EXPECT_TRUE(bitmap != nullptr);
    const uint64_t youngSequence = LiveMapCycleAccess::Cycle(
        Heap::GetHeap().GetCollector(), Generation::Young).Sequence();
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    auto next = region->GetMarkView<Generation::Old>();
    GC_EXPECT_EQ(next.GetEpoch(), first.GetEpoch() + 1);
    GC_EXPECT_TRUE(region->GetMarkBitmap(next) == nullptr);
    // No per-page ClearLiveInfo: the generation sequence invalidates both counts.
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(0));
    GC_EXPECT_EQ(region->GetLiveByteCount(), uint64_t(0));
    GC_EXPECT_EQ(LiveMapCycleAccess::Cycle(Heap::GetHeap().GetCollector(), Generation::Young).Sequence(), youngSequence);
    GC_EXPECT_TRUE(mark(region, next, object, object->GetSize(), true));
    GC_EXPECT_TRUE(region->GetMarkBitmap(next) == bitmap);
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(1));
    GC_EXPECT_EQ(region->GetLiveByteCount(), uint64_t(object->GetSize()));
    GC_EXPECT_EQ(bitmap->GetLiveObjects(), size_t(1));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), size_t(object->GetSize()));
}

// The real RegionInfo marking entry includes both map metadata initialization
// and segment initialization; all workers start with an uninitialized map.
GC_TEST(ZLiveMapPort, ConcurrentProductFirstMarkPublishesMetadata)
{
    GcHeapFixture fx;
    constexpr size_t workers = 8;
    auto mark = ProductMark();
    auto* region = fx.region0;
    auto view = region->GetMarkView<Generation::Old>();
    BaseObject* objects[workers];
    for (size_t i = 0; i < workers; ++i) {
        objects[i] = fx.PlaceObject(region->GetRegionStart() + (i + 1) * 256);
    }
    std::atomic<bool> go {false};
    std::atomic<size_t> firstMarks {0};
    std::thread threads[workers];
    for (size_t i = 0; i < workers; ++i) {
        threads[i] = std::thread([&, i]() {
            while (!go.load(std::memory_order_acquire)) {}
            if (mark(region, view, fx.obj0, fx.obj0->GetSize(), true)) {
                firstMarks.fetch_add(1, std::memory_order_relaxed);
            }
            if (mark(region, view, objects[i], objects[i]->GetSize(), true)) {
                firstMarks.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    RegionBitmap* bitmap = region->GetMarkBitmap(view);
    GC_EXPECT_TRUE(bitmap != nullptr);
    GC_EXPECT_EQ(firstMarks.load(), workers + 1);
    GC_EXPECT_EQ(bitmap->GetLiveObjects(), workers + 1);
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), (workers + 1) * fx.obj0->GetSize());
    GC_EXPECT_TRUE(bitmap->IsMarked(64));
    for (size_t i = 0; i < workers; ++i) {
        GC_EXPECT_TRUE(bitmap->IsMarked((i + 1) * 256));
    }
}

// ZPageAllocator::safe_destroy_page releases the map's storage. Reuse exceeds
// the fixture's unchanged 64-unit envelope, which the former bump arena exhausted.
GC_TEST(ZLiveMapPort, PageRetirementReclaimsMapStorage)
{
    GcHeapFixture fx;
    auto mark = ProductMark();
    for (size_t incarnation = 0; incarnation < 128; ++incarnation) {
        RegionInfo* region = fx.region0;
        region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
        BaseObject* object = fx.PlaceObject(region->GetRegionStart() + 64);
        region->SetRegionAllocPtr(region->GetRegionStart() + 64 + object->GetSize());
        auto view = region->GetMarkView<Generation::Old>();
        GC_EXPECT_TRUE(mark(region, view, object, object->GetSize(), true));
        GC_EXPECT_EQ(region->GetMarkBitmap(view)->GetLiveObjects(), size_t(1));
        RegionInfo::RetirePage(region, [region]() { region->InitFreeUnits(); });
        fx.region0 = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        GC_EXPECT_TRUE(fx.region0->GetLiveInfo() != nullptr);
        GC_EXPECT_TRUE(fx.region0->GetMarkBitmap(fx.region0->GetMarkView<Generation::Old>()) == nullptr);
    }
}

// ZPage::mark_object -> ZMarkCache::inc_live -> ZPage::inc_live: deferred
// accounting consumes the first-live claim, then readers use that same map.
GC_TEST(ZLiveMapPort, DeferredProductCountsUseOnlyLiveMap)
{
    GcHeapFixture fx;
    auto* region = fx.region0;
    auto* object = fx.obj0;
    auto view = region->GetMarkView<Generation::Old>();
    auto mark = ProductMark();
    GC_EXPECT_TRUE(mark(region, view, object, object->GetSize(), false));
    RegionBitmap* bitmap = region->GetMarkBitmap(view);
    GC_EXPECT_TRUE(bitmap != nullptr);
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(0));
    GC_EXPECT_EQ(region->GetLiveByteCount(), uint64_t(0));
    GC_EXPECT_EQ(bitmap->GetLiveObjects(), size_t(0));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), size_t(0));
    region->AddLiveCounts(1, object->GetSize());
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(1));
    GC_EXPECT_EQ(region->GetLiveByteCount(), uint64_t(object->GetSize()));
    GC_EXPECT_EQ(bitmap->GetLiveObjects(), size_t(1));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), size_t(object->GetSize()));
    GC_EXPECT_FALSE(mark(region, view, object, object->GetSize(), true));
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(1));
    GC_EXPECT_EQ(region->GetLiveByteCount(), uint64_t(object->GetSize()));
}

// ZBitMap::par_set_bit_pair_finalizable/strong and ZPage::inc_live: upgrading
// an object already accounted by finalizable marking cannot add a second entry.
GC_TEST(ZLiveMapPort, ProductFinalizableUpgradeKeepsSingleCount)
{
    GcHeapFixture fx;
    auto* region = fx.region0;
    auto* object = fx.obj0;
    auto view = region->GetMarkView<Generation::Old>();
    bool firstLive = false;
    GC_EXPECT_TRUE(region->ResurrectObjectWithLiveClaim(object,
        region->GetAddressOffset(reinterpret_cast<MAddress>(object)), false, firstLive));
    GC_EXPECT_TRUE(firstLive);
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(0));
    region->AddLiveCounts(1, object->GetSize());
    GC_EXPECT_TRUE(ProductMark()(region, view, object, object->GetSize(), true));
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(1));
    GC_EXPECT_EQ(region->GetLiveByteCount(), uint64_t(object->GetSize()));
    GC_EXPECT_EQ(region->GetMarkBitmap(view)->GetLiveObjects(), size_t(1));
}

GC_TEST(ZLiveMapPort, YoungSequenceInvalidatesCountsWithoutPageClear)
{
    GcHeapFixture fx;
    auto* region = fx.region0;
    region->SetYoungRegionFlag(1);
    auto first = region->GetMarkView<Generation::Young>();
    GC_EXPECT_TRUE(region->MarkObject(first, fx.obj0, fx.obj0->GetSize(), true));
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(1));
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    auto next = region->GetMarkView<Generation::Young>();
    GC_EXPECT_EQ(region->GetLiveByteCount(), uint64_t(0));
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(0));
    GC_EXPECT_TRUE(region->MarkObject(next, fx.obj0, fx.obj0->GetSize(), true));
    GC_EXPECT_EQ(region->GetLiveByteCount(), uint64_t(fx.obj0->GetSize()));
    GC_EXPECT_EQ(region->GetLiveObjectCount(), uint32_t(1));
}

// ZBitMap::par_set_bit_pair_strong/finalizable: claim result and live claim
// are independent when finalizable is upgraded to strong. Calls link to the
// product zLiveMap.cpp; this TU does not include zLiveMap.inline.hpp.
GC_TEST(P1BitMap, StrongClaimResult)
{
    constexpr size_t pageSize = 4096;
    auto* bitmap = GcHeapFixture::AllocPlantedBitmap(pageSize);
    bool firstLive = false;
    bool duplicateLive = true;
    const bool first = bitmap->MarkBits(64, 32, pageSize, firstLive);
    const bool duplicate = bitmap->MarkBits(64, 32, pageSize, duplicateLive);
    const bool marked = bitmap->IsMarked(64);
    std::fprintf(stderr, "P1_BITMAP_STRONG_RESULT first=%d duplicate=%d first_live=%d duplicate_live=%d marked=%d\n",
                 first, duplicate, firstLive, duplicateLive, marked);
    GcHeapFixture::FreePlantedBitmap(bitmap);
    GC_EXPECT_TRUE(first && !duplicate);
    GC_EXPECT_TRUE(firstLive && !duplicateLive && marked);
}

GC_TEST(P1BitMap, FinalizableClaimResult)
{
    constexpr size_t pageSize = 4096;
    auto* bitmap = GcHeapFixture::AllocPlantedBitmap(pageSize);
    bool firstLive = false;
    bool duplicateLive = true;
    const bool first = bitmap->MarkFinalizableBits(64, 32, pageSize, firstLive);
    const bool duplicate = bitmap->MarkFinalizableBits(64, 32, pageSize, duplicateLive);
    const bool finalizable = bitmap->IsFinalizable(64);
    std::fprintf(stderr, "P1_BITMAP_FINALIZABLE_RESULT first=%d duplicate=%d first_live=%d duplicate_live=%d finalizable=%d\n",
                 first, duplicate, firstLive, duplicateLive, finalizable);
    GcHeapFixture::FreePlantedBitmap(bitmap);
    GC_EXPECT_TRUE(first && !duplicate);
    GC_EXPECT_TRUE(firstLive && !duplicateLive && finalizable);
}

GC_TEST(P1BitMap, ClaimContentsAndUpgradeControl)
{
    constexpr size_t pageSize = 4096;
    auto* bitmap = GcHeapFixture::AllocPlantedBitmap(pageSize);
    bool firstLive = false;
    (void)bitmap->MarkFinalizableBits(64, 32, pageSize, firstLive);
    if (firstLive) bitmap->AddLiveCounts(1, 32);
    const bool finalizable = bitmap->IsFinalizable(64);
    bool upgradeLive = true;
    (void)bitmap->MarkBits(64, 32, pageSize, upgradeLive);
    if (upgradeLive) bitmap->AddLiveCounts(1, 32);
    const bool marked = bitmap->IsMarked(64);
    const size_t bytes = bitmap->GetLiveBytes();
    const size_t objects = bitmap->GetLiveObjects();
    std::fprintf(stderr, "P1_BITMAP_CONTENTS finalizable=%d marked=%d first_live=%d upgrade_live=%d bytes=%zu objects=%zu\n",
                 finalizable, marked, firstLive, upgradeLive, bytes, objects);
    GcHeapFixture::FreePlantedBitmap(bitmap);
    GC_EXPECT_TRUE(finalizable && marked && firstLive && !upgradeLive);
    GC_EXPECT_EQ(bytes, size_t(32));
    GC_EXPECT_EQ(objects, size_t(1));
}
