// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <dlfcn.h>
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/Collector/MarkStripe.h"
#include "Heap/WCollector/WCollector.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
// Bind the existing product implementation, never instantiate a second copy
// of the mark claim in the test ELF. The runtime entry arms are separate from
// these focused accounting checks.
using ProductMark = bool (*)(const WCollector*, BaseObject*, bool, MarkLiveCache*);
ProductMark CachedMark()
{
    auto fn = reinterpret_cast<ProductMark>(dlsym(RTLD_DEFAULT,
        "_ZNK12MapleRuntime10WCollector14MarkObjectImplEPNS_10BaseObjectEbPNS_13MarkLiveCacheE"));
    GC_EXPECT_TRUE(fn != nullptr);
    return fn;
}

void CheckCachedClaim(bool finalizable, bool repeat, bool large = false)
{
    GcHeapFixture fx;
    if (large) {
        fx.region0->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        fx.region0->SetRegionType(RegionInfo::RegionType::LARGE_REGION);
        fx.obj0 = fx.PlaceObject(fx.region0->GetRegionStart());
        fx.region0->SetRegionAllocPtr(fx.region0->GetRegionStart() + fx.obj0->GetSize());
    }
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap(live, fx.region0->GetRegionSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    const size_t size = fx.obj0->GetSize();
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    if (finalizable) {
        GC_EXPECT_FALSE(collector.ResurrectObject(fx.obj0, offset, fx.region0));
    }
    MarkLiveCache cache(1);
    const bool already = CachedMark()(&collector, fx.obj0, false, &cache);
    const bool secondAlready = repeat ? CachedMark()(&collector, fx.obj0, false, &cache) : true;
    cache.Flush();
    const uint64_t bytes = fx.region0->GetLiveByteCount();
    const uint32_t objects = fx.region0->GetLiveObjectCount();
    // Capture product results before releasing the fixture's bitmap, and do
    // not place a transition assertion ahead of the accounting invariant.
    const bool strong = fx.region0->IsMarkedObject(fx.region0->GetMarkView<Generation::Old>(), fx.obj0);
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    std::fprintf(stderr, "M2_LIVE_RESULT finalizable=%d repeat=%d bytes=%zu expected=%zu strong=%d\n",
                 finalizable, repeat, static_cast<size_t>(bytes), size, strong);
    GC_EXPECT_EQ(bytes, static_cast<uint64_t>(size));
    GC_EXPECT_EQ(objects, 1u);
    GC_EXPECT_TRUE(strong);
    GC_EXPECT_FALSE(already);
    GC_EXPECT_TRUE(secondAlready);
}
}

GC_TEST(MarkPort203Entries, FirstLiveIsAccountedAfterCacheFlush)
{
    CheckCachedClaim(false, false);
}

GC_TEST(MarkPort203Entries, RepeatedStrongClaimDoesNotAccountTwice)
{
    CheckCachedClaim(false, true);
}

GC_TEST(MarkPort203Entries, FinalizableUpgradeDoesNotAccountTwice)
{
    CheckCachedClaim(true, false);
}

GC_TEST(MarkPort203Entries, LargeFirstLiveIsAccountedAfterCacheFlush)
{
    CheckCachedClaim(false, false, true);
}

GC_TEST(MarkPort203Entries, LargeRepeatedStrongClaimDoesNotAccountTwice)
{
    CheckCachedClaim(false, true, true);
}

GC_TEST(MarkPort203Entries, LargeFinalizableUpgradeDoesNotAccountTwice)
{
    CheckCachedClaim(true, false, true);
}

GC_TEST(MarkPort203Entries, CacheCollisionAndExitWriteBothPageCounts)
{
    GcHeapFixture fx;
    LiveInfo* live0 = fx.PlantLiveInfo(fx.region0);
    LiveInfo* live1 = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap(live0, fx.region0->GetRegionSize());
    (void)fx.PlantMarkBitmap(live1, fx.region1->GetRegionSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    // Choose a legal power-of-two shift from actual fixture addresses rather
    // than assuming that mmap placed both pages below one bucket boundary.
    size_t stripes = 1;
    size_t shift = 20;
    while ((fx.region0->GetRegionStart() >> shift) != (fx.region1->GetRegionStart() >> shift)) {
        stripes *= 2;
        ++shift;
    }
    uint32_t evictedObjects = 0;
    uint64_t evictedBytes = 0;
    {
        MarkLiveCache cache(stripes);
        (void)CachedMark()(&collector, fx.obj0, false, &cache);
        (void)CachedMark()(&collector, fx.obj1, false, &cache);
        evictedObjects = fx.region0->GetLiveObjectCount();
        evictedBytes = fx.region0->GetLiveByteCount();
    }
    const auto exitObjects = fx.region1->GetLiveObjectCount();
    const auto exitBytes = fx.region1->GetLiveByteCount();
    const size_t size0 = fx.obj0->GetSize();
    const size_t size1 = fx.obj1->GetSize();
    fx.region0->metadata.liveInfo = nullptr;
    fx.region1->metadata.liveInfo = nullptr;
    fx.FreePlanted(live0);
    fx.FreePlanted(live1);
    std::fprintf(stderr, "M2_CACHE_RESULT collision_objects=%u collision_bytes=%zu exit_objects=%u exit_bytes=%zu\n",
                 evictedObjects, static_cast<size_t>(evictedBytes), exitObjects, static_cast<size_t>(exitBytes));
    GC_EXPECT_EQ(evictedObjects, 1u);
    GC_EXPECT_EQ(evictedBytes, static_cast<uint64_t>(size0));
    GC_EXPECT_EQ(exitObjects, 1u);
    GC_EXPECT_EQ(exitBytes, static_cast<uint64_t>(size1));
}

#if defined(MRT_TESTABLE_INTERNALS)
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/GcThreadPool.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "Heap/Collector/MarkPartialArray.h"

extern "C" int CJ_ScheduleManagerInit();

namespace MapleRuntime {
struct MarkPort203TestAccess {
    static void Bind(CollectorResources& resources, TracingCollector* collector, GCThreadPool* pool)
    {
        resources.collectorProxy.currentCollector = collector;
        resources.gcThreadPool = pool;
        resources.gcThreadCount = 1;
        resources.concurrentGcThreadCount = 1;
    }
    static void CollectYoung(WCollector& collector)
    {
        collector.SetGCReason(GC_REASON_YOUNG);
        collector.DoGarbageCollection();
    }
};
}

namespace {
class MarkPortRuntime final : public Runtime {
public:
    explicit MarkPortRuntime(MutatorManager& manager)
    {
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        runtime = this;
        manager.Init();
        concurrency.Init(ConcurrencyParam{1024, 64, 1});
    }
    ~MarkPortRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam{}; }
    void SetGCThreshold(uint64_t) override {}
private:
    Concurrency concurrency;
};

void RunArrayCollection(const char* variant, size_t helpers)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GC_UNIT_YOUNG_WEAK_VARIANT", variant, 1), 0);
    GC_EXPECT_EQ(unsetenv("MRT_GCV2_PARTIAL_ARRAY"), 0);
    MutatorManager manager;
    MarkPortRuntime runtime(manager);
    GcHeapFixture fx;
    // An actual multi-unit page keeps all array slots in the mapped heap;
    // each subordinate unit resolves back to the same owning region.
    fx.region1 = RegionInfo::InitRegion(1, 4, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());

    alignas(TypeInfo) unsigned char arrayTypeStorage[sizeof(TypeInfo)]{};
    auto* arrayType = reinterpret_cast<TypeInfo*>(arrayTypeStorage);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetFlagHasRefField();
    arrayType->SetComponentTypeInfo(fx.typeInfo);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(arrayTypeStorage), sizeof(arrayTypeStorage));
    constexpr size_t length = 3 * MarkPartialArray::MIN_LENGTH + 17;
    auto* array = reinterpret_cast<MArray*>(fx.region1->GetRegionStart() + 64);
    *reinterpret_cast<uintptr_t*>(array) = reinterpret_cast<uintptr_t>(arrayType);
    array->SetLength(length);
    const size_t arrayBytes = array->GetSize();
    auto* slots = reinterpret_cast<HeapSlot<>*>(array->ConvertToCArray());
    for (size_t i = 0; i < length; ++i) {
        slots[i].StoreColoured(zpointer::null);
    }
    constexpr size_t childrenCount = 40;
    std::vector<BaseObject*> children;
    MAddress next = AlignUp(reinterpret_cast<MAddress>(array) + arrayBytes, size_t{64});
    for (size_t i = 0; i < childrenCount; ++i) {
        BaseObject* child = fx.PlaceObject(next);
        HeapSlotAt<>(next + TYPEINFO_PTR_SIZE).StoreColoured(zpointer::null);
        children.push_back(child);
        next += child->GetSize();
        slots[i].StoreColoured(StoreGoodPointer(child));
    }
    // Distinct tail-only children force the continuation consumer to carry
    // useful work. Prefix processing alone cannot make this result pass.
    for (size_t i = 0; i < 4; ++i) {
        slots[i].StoreColoured(zpointer::null);
    }
    slots[MarkPartialArray::MIN_LENGTH - 1].StoreColoured(StoreGoodPointer(children[0]));
    slots[MarkPartialArray::MIN_LENGTH].StoreColoured(StoreGoodPointer(children[1]));
    slots[2 * MarkPartialArray::MIN_LENGTH].StoreColoured(StoreGoodPointer(children[2]));
    slots[length - 1].StoreColoured(StoreGoodPointer(children[3]));
    fx.region1->SetRegionAllocPtr(next);
    GC_EXPECT_TRUE(next <= fx.region1->GetRegionEnd());

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    GCThreadPool pool("gc-unit-m2-array", static_cast<int32_t>(helpers), GCPoolThread::GC_THREAD_PRIORITY);
    MarkPort203TestAccess::Bind(resources, &collector, &pool);
    collector.SetGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    auto& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(children.back());
    Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(array);
    const bool wasStarted = resources.IsGcStarted();
    const GCReason oldReason = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;
    MarkPort203TestAccess::CollectYoung(collector);

    size_t markedChildren = 0;
    const auto view = fx.region1->GetMarkView<Generation::Young>();
    for (auto* child : children) {
        markedChildren += fx.region1->IsMarkedObject(view, child) ? 1 : 0;
    }
    const bool arrayMarked = fx.region1->IsMarkedObject(view, array);
    const auto objects = fx.region1->GetLiveObjectCount();
    const auto bytes = fx.region1->GetLiveByteCount();
    const size_t expectedBytes = arrayBytes + childrenCount * children[0]->GetSize();
    Heap::GetHeap().RemoveExportObject(handle);
    resources.SetGcStarted(wasStarted);
    resources.GetGCStats().reason = oldReason;
    MarkPort203TestAccess::Bind(resources, nullptr, nullptr);
    pool.Exit();
    std::fprintf(stderr, "M2_ARRAY_RESULT variant=%s array=%d children=%zu objects=%u bytes=%zu expected_bytes=%zu\n",
                 variant, arrayMarked, markedChildren, objects, static_cast<size_t>(bytes), expectedBytes);
    GC_EXPECT_EQ(markedChildren, childrenCount);
    GC_EXPECT_EQ(objects, childrenCount + 1);
    GC_EXPECT_EQ(bytes, static_cast<uint64_t>(expectedBytes));
    GC_EXPECT_TRUE(arrayMarked);
}
}

GC_OTHER_VM_TEST(MarkPort203Entries, SerialCollectionConsumesArrayTails)
{
    RunArrayCollection("serial", 0);
}
GC_OTHER_VM_TEST(MarkPort203Entries, LegacyParallelCollectionConsumesArrayTails)
{
    RunArrayCollection("legacy-parallel", 1);
}
GC_OTHER_VM_TEST(MarkPort203Entries, StripedCollectionConsumesArrayTails)
{
    RunArrayCollection("striped", 1);
}
#endif
