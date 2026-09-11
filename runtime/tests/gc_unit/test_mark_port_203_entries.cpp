// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <dlfcn.h>
#include <memory>
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
    static void Bind(CollectorResources& resources, TracingCollector* collector, GCThreadPool* pool, int32_t count = 1)
    {
        resources.collectorProxy.currentCollector = collector;
        resources.gcThreadPool = pool;
        resources.gcThreadCount = count;
        resources.concurrentGcThreadCount = count;
    }
    static void Collect(WCollector& collector, bool major)
    {
        collector.SetGCReason(major ? GC_REASON_USER : GC_REASON_YOUNG);
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

struct ArrayClosureResult {
    RegionInfo* region = nullptr;
    BaseObject* array = nullptr;
    const std::vector<BaseObject*>* children = nullptr;
    size_t markedChildren = 0;
    bool arrayMarked = false;
    bool finalizable = false;
    bool arrayStrong = false;
    uint32_t objects = 0;
    uint64_t bytes = 0;
    size_t publishedObjects = 0;
    bool allocateBlack = false;
    bool allocationAttempted = false;
    RegionInfo* allocationRegion = nullptr;
    TypeInfo* allocationType = nullptr;
    AllocBuffer* allocationBuffer = nullptr;
    AllocBuffer* previousBuffer = nullptr;
    RegionInfo* previousRegion = nullptr;
    BaseObject* allocated = nullptr;
    uint32_t allocatedObjects = 0;
    uint64_t allocatedBytes = 0;
};
ArrayClosureResult* arrayClosureResult = nullptr;
void ObserveArrayClosure(const std::vector<BaseObject*>* reachable)
{
    auto& result = *arrayClosureResult;
    if (result.allocateBlack && !result.allocationAttempted) {
        result.allocationAttempted = true;
        // Inject a real mutator allocation after the initial GC closure.
        // Allocate itself claims live and publishes its private Follow work;
        // the test only initializes the returned object's header and field.
        result.previousBuffer = AllocBuffer::GetAllocBuffer();
        result.allocationBuffer = AllocBuffer::GetOrCreateAllocBuffer();
        result.previousRegion = result.allocationBuffer->GetRegion();
        result.allocationRegion->SetYoungRegionFlag(1);
        result.allocationRegion->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        result.allocationBuffer->SetRegion(result.allocationRegion);
        const MAddress address = result.allocationBuffer->Allocate(2 * sizeof(MAddress), AllocType::MOVEABLE_OBJECT);
        if (address != 0) {
            result.allocated = reinterpret_cast<BaseObject*>(address);
            *reinterpret_cast<uintptr_t*>(address) = reinterpret_cast<uintptr_t>(result.allocationType);
            HeapSlotAt<>(address + TYPEINFO_PTR_SIZE).StoreColoured(StoreGoodPointer((*result.children)[0]));
        }
        // The mapped fixture page is not in the allocator's intrusive TL
        // list. Restore the buffer shortcut before the GC's ordinary flush;
        // the real private Follow queue remains registered for consumption.
        result.allocationBuffer->SetRegion(result.previousRegion);
    }
    if (result.allocated != nullptr) {
        result.allocatedObjects = result.allocationRegion->GetLiveObjectCount();
        result.allocatedBytes = result.allocationRegion->GetLiveByteCount();
    }
    auto isMarked = [&](BaseObject* object) {
        return result.region->IsYoungRegion()
            ? result.region->IsMarkedObject(result.region->GetMarkView<Generation::Young>(), object)
            : result.region->IsMarkedObject(result.region->GetMarkView<Generation::Old>(), object);
    };
    size_t marked = 0;
    for (auto* child : *result.children) {
        marked += isMarked(child) ? 1 : 0;
    }
    result.markedChildren = marked;
    result.arrayStrong = isMarked(result.array);
    result.arrayMarked = result.arrayStrong ||
        (result.finalizable && result.region->IsResurrectedObject(result.array));
    if (result.finalizable) {
        result.markedChildren = 0;
        for (auto* child : *result.children) {
            result.markedChildren += (isMarked(child) || result.region->IsResurrectedObject(child)) ? 1 : 0;
        }
    }
    result.objects = result.region->GetLiveObjectCount();
    result.bytes = result.region->GetLiveByteCount();
    result.publishedObjects = reachable == nullptr ? 0 : reachable->size();
}

void RunArrayCollection(const char* variant, size_t helpers, bool allocateBlack = false, bool markOnly = false,
                        size_t length = 3 * MarkPartialArray::MIN_LENGTH + 17, bool structArray = false)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    const bool major = std::strncmp(variant, "major", 5) == 0;
    const bool commonRoot = std::strcmp(variant, "major-common") == 0;
    const bool finalizable = std::strcmp(variant, "major-finalizable") == 0;
    if (!major) {
        GC_EXPECT_EQ(setenv("MRT_GC_UNIT_YOUNG_WEAK_VARIANT", variant, 1), 0);
    }
    GC_EXPECT_EQ(unsetenv("MRT_GCV2_PARTIAL_ARRAY"), 0);
    MutatorManager manager;
    MarkPortRuntime runtime(manager);
    GcHeapFixture fx;
    // An actual multi-unit page keeps all array slots in the mapped heap;
    // each subordinate unit resolves back to the same owning region.
    fx.region1 = RegionInfo::InitRegion(1, 4, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    fx.region1->SetYoungRegionFlag(major ? 0 : 1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    if (major) {
        (void)fx.PlantMarkBitmap<Generation::Old>(live, fx.region1->GetRegionSize());
    } else {
        (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());
    }

    alignas(TypeInfo) unsigned char arrayTypeStorage[sizeof(TypeInfo)]{};
    auto* arrayType = reinterpret_cast<TypeInfo*>(arrayTypeStorage);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetFlagHasRefField();
    alignas(TypeInfo) unsigned char structTypeStorage[sizeof(TypeInfo)]{};
    auto* structType = reinterpret_cast<TypeInfo*>(structTypeStorage);
    if (structArray) {
        structType->SetType(TypeKind::TYPE_KIND_STRUCT);
        structType->SetFlagHasRefField();
        structType->SetInstanceSize(2 * sizeof(MAddress));
        GCTib tib{};
        tib.tag = SIGN_BIT | 3;
        structType->SetGCTib(tib);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(structTypeStorage), sizeof(structTypeStorage));
    }
    arrayType->SetComponentTypeInfo(structArray ? structType : fx.typeInfo);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(arrayTypeStorage), sizeof(arrayTypeStorage));
    auto* array = reinterpret_cast<MArray*>(fx.region1->GetRegionStart() + 64);
    *reinterpret_cast<uintptr_t*>(array) = reinterpret_cast<uintptr_t>(arrayType);
    array->SetLength(length);
    const size_t arrayBytes = array->GetSize();
    auto* slots = reinterpret_cast<HeapSlot<>*>(array->ConvertToCArray());
    const size_t referenceSlots = length * (structArray ? 2 : 1);
    for (size_t i = 0; i < referenceSlots; ++i) {
        slots[i].StoreColoured(zpointer::null);
    }
    constexpr size_t childrenCount = 160;
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
    const size_t tail0 = referenceSlots > 2 * MarkPartialArray::MIN_LENGTH ? MarkPartialArray::MIN_LENGTH - 1 : referenceSlots - 4;
    const size_t tail1 = referenceSlots > 2 * MarkPartialArray::MIN_LENGTH ? MarkPartialArray::MIN_LENGTH : referenceSlots - 3;
    const size_t tail2 = referenceSlots > 2 * MarkPartialArray::MIN_LENGTH ? 2 * MarkPartialArray::MIN_LENGTH : referenceSlots - 2;
    if (!allocateBlack) {
        slots[tail0].StoreColoured(StoreGoodPointer(children[0]));
    }
    slots[tail1].StoreColoured(StoreGoodPointer(children[1]));
    slots[tail2].StoreColoured(StoreGoodPointer(children[2]));
    slots[referenceSlots - 1].StoreColoured(StoreGoodPointer(children[3]));
    BaseObject* finalizerRoot = nullptr;
    if (finalizable) {
        finalizerRoot = fx.PlaceObject(next);
        HeapSlotAt<>(next + TYPEINFO_PTR_SIZE).StoreColoured(StoreGoodPointer(array));
        next += finalizerRoot->GetSize();
    }
    fx.region1->SetRegionAllocPtr(next);
    GC_EXPECT_TRUE(next <= fx.region1->GetRegionEnd());

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    GCThreadPool pool("gc-unit-m2-array", static_cast<int32_t>(helpers), GCPoolThread::GC_THREAD_PRIORITY);
    MarkPort203TestAccess::Bind(resources, &collector, &pool, static_cast<int32_t>(helpers + 1));
    collector.SetGCPhase(major ? GCPhase::GC_PHASE_IDLE : GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    auto& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(children.back());
    Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    const size_t rootCount = commonRoot && helpers != 0 ? 17 * 64 : 1;
    std::unique_ptr<RootSlot[]> rootSlots(new RootSlot[rootCount]);
    std::vector<RootSlot*> roots(rootCount);
    U64 handle = 0;
    AllocBuffer* invisibleBuffer = nullptr;
    bool ownsInvisibleBuffer = false;
    if (finalizable) {
        resources.GetFinalizerProcessor().RegisterFinalizer(finalizerRoot);
    } else if (markOnly) {
        ownsInvisibleBuffer = AllocBuffer::GetAllocBuffer() == nullptr;
        invisibleBuffer = AllocBuffer::GetOrCreateAllocBuffer();
        array->SetInvisibleObject(true);
        invisibleBuffer->PushInvisibleRoot(array);
    } else if (commonRoot) {
        for (size_t i = 0; i < rootCount; ++i) {
            StorePlain(rootSlots[i], from_object(array));
            roots[i] = &rootSlots[i];
        }
        Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(roots.data()), static_cast<U32>(rootCount));
    } else {
        handle = Heap::GetHeap().RegisterExportRoot(array);
    }
    if (major) {
        space.GetRegionManager().AddRawPointerObject(array);
        for (auto* child : children) {
            space.GetRegionManager().AddRawPointerObject(child);
        }
    }
    const bool wasStarted = resources.IsGcStarted();
    const GCReason oldReason = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = major ? GC_REASON_USER : GC_REASON_YOUNG;
    ArrayClosureResult result;
    result.region = fx.region1;
    result.array = array;
    result.children = &children;
    result.finalizable = finalizable;
    result.allocateBlack = allocateBlack;
    result.allocationRegion = fx.region0;
    result.allocationType = fx.typeInfo;
    arrayClosureResult = &result;
    SetMarkClosureObserverForTest(ObserveArrayClosure);
    MarkPort203TestAccess::Collect(collector, major);
    SetMarkClosureObserverForTest(nullptr);
    arrayClosureResult = nullptr;

    const size_t markedChildren = result.markedChildren;
    const bool arrayMarked = result.arrayMarked;
    const auto objects = result.objects;
    const auto bytes = result.bytes;
    const size_t expectedChildren = markOnly ? 0 : childrenCount;
    const size_t expectedObjects = expectedChildren + 1 + (finalizable ? 1 : 0);
    const size_t expectedBytes = arrayBytes + expectedChildren * children[0]->GetSize() +
        (finalizable ? finalizerRoot->GetSize() : 0);
    if (finalizable) {
        resources.GetFinalizerProcessor().VisitRawPointers([](ObjectRef& root) {
            StorePlain(root, zaddress::null);
        });
    } else if (markOnly) {
        if (ownsInvisibleBuffer) {
            invisibleBuffer->SetRegion(nullptr);
            invisibleBuffer->Fini();
            ThreadLocal::SetAllocBuffer(nullptr);
            delete invisibleBuffer;
        }
    } else if (commonRoot) {
        Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(roots.data()), static_cast<U32>(rootCount));
    } else {
        Heap::GetHeap().RemoveExportObject(handle);
    }
    resources.SetGcStarted(wasStarted);
    resources.GetGCStats().reason = oldReason;
    MarkPort203TestAccess::Bind(resources, nullptr, nullptr);
    pool.Exit();
    if (result.allocationBuffer != nullptr) {
        result.allocationBuffer->SetRegion(result.previousRegion);
        if (result.previousBuffer == nullptr) {
            result.allocationBuffer->SetRegion(nullptr);
            result.allocationBuffer->Fini();
            ThreadLocal::SetAllocBuffer(nullptr);
            delete result.allocationBuffer;
        }
    }
    std::fprintf(stderr, "M2_ALLOC_RESULT attempted=%d objects=%u bytes=%zu\n",
                 result.allocationAttempted, result.allocatedObjects, static_cast<size_t>(result.allocatedBytes));
    std::fprintf(stderr, "M2_ARRAY_RESULT variant=%s array=%d children=%zu objects=%u bytes=%zu expected_bytes=%zu\n",
                 variant, arrayMarked, markedChildren, objects, static_cast<size_t>(bytes), expectedBytes);
    GC_EXPECT_EQ(markedChildren, expectedChildren);
    GC_EXPECT_EQ(objects, expectedObjects);
    GC_EXPECT_EQ(bytes, static_cast<uint64_t>(expectedBytes));
    GC_EXPECT_TRUE(arrayMarked);
    if (finalizable) {
        GC_EXPECT_FALSE(result.arrayStrong);
    }
    if (!major) {
        GC_EXPECT_EQ(result.publishedObjects, expectedChildren + 1 + (allocateBlack ? 1 : 0));
    }
    if (allocateBlack) {
        GC_EXPECT_EQ(result.allocatedObjects, 1u);
        GC_EXPECT_EQ(result.allocatedBytes, static_cast<uint64_t>(2 * sizeof(MAddress)));
    }
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

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(MarkPort203Entries, MajorSerialCollectionConsumesArrayTails)
{
    RunArrayCollection("major-common", 0);
}
GC_OTHER_VM_TEST(MarkPort203Entries, MajorParallelCollectionConsumesArrayTails)
{
    RunArrayCollection("major-common", 1);
}
GC_OTHER_VM_TEST(MarkPort203Entries, MajorExportCollectionConsumesArrayTails)
{
    RunArrayCollection("major-export", 1);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(MarkPort203Entries, AllocateBlackFollowKeepsSingleLiveCount)
{
    RunArrayCollection("striped", 1, true);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(MarkPort203Entries, SerialInvisibleRootIsLiveWithoutFollowingFields)
{
    RunArrayCollection("serial", 0, false, true);
}
GC_OTHER_VM_TEST(MarkPort203Entries, StripedInvisibleRootIsLiveWithoutFollowingFields)
{
    RunArrayCollection("striped", 1, false, true);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(MarkPort203Entries, FinalizableArrayClosureAccountsWithoutStrongUpgrade)
{
    RunArrayCollection("major-finalizable", 1);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(MarkPort203Entries, SerialCollectionHandlesExactArrayThreshold)
{
    RunArrayCollection("serial", 0, false, false, MarkPartialArray::MIN_LENGTH);
}
GC_OTHER_VM_TEST(MarkPort203Entries, SerialCollectionHandlesOnePastArrayThreshold)
{
    RunArrayCollection("serial", 0, false, false, MarkPartialArray::MIN_LENGTH + 1);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(MarkPort203Entries, StructArrayCollectionVisitsBothFields)
{
    RunArrayCollection("striped", 1, false, false, MarkPartialArray::MIN_LENGTH + 1, true);
}
#endif
