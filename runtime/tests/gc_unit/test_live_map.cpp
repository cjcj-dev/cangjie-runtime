// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U4 — product LiveInfo / RegionBitmap + liveInfo0 snapshot + BindLiveInfo0FromLiveIfNull.
// Product symbols: RegionBitmap::MarkBits / IsMarked, LiveInfo::IsSurvivedObject,
// RegionInfo::BindLiveInfo0FromLiveIfNull, PrepareForwardable-style ghost pointer share.

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <dlfcn.h>
#include <limits>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/WCollector/WCollector.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// Do not materialize product inline/template bodies in this test executable.
// Product-path calls must resolve from libcangjie-runtime.so, so the test ELF
// cannot self-satisfy the product symbols with weak definitions.
namespace MapleRuntime {
extern template bool RegionInfo::MarkObject<Generation::Young>(
    MarkView<Generation::Young>, const BaseObject*, size_t, bool);
extern template bool RegionInfo::MarkObject<Generation::Old>(
    MarkView<Generation::Old>, const BaseObject*, size_t, bool);
extern template bool RegionInfo::MarkObject<Generation::Young>(
    MarkView<Generation::Young>, const BaseObject*);
extern template bool RegionInfo::MarkObject<Generation::Old>(
    MarkView<Generation::Old>, const BaseObject*);
// The product emits these explicit instantiations only in its GC-unit build.
// Other configurations exercise the same product header template directly.
#if defined(MRT_GC_UNIT_TESTS)
extern template void RegionInfo::ClearLiveInfo<Generation::Young>(MarkView<Generation::Young>);
extern template void RegionInfo::ClearLiveInfo<Generation::Old>(MarkView<Generation::Old>);
#endif
} // namespace MapleRuntime

namespace {

void* ProductRuntimeHandle()
{
    static void* handle = []() {
        void* h = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
        if (h == nullptr) {
            h = dlopen("libcangjie-runtime.so", RTLD_NOW);
        }
        return h;
    }();
    return handle;
}

template<Generation G>
using ProductMarkObject = bool (*)(RegionInfo*, MarkView<G>, const BaseObject*, size_t, bool);

template<Generation G>
ProductMarkObject<G> ProductMarkObjectFn();

template<>
ProductMarkObject<Generation::Young> ProductMarkObjectFn<Generation::Young>()
{
    static auto fn = reinterpret_cast<ProductMarkObject<Generation::Young>>(dlsym(
        ProductRuntimeHandle(),
        "_ZN12MapleRuntime10RegionInfo10MarkObjectILNS_10GenerationE0EEEbNS_8MarkViewIXT_EEEPKNS_10BaseObjectEmb"));
    return fn;
}

template<>
ProductMarkObject<Generation::Old> ProductMarkObjectFn<Generation::Old>()
{
    static auto fn = reinterpret_cast<ProductMarkObject<Generation::Old>>(dlsym(
        ProductRuntimeHandle(),
        "_ZN12MapleRuntime10RegionInfo10MarkObjectILNS_10GenerationE1EEEbNS_8MarkViewIXT_EEEPKNS_10BaseObjectEmb"));
    return fn;
}

using ProductClearLiveInfo = void (*)(RegionInfo*, MarkView<Generation::Young>);

ProductClearLiveInfo ProductClearLiveInfoFn()
{
    static auto fn = reinterpret_cast<ProductClearLiveInfo>(dlsym(
        ProductRuntimeHandle(),
        "_ZN12MapleRuntime10RegionInfo13ClearLiveInfoILNS_10GenerationE0EEEvNS_8MarkViewIXT_EEE"));
    return fn;
}

} // namespace

// Port of test_zLiveMap.cpp's one-object large-page invariant onto the
// Cangjie RegionBitmap representation.  The first mark makes the only object
// live and accounts its bytes exactly once; repeating it is idempotent.
GC_TEST(ZLiveMapPort, OneObjectPageMarkAccountsLiveOnce)
{
    constexpr size_t kPageSize = 4096;
    RegionBitmap* bitmap = GcHeapFixture::AllocPlantedBitmap(kPageSize);

    GC_EXPECT_FALSE(bitmap->IsMarked(0));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), static_cast<size_t>(0));
    GC_EXPECT_FALSE(bitmap->MarkBits(0, kPageSize, kPageSize));
    GC_EXPECT_TRUE(bitmap->IsMarked(0));
    GC_EXPECT_FALSE(bitmap->IsMarked(kPageSize - kMarkedBytesPerBit));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), kPageSize);
    GC_EXPECT_EQ(bitmap->RecomputeLiveBytes(), kPageSize);

    GC_EXPECT_TRUE(bitmap->MarkBits(0, kPageSize, kPageSize));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), kPageSize);
    GcHeapFixture::FreePlantedBitmap(bitmap);
}

// U4: product mark then IsSurvivedObject.
GC_TEST(LiveMap, MarkAndSurvive)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    size_t regionSize = region->GetRegionSize();
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);

    size_t off0 = 0;
    size_t off64 = 64;
    MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, off0));
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, off64));

    bool was = bm->MarkBits(off64, 8, regionSize);
    GC_EXPECT_FALSE(was);
    GC_EXPECT_TRUE(live->IsSurvivedObject(view, off64));
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, off0));
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, 128));

    GC_EXPECT_TRUE(bm->MarkBits(off64, 8, regionSize));
    GC_EXPECT_TRUE(live->IsSurvivedObject(view, off64));
    GC_EXPECT_TRUE(bm->IsMarked(off64));

    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// ZGC zRelocationSet.cpp:79-134 allocates the selected forwarding set before
// publication. This is focused fixture setup, not a complete GC phase test.
namespace {
// Headers expose inline definitions used by older focused tests in this TU.
// Resolve these source-page operations from the loaded product, so this set
// cannot silently execute the ELF's local copies when the SO is changed.
bool ProductSourceSurvives(RegionInfo* region, size_t offset)
{
    using Fn = bool (*)(RegionInfo*, size_t);
    static auto fn = reinterpret_cast<Fn>(dlsym(ProductRuntimeHandle(),
        "_ZN12MapleRuntime10RegionInfo21IsRouteSurvivedObjectEm"));
    GC_EXPECT_TRUE(fn != nullptr);
    return fn(region, offset);
}

void ProductBindSource(RegionInfo* region)
{
    using Fn = void (*)(RegionInfo*);
    static auto fn = reinterpret_cast<Fn>(dlsym(ProductRuntimeHandle(),
        "_ZN12MapleRuntime10RegionInfo27BindLiveInfo0FromLiveIfNullEv"));
    GC_EXPECT_TRUE(fn != nullptr);
    fn(region);
}

void ProductDispelSource(RegionInfo* region)
{
    using Fn = void (*)(RegionInfo*);
    static auto fn = reinterpret_cast<Fn>(dlsym(ProductRuntimeHandle(),
        "_ZN12MapleRuntime10RegionInfo21DispelGhostFromRegionEv"));
    GC_EXPECT_TRUE(fn != nullptr);
    fn(region);
}

template<Generation G>
bool ProductMarkSource(RegionInfo* region, BaseObject* object)
{
    const auto mark = ProductMarkObjectFn<G>();
    GC_EXPECT_TRUE(mark != nullptr);
    return mark(region, region->GetMarkView<G>(), object, object->GetSize(), true);
}

template<Generation G>
void PublishLiveMapSource(RegionInfo* region)
{
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    RegionList selected("livemap-source");
    selected.PrependRegion(region, region->GetRegionType());
    GC_EXPECT_TRUE(ForwardingTable::BeginForwardingArena(G, selected));
    while (selected.TakeHeadRegion() != nullptr) {}
    region->PublishForwardingCarrier(region->GetMarkView<G>());
}

void ExpectSourceObject(RegionInfo* region, BaseObject* object, const char* stage)
{
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(object));
    const bool survived = ProductSourceSurvives(region, offset);
    std::fprintf(stderr, "LIVEMAP_SOURCE stage=%s survived=%d\n", stage, survived);
    GC_EXPECT_TRUE(survived);
    GC_EXPECT_FALSE(ProductSourceSurvives(region, offset + 16));
}
} // namespace

// ZGC zForwarding.cpp:65-77: source iteration finishes before its map is reset.
// Clearing the reusable descriptor's pointer does not clear the source map.
GC_TEST(LiveMap, LiveInfo0SnapshotSurvivesClear)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    GC_EXPECT_FALSE(ProductMarkSource<Generation::Old>(region, fx.obj0));
    LiveInfo* source = region->GetLiveInfo();
    PublishLiveMapSource<Generation::Old>(region);
    region->metadata.liveInfo = nullptr;
    // Restore the descriptor even when an assertion throws; arena owns source.
    struct Restore {
        RegionInfo* region;
        LiveInfo* source;
        ~Restore() { region->metadata.liveInfo = source; }
    } restore { region, source };
    ExpectSourceObject(region, fx.obj0, "current-pointer-cleared");
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == source);
}

// The local late-bind adapter supplies the same source-page map required by
// ZGC zForwarding.cpp:65-77. Install the selected owner before publishing an
// empty source; then mark through the product and bind its resulting map.
GC_TEST(LiveMap, BindLiveInfo0AfterLateMark)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    LiveInfoArena::GetLiveInfoArena().RecyclePageLiveInfo(region);
    region->metadata.liveInfo = nullptr;
    PublishLiveMapSource<Generation::Old>(region);
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == nullptr);
    region->InitializeLiveInfo();
    GC_EXPECT_FALSE(ProductMarkSource<Generation::Old>(region, fx.obj0));
    ProductBindSource(region);
    ExpectSourceObject(region, fx.obj0, "late-product-mark-bound");
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == region->GetLiveInfo());
}

// ZGC zForwarding.cpp:86-108 retains the page until release; :65-77 resets
// the non-promoted map only after source iteration. No retained bitmap copy.
GC_TEST(LiveMap, OldForwardingCarrierPublishesOwnerAndRetires)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    GC_EXPECT_FALSE(ProductMarkSource<Generation::Old>(region, fx.obj0));
    PublishLiveMapSource<Generation::Old>(region);
    GC_EXPECT_TRUE(region->HasFromPageMetadata());
    GC_EXPECT_EQ(region->GetRouteMarkGeneration(), Generation::Old);
    {
        RegionInfo::RetainScope retained(region);
        GC_EXPECT_TRUE(retained.ok());
        ExpectSourceObject(region, fx.obj0, "old-retained");
    }
    ProductDispelSource(region);
    {
        RegionInfo::RetainScope released(region);
        GC_EXPECT_FALSE(released.ok());
    }
    // ZForwarding::detach_page releases page access, not the forwarding set.
    ForwardingTable::ResetRelocationSet(Generation::Old);
    GC_EXPECT_FALSE(region->HasFromPageMetadata());
    // This fixture did not relocate the page. Dispel retires forwarding
    // access, whereas ForwardRegion resets the map after actual relocation
    // (ZGC zForwarding.cpp:65-77; local zRelocate.cpp:2424). Preserve the
    // original current-page fallback assertion: it is not a retained copy.
    GC_EXPECT_TRUE(ProductSourceSurvives(region, 64));
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == nullptr);
}

// ZGC zPage.cpp:64-72 clones only layout/top; the original young page owns
// its map, while the reusable descriptor becomes old with a fresh map.
GC_TEST(LiveMap, FromPageOwnerAndLivemapStayIdenticalAcrossPromotion)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    GC_EXPECT_FALSE(ProductMarkSource<Generation::Young>(region, fx.obj0));
    LiveInfo* source = region->GetLiveInfo();
    PublishLiveMapSource<Generation::Young>(region);
    auto original = region->CloneForPromotion(region->GetMarkView<Generation::Young>());
    GC_EXPECT_FALSE(region->IsYoungRegion());
    GC_EXPECT_TRUE(region->GetLiveInfo() != source);
    GC_EXPECT_EQ(region->GetRouteMarkGeneration(), Generation::Young);
    ExpectSourceObject(region, fx.obj0, "promoted-source-owner");
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == source);
    size_t visits = 0;
    original->ObjectIterate([&](BaseObject* object) {
        GC_EXPECT_TRUE(object == fx.obj0);
        ++visits;
    });
    GC_EXPECT_EQ(visits, 1u);
    // Forwarding must stop referring to the original before its owner dies.
    ForwardingTable::ResetRelocationSet(Generation::Young);
}

// zPage.cpp:64-72 + zForwarding.cpp:65-108, for both ordinary and one-object
// pages. Source state is consumed while retained, then becomes inaccessible.
GC_TEST(LiveMap, PromotionCarrierLivesUntilForwardingRelease)
{
    for (bool large : { false, true }) {
        GcHeapFixture fx;
        RegionInfo* region = fx.region0;
        region->SetYoungRegionFlag(1);
        if (large) region->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        BaseObject* object = large ? fx.PlaceObject(region->GetRegionStart()) : fx.obj0;
        GC_EXPECT_FALSE(ProductMarkSource<Generation::Young>(region, object));
        LiveInfo* source = region->GetLiveInfo();
        PublishLiveMapSource<Generation::Young>(region);
        auto original = region->CloneForPromotion(region->GetMarkView<Generation::Young>());
        region->ClearLiveInfo(region->GetMarkView<Generation::Old>());
        {
            RegionInfo::RetainScope retained(region);
            GC_EXPECT_TRUE(retained.ok());
            const bool survived = ProductSourceSurvives(region, large ? 0 : 64);
            std::fprintf(stderr, "LIVEMAP_SOURCE stage=promotion-retained large=%d survived=%d\n",
                         large, survived);
            GC_EXPECT_TRUE(survived);
            GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == source);
            GC_EXPECT_TRUE(region->GetCurrentLiveMap() == nullptr);
        }
        ProductDispelSource(region);
        {
            RegionInfo::RetainScope released(region);
            GC_EXPECT_FALSE(released.ok());
        }
        // The relocation set owns forwarding storage beyond page detach.
        ForwardingTable::ResetRelocationSet(Generation::Young);
        GC_EXPECT_FALSE(region->HasFromPageMetadata());
        GC_EXPECT_FALSE(ProductSourceSurvives(region, large ? 0 : 64));
        size_t visits = 0;
        original->ObjectIterate([&](BaseObject* visited) {
            GC_EXPECT_TRUE(visited == object);
            ++visits;
        });
        GC_EXPECT_EQ(visits, 1u);
        ForwardingTable::ResetRelocationSet(Generation::Young);
    }
}

// U4: null markBitmap ⇒ never survived (domain reject).
GC_TEST(LiveMap, NullBitmapNeverSurvived)
{
    GcHeapFixture fx;
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    MarkView<Generation::Old> view = fx.region0->GetMarkView<Generation::Old>();
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, 0));
    GC_EXPECT_FALSE(live->IsSurvivedObject(view, 100));
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// Typed views do not select independent storage. A current page has one
// livemap and its owner metadata decides which closure may paint it.
GC_TEST(LiveMap, CurrentPageHasSingleBitmap)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();
    MarkView<Generation::Old> old = region->GetMarkView<Generation::Old>();

    (void)bitmap->MarkBits(64, 8, region->GetRegionSize());
    GC_EXPECT_TRUE(region->IsMarkedObject(young, 64));
    GC_EXPECT_TRUE(region->IsMarkedObject(old, 64));

    GcHeapFixture::AdvanceGeneration(Generation::Young);
    region->ClearLiveInfo(region->GetMarkView<Generation::Young>());
    MarkView<Generation::Young> nextYoung = region->GetMarkView<Generation::Young>();
    GC_EXPECT_TRUE(region->GetMarkBitmap(nextYoung) == nullptr);
    GC_EXPECT_FALSE(region->IsMarkedObject(old, 64));
    fx.FreePlanted(live);
}

// pageown / ZGC zPage.inline.hpp:284-294: the target page, not the
// collector closure, chooses the physical mark authority.
GC_TEST(LiveMap, OwnerDispatchMarksYoungFace)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    LiveInfo* live = fx.PlantLiveInfo(region);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    (void)fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));

    GC_EXPECT_FALSE(region->MarkObjectByOwner(fx.obj0, fx.obj0->GetSize()));
    GC_EXPECT_TRUE(region->IsMarkedObject(region->GetMarkView<Generation::Young>(), offset));
    GC_EXPECT_TRUE(region->IsMarkedObject(region->GetMarkView<Generation::Old>(), offset));
    GC_EXPECT_TRUE(region->MarkFaceMatchesOwner<Generation::Young>());
    GC_EXPECT_FALSE(region->MarkFaceMatchesOwner<Generation::Old>());

    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// Large pages follow the same single-livemap rule, represented by one bit.
GC_TEST(LiveMap, CurrentLargePageHasSingleMarkBit)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    region->SetRegionType(RegionInfo::RegionType::RECENT_LARGE_REGION);
    region->SetYoungRegionFlag(1);
    MarkView<Generation::Young> young = region->GetMarkView<Generation::Young>();
    MarkView<Generation::Old> old = region->GetMarkView<Generation::Old>();

    region->SetMarkedRegionFlag(young, 1);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(young), 1u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 1u);

    GcHeapFixture::AdvanceGeneration(Generation::Young);
    region->ClearLiveInfo(region->GetMarkView<Generation::Young>());
    MarkView<Generation::Young> nextYoung = region->GetMarkView<Generation::Young>();
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(nextYoung), 0u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 0u);

    region->SetMarkedRegionFlag(nextYoung, 1);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(nextYoung), 1u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(young), 0u);
    GC_EXPECT_EQ(region->GetMarkedRegionFlag(old), 0u);
}

// The large-page first paint is a single publication/accounting RMW.  Two
// concurrent callers therefore have exactly one false (new-mark) result and
// the live byte book contains the object size once.
GC_TEST(LiveMap, LargeFirstPaintHasSingleWinner)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    region->SetRegionType(RegionInfo::RegionType::LARGE_REGION);
    BaseObject* holder = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(region->GetRegionStart() + holder->GetSize());
    MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();

    std::atomic<bool> go { false };
    std::atomic<int> first { 0 };
    std::thread t0([&]() {
        while (!go.load(std::memory_order_acquire)) {
        }
        if (!ProductMarkObjectFn<Generation::Old>()(region, view, holder, holder->GetSize(), true)) {
            first.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread t1([&]() {
        while (!go.load(std::memory_order_acquire)) {
        }
        if (!ProductMarkObjectFn<Generation::Old>()(region, view, holder, holder->GetSize(), true)) {
            first.fetch_add(1, std::memory_order_relaxed);
        }
    });
    go.store(true, std::memory_order_release);
    t0.join();
    t1.join();

    GC_EXPECT_EQ(first.load(std::memory_order_relaxed), 1);
    GC_EXPECT_EQ(region->GetLiveByteCount(), static_cast<uint64_t>(holder->GetSize()));
    GC_EXPECT_TRUE(region->IsCurrentFacePublished());
}

// ZLiveMapTest::SetUp and ZGeneration::mark_start: pages share their owner's
// sequence; clearing one page cannot advance the generation or invalidate another.
GC_TEST(LiveMap, GenerationSequenceSharedByPages)
{
    GcHeapFixture fx;
    const uint64_t oldSequence = fx.region0->GetSnapshotEpoch();
    GC_EXPECT_EQ(fx.region1->GetSnapshotEpoch(), oldSequence);
    fx.region0->ClearLiveInfo(fx.region0->GetMarkView<Generation::Old>());
    GC_EXPECT_EQ(fx.region0->GetSnapshotEpoch(), oldSequence);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    GC_EXPECT_EQ(fx.region0->GetSnapshotEpoch(), oldSequence + 1);
    GC_EXPECT_EQ(fx.region1->GetSnapshotEpoch(), oldSequence + 1);
}

// markwater: ClearLiveInfo snapshots allocPtr. Objects at offset ≥ water
// are allocate-black (ZGC zPage is_allocating). A stale view must not
// inherit that verdict (oracleblack2 b-face).
GC_TEST(LiveMap, MarkStartAllocWaterIsImplicitLive)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    MAddress start = region->GetRegionStart();
    region->SetRegionAllocPtr(start + 128);
    GC_EXPECT_EQ(region->GetMarkStartAllocPtr(), 0u);

    MarkView<Generation::Old> stale = region->GetMarkView<Generation::Old>();
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    region->ClearLiveInfo(region->GetMarkView<Generation::Old>());
    GC_EXPECT_EQ(region->GetMarkStartAllocPtr(), start + 128);

    region->SetRegionAllocPtr(start + 256);
    MarkView<Generation::Old> current = region->GetMarkView<Generation::Old>();
    GC_EXPECT_TRUE(region->HasMarkStartAllocGap());
    GC_EXPECT_FALSE(region->IsKnownEmpty(current));
    GC_EXPECT_FALSE(region->AllocatedAfterMarkStart(64));
    GC_EXPECT_TRUE(region->AllocatedAfterMarkStart(128));
    GC_EXPECT_TRUE(region->AllocatedAfterMarkStart(192));
    GC_EXPECT_FALSE(region->IsMarkedObject(current, static_cast<size_t>(64)));
    GC_EXPECT_TRUE(region->IsMarkedObject(current, static_cast<size_t>(128)));
    GC_EXPECT_TRUE(region->IsSurvivedObject(current, static_cast<size_t>(192)));
    GC_EXPECT_TRUE(region->IsRouteSurvivedObject(128));
    // Stale view (previous generation sequence) must not treat post-water as marked.
    GC_EXPECT_FALSE(region->IsMarkedObject(stale, static_cast<size_t>(128)));
    GC_EXPECT_FALSE(region->IsSurvivedObject(stale, static_cast<size_t>(192)));
}

// Main-line ZLiveMap paired-bit and product-consumer coverage retained by content synthesis.
GC_TEST(ZLiveMapPort, FinalizableAndStrongShareOnePair)
{
    constexpr size_t kPageSize = 4096;
    RegionBitmap* bitmap = GcHeapFixture::AllocPlantedBitmap(kPageSize);
    bool incLive = false;

    GC_EXPECT_FALSE(bitmap->MarkFinalizableBits(64, 16, kPageSize, incLive));
    GC_EXPECT_TRUE(incLive);
    bitmap->AddLiveCounts(1, 16);
    GC_EXPECT_TRUE(bitmap->IsLive(64));
    GC_EXPECT_TRUE(bitmap->IsFinalizable(64));
    GC_EXPECT_FALSE(bitmap->IsMarked(64));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), static_cast<size_t>(16));

    GC_EXPECT_FALSE(bitmap->MarkBits(64, 16, kPageSize, incLive));
    GC_EXPECT_FALSE(incLive);
    GC_EXPECT_TRUE(bitmap->IsLive(64));
    GC_EXPECT_FALSE(bitmap->IsFinalizable(64));
    GC_EXPECT_TRUE(bitmap->IsMarked(64));
    GC_EXPECT_EQ(bitmap->GetLiveBytes(), static_cast<size_t>(16));

    GcHeapFixture::FreePlantedBitmap(bitmap);
}

// SATB producers may publish the same object more than once. The strong half
// of the pair is the consumer-side receipt: exactly one 0->1 owns live-byte
// accounting, matching ZLiveMap::set/par_set_bit_pair.

GC_TEST(ZLiveMapPort, DuplicatePublicationConvergesAtStrongMark)
{
    GcHeapFixture fx;
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    RegionBitmap* bitmap = fx.PlantMarkBitmap(live, fx.region0->GetRegionSize());
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));

    GC_EXPECT_TRUE(RegionSpace::ShouldEnqueue<Generation::Old>(fx.obj0));
    GC_EXPECT_TRUE(RegionSpace::ShouldEnqueue<Generation::Old>(fx.obj0));

    bool firstIncLive = false;
    bool secondIncLive = false;
    const bool firstAlready = bitmap->MarkBits(offset, 8, fx.region0->GetRegionSize(), firstIncLive);
    const bool secondAlready = bitmap->MarkBits(offset, 8, fx.region0->GetRegionSize(), secondIncLive);
    const size_t liveBytes = bitmap->GetLiveBytes();
    const bool receiptOnce = !firstAlready && secondAlready;
    const bool incLiveOnce = firstIncLive && !secondIncLive;
    const bool bytesOnce = liveBytes == static_cast<size_t>(8);
    std::fprintf(stderr,
                 "DETAIL duplicate_consumer receipt_once=%d inc_live_once=%d bytes_once=%d live_bytes=%zu\n",
                 receiptOnce, incLiveOnce, bytesOnce, liveBytes);
    GC_EXPECT_TRUE(receiptOnce && incLiveOnce && bytesOnce);
    GC_EXPECT_TRUE(bitmap->IsMarked(offset));
    GC_EXPECT_EQ(liveBytes, static_cast<size_t>(8));
    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Old>(fx.obj0));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// The sequential duplicate-publication test above proves idempotence, but it
// cannot distinguish one atomic RMW from a load followed by a store.  Start two
// workers at the same product MarkObject call boundary on every round.  The
// window is real but is not forced inside MarkBits: a faulty non-atomic product
// implementation therefore has many opportunities to expose two 0->1 strong
// receipts, while the atomic product must have exactly one winner per round.

GC_TEST(ZLiveMapPort, ConcurrentDuplicateSatbPublicationHasOneStrongReceipt)
{
    constexpr size_t kRounds = 2000;
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    BaseObject* holder = fx.obj0;
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap(live, region->GetRegionSize());
    MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
    const auto markObject = ProductMarkObjectFn<Generation::Old>();
    GC_EXPECT_TRUE(markObject != nullptr);

    std::atomic<size_t> phase{ 0 };
    std::atomic<size_t> arrived{ 0 };
    std::atomic<size_t> completed{ 0 };
    std::atomic<bool> already[2];
    std::atomic<bool> incLive[2];

    auto publish = [&](size_t worker) {
        for (size_t round = 1; round <= kRounds; ++round) {
            while (phase.load(std::memory_order_acquire) < round) {
                std::this_thread::yield();
            }
            arrived.fetch_add(1, std::memory_order_acq_rel);
            while (arrived.load(std::memory_order_acquire) < 2 * round) {
                std::this_thread::yield();
            }
            // This is the product small-region entry.  The test deliberately
            // does not call RegionBitmap::MarkBits directly: the product
            // RegionInfo::MarkObject implementation owns the pair RMW and its
            // live-byte receipt.
            const bool localAlready = markObject(region, view, holder, holder->GetSize(), true);
            const bool localIncLive = !localAlready;
            already[worker].store(localAlready, std::memory_order_relaxed);
            incLive[worker].store(localIncLive, std::memory_order_relaxed);
            completed.fetch_add(1, std::memory_order_release);
        }
    };

    std::thread first(publish, 0);
    std::thread second(publish, 1);
    JoinGuard firstGuard(first);
    JoinGuard secondGuard(second);
    size_t duplicateReceiptRounds = 0;
    size_t duplicateIncLiveRounds = 0;
    size_t doubleLiveBytesRounds = 0;
    for (size_t round = 1; round <= kRounds; ++round) {
        bitmap->Reset();

        phase.store(round, std::memory_order_release);
        while (completed.load(std::memory_order_acquire) < 2 * round) {
            std::this_thread::yield();
        }

        const size_t receiptClaims = static_cast<size_t>(!already[0].load(std::memory_order_relaxed)) +
            static_cast<size_t>(!already[1].load(std::memory_order_relaxed));
        const size_t incLiveClaims = static_cast<size_t>(incLive[0].load(std::memory_order_relaxed)) +
            static_cast<size_t>(incLive[1].load(std::memory_order_relaxed));
        duplicateReceiptRounds += receiptClaims != 1 ? 1 : 0;
        duplicateIncLiveRounds += incLiveClaims != 1 ? 1 : 0;
        doubleLiveBytesRounds += bitmap->GetLiveBytes() != holder->GetSize() ? 1 : 0;
    }
    first.join();
    second.join();

    std::fprintf(stderr,
                 "DETAIL concurrent_duplicate rounds=%zu duplicate_receipt=%zu duplicate_inc_live=%zu "
                 "double_live_bytes=%zu\n",
                 kRounds, duplicateReceiptRounds, duplicateIncLiveRounds, doubleLiveBytesRounds);
    GC_EXPECT_EQ(duplicateReceiptRounds, static_cast<size_t>(0));
    GC_EXPECT_EQ(duplicateIncLiveRounds, static_cast<size_t>(0));
    GC_EXPECT_EQ(doubleLiveBytesRounds, static_cast<size_t>(0));
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// Product collector consumer of RegionInfo::MarkObject (Mark.cpp:99-111).

GC_TEST(ZLiveMapPort, CollectorMarkObjectConsumesProductPair)
{
    GcHeapFixture fx;
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap(live, fx.region0->GetRegionSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    GC_EXPECT_FALSE(collector.MarkObject(fx.obj0));
    MarkView<Generation::Old> view = fx.region0->GetMarkView<Generation::Old>();
    GC_EXPECT_TRUE(fx.region0->IsMarkedObject(view, fx.obj0));
    GC_EXPECT_TRUE(collector.MarkObject(fx.obj0));
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// U4: product mark then IsSurvivedObject.

// ZPage::clone_for_promotion / ZPage::object_iterate; the large-page case
// follows ZLiveMapTest.strongly_live_for_large_zpage. The old page descriptor
// gets a fresh map while the original young page keeps the same owned map.
GC_TEST(ZLiveMapPort, CloneForPromotionKeepsOriginalPageLivemap)
{
    for (bool large : { false, true }) {
        GcHeapFixture fx;
        RegionInfo* region = fx.region0;
        region->SetYoungRegionFlag(1);
        region->SetYoungAge(1);
        if (large) {
            region->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        }
        BaseObject* object = large ? fx.PlaceObject(region->GetRegionStart()) : fx.obj0;
        LiveInfo* originalLive = region->GetLiveInfo();
        GC_EXPECT_TRUE(originalLive != nullptr);
        const auto mark = ProductMarkObjectFn<Generation::Young>();
        GC_EXPECT_TRUE(mark != nullptr);
        GC_EXPECT_FALSE(mark(region, region->GetMarkView<Generation::Young>(),
                             object, object->GetSize(), true));
        auto originalPage = region->CloneForPromotion(region->GetMarkView<Generation::Young>());
        GC_EXPECT_FALSE(region->IsYoungRegion());
        GC_EXPECT_EQ(originalPage->Age(), 1u);
        GC_EXPECT_TRUE(region->GetLiveInfo() != originalLive);
        GC_EXPECT_TRUE(region->GetCurrentLiveMap() == nullptr);
        std::vector<BaseObject*> visited;
        originalPage->ObjectIterate([&](BaseObject* obj) { visited.push_back(obj); });
        GC_EXPECT_EQ(visited.size(), 1u);
        GC_EXPECT_TRUE(visited[0] == object);
        // Retiring the reusable descriptor cannot release the original page.
        LiveInfoArena::GetLiveInfoArena().RecyclePageLiveInfo(region);
        region->metadata.liveInfo = nullptr;
        visited.clear();
        originalPage->ObjectIterate([&](BaseObject* obj) { visited.push_back(obj); });
        GC_EXPECT_EQ(visited.size(), 1u);
        GC_EXPECT_TRUE(visited[0] == object);
    }
}

// ZLiveMap::iterate checks is_marked(original_generation) before consuming
// segments, including when an allocated bitmap still contains previous bits.
GC_TEST(ZLiveMapPort, PromotionIteratorRequiresCurrentYoungSequence)
{
    for (bool large : { false, true }) {
        for (bool advanceBeforeClone : { false, true }) {
            GcHeapFixture fx;
            RegionInfo* region = fx.region0;
            region->SetYoungRegionFlag(1);
            if (large) {
                region->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
            }
            BaseObject* object = large ? fx.PlaceObject(region->GetRegionStart()) : fx.obj0;
            const auto mark = ProductMarkObjectFn<Generation::Young>();
            GC_EXPECT_TRUE(mark != nullptr);
            GC_EXPECT_FALSE(mark(region, region->GetMarkView<Generation::Young>(),
                                 object, object->GetSize(), true));
            LiveInfo* originalLive = region->GetLiveInfo();
            RegionBitmap* bitmap = originalLive->GetMarkFace().bitmap;
            const size_t offset = reinterpret_cast<MAddress>(object) - region->GetRegionStart();
            GC_EXPECT_TRUE(bitmap->IsObjectStart(offset));
            if (advanceBeforeClone) {
                GcHeapFixture::AdvanceGeneration(Generation::Young);
            }
            auto originalPage = region->CloneForPromotion(region->GetMarkView<Generation::Young>());
            size_t visits = 0;
            const auto visit = [&](BaseObject* obj) {
                GC_EXPECT_TRUE(obj == object);
                ++visits;
            };
            originalPage->ObjectIterate(visit);
            GC_EXPECT_EQ(visits, advanceBeforeClone ? 0u : 1u);
            if (!advanceBeforeClone) {
                // The old owner of the reused slot is irrelevant to this page.
                GcHeapFixture::AdvanceGeneration(Generation::Old);
                visits = 0;
                originalPage->ObjectIterate(visit);
                GC_EXPECT_EQ(visits, 1u);
                GcHeapFixture::AdvanceGeneration(Generation::Young);
            }
            // Old start bits really remain; sequence mismatch must suppress them.
            GC_EXPECT_TRUE(bitmap->IsObjectStart(offset));
            visits = 0;
            originalPage->ObjectIterate(visit);
            GC_EXPECT_EQ(visits, 0u);
        }
    }
}

// ZPage::object_iterate (zPage.inline.hpp:320-331) consumes start bits;
// unmarked storage before and between objects is not an object header.
GC_TEST(ZLiveMapPort, LiveIteratorVisitsOnlyObjectStarts)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    BaseObject* second = fx.PlaceObject(region->GetRegionStart() + 256);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + second->GetSize());
    auto mark = ProductMarkObjectFn<Generation::Old>();
    GC_EXPECT_TRUE(mark != nullptr);
    auto view = region->GetMarkView<Generation::Old>();
    GC_EXPECT_FALSE(mark(region, view, fx.obj0, fx.obj0->GetSize(), true));
    GC_EXPECT_FALSE(mark(region, view, second, second->GetSize(), true));
    std::vector<BaseObject*> visited;
    GC_EXPECT_TRUE(region->VisitLiveObjectsUntilFalse([&](BaseObject* obj) {
        visited.push_back(obj);
        return true;
    }));
    GC_EXPECT_EQ(visited.size(), 2u);
    GC_EXPECT_TRUE(visited[0] == fx.obj0);
    GC_EXPECT_TRUE(visited[1] == second);
    size_t visits = 0;
    GC_EXPECT_FALSE(region->VisitLiveObjectsUntilFalse([&](BaseObject*) {
        ++visits;
        return false;
    }));
    GC_EXPECT_EQ(visits, 1u);
}
