// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_cycle_sequence_fixture.hpp"
#include <memory>
#include <string>
#include <unistd.h>
#include "gc_heap_fixture.hpp"
#include "zunittest.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zBarrier.hpp"


#include "gc_generation_test.hpp"
#include "gc_product_access_test.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
// Exercise the product mark-entry page claim and live accounting directly.
// Runtime entry arms remain separate from these focused accounting checks.
void CheckCachedClaim(bool finalizable, bool repeat, bool large = false)
{
    GcHeapFixture fx;
    if (large) {
        (void)ZPageType::large;
        fx.obj0 = fx.PlaceObject(fx.region0->GetRegionStart());
        fx.region0->SetRegionAllocPtr(fx.region0->GetRegionStart() + fx.obj0->GetSize());
    }
    const size_t size = fx.obj0->GetSize();
    if (finalizable) {
        GC_EXPECT_FALSE(ZMark::MarkEntryObject(fx.obj0,
            MarkStackEntry(untype(ZAddress::offset(from_object(fx.obj0))), true, true, false, true), nullptr));
    }
    MarkLiveCache cache(1);
    const bool already = ZMark::MarkEntryObject(fx.obj0,
        MarkStackEntry(untype(ZAddress::offset(from_object(fx.obj0))), true, true, false, false), &cache);
    const bool secondAlready = repeat ? ZMark::MarkEntryObject(fx.obj0,
        MarkStackEntry(untype(ZAddress::offset(from_object(fx.obj0))), true, true, false, false), &cache) : true;
    cache.Flush();
    const uint64_t bytes = fx.region0->live_bytes();
    const uint32_t objects = fx.region0->live_objects();
    // Do not place a transition assertion ahead of the accounting invariant.
    const bool strong = fx.region0->is_object_strongly_live(from_object(fx.obj0));
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
        (void)ZMark::MarkEntryObject(fx.obj0,
            MarkStackEntry(untype(ZAddress::offset(from_object(fx.obj0))), true, true, false, false), &cache);
        (void)ZMark::MarkEntryObject(fx.obj1,
            MarkStackEntry(untype(ZAddress::offset(from_object(fx.obj1))), true, true, false, false), &cache);
        evictedObjects = fx.region0->live_objects();
        evictedBytes = fx.region0->live_bytes();
    }
    const auto exitObjects = fx.region1->live_objects();
    const auto exitBytes = fx.region1->live_bytes();
    const size_t size0 = fx.obj0->GetSize();
    const size_t size1 = fx.obj1->GetSize();
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
#include "Heap/z/zMark.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zWorkers.hpp"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "Heap/z/zMarkPartialArray.hpp"

extern "C" int CJ_ScheduleManagerInit();

namespace MapleRuntime {
struct MarkPort203TestAccess {
    static void Bind(Heap* collector, int32_t count = 1)
    {
        if (collector != nullptr) CHECK(collector == &Heap::GetHeap());
        ZCollectedHeapTest::SetWorkers(count);
    }
    // Phase-unit boundary approved for P16: setup uses the product mark-start
    // operation, and all observed state is read before relocation can reset it.
    static void Collect(Heap& collector, bool major, BaseObject* array, bool markOnly, int duplicateRootOrder)
    {
        if (major) {
            // The heap fixture has an active synthetic epoch. The real old
            // mark-start owns Begin (ZGC zGeneration.cpp:1212-1240).
            if (collector.old().Snapshot().active) collector.old().End();
            ScopedStopTheWorld pause("P16 old mark-start fixture", false);
            collector.old().mark_start();
        } else {
            YoungTypeSetter type(collector.young(), ZYoungType::minor);
            collector.young().pause_mark_start();
        }
        if (duplicateRootOrder != 0) {
            if (duplicateRootOrder < 0) { ZBarrier::Mark<false, false, true, false>(from_object(array)); }
            ZBarrier::Mark<false, false, false, false>(from_object(array));
            if (duplicateRootOrder > 0) { ZBarrier::Mark<false, false, true, false>(from_object(array)); }
        } else if (markOnly) {
            array->SetInvisibleObject(true);
            ZBarrier::Mark<false, false, false, false>(from_object(array));
        }
        if (major) { collector.old().concurrent_mark(); }
        else { collector.young().concurrent_mark(); }
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
    ZPage* region = nullptr;
    BaseObject* array = nullptr;
    const std::vector<BaseObject*>* children = nullptr;
    size_t markedChildren = 0;
    bool arrayMarked = false;
    bool finalizable = false;
    bool arrayStrong = false;
    uint32_t objects = 0;
    uint64_t bytes = 0;

};
void ReadArrayMarkState(ArrayClosureResult& result)
{


    auto isMarked = [&](BaseObject* object) {
        return result.region->is_object_strongly_live(from_object(object));
    };
    auto isResurrected = [&](BaseObject* object) {
        return result.region->is_object_live(from_object(object)) &&
            !result.region->is_object_strongly_live(from_object(object));
    };
    size_t marked = 0;
    for (auto* child : *result.children) {
        marked += isMarked(child) ? 1 : 0;
    }
    result.markedChildren = marked;
    result.arrayStrong = isMarked(result.array);
    result.arrayMarked = result.arrayStrong ||
        (result.finalizable && isResurrected(result.array));
    if (result.finalizable) {
        result.markedChildren = 0;
        for (auto* child : *result.children) {
            result.markedChildren += (isMarked(child) || isResurrected(child)) ? 1 : 0;
        }
    }
    result.objects = result.region->live_objects();
    result.bytes = result.region->live_bytes();
}

void RunArrayCollection(const char* variant, size_t helpers, bool markOnly = false,
                        size_t length = 3 * MarkPartialArray::MIN_LENGTH + 17, bool structArray = false,
                        int duplicateRootOrder = 0)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    // Phase timers require the same storage initialization as CollectorResources::Init.
    // ZGC ZStatValue::initialize, zStat.cpp:362.
    ZStat::Initialize();
    const bool major = std::strncmp(variant, "major", 5) == 0;
    const bool commonRoot = std::strcmp(variant, "major-common") == 0;
    const bool finalizable = std::strcmp(variant, "major-finalizable") == 0;
    MutatorManager manager;
    MarkPortRuntime runtime(manager);
    GcHeapFixture fx;
    // Install the synthetic payload reservation before publishing GC roots.
    Heap::OnHeapCreated(fx.heapStart);
    Heap::OnHeapExtended(fx.heapStart + GcHeapFixture::kUnits * ZGranuleSize);
    // The 2 MiB small page already covers the entire reference array.
    fx.region1->reset(major ? PageAge::old : PageAge::eden);
    // The product allocates and owns this page's livemap (InitRegion ->
    // InitializeLiveMap); promotion transfers that ownership
    // (ZPage::clone_for_promotion, zPage.cpp:64).

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
    slots[tail0].StoreColoured(StoreGoodPointer(children[0]));
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

    Heap& collector = Heap::GetHeap();
    MarkPort203TestAccess::Bind(&collector, static_cast<int32_t>(helpers + 1));
    // ZGeneration owns its worker set (zGeneration.cpp:124-129).
    for (auto generation : {ZGenerationId::young, ZGenerationId::old}) {
        Heap::GetHeap().GetZGeneration(generation).InitializeWorkers(helpers + 1);
    }
    ZGenerationTest::SetReason(Heap::GetHeap().GetZGeneration(major ? ZGenerationId::old : ZGenerationId::young), major ? GC_REASON_USER : GC_REASON_YOUNG);
    Heap::GetHeap().GetZGeneration(major ? ZGenerationId::old : ZGenerationId::young).set_phase(major ? ZGenerationPhase::Relocate : ZGenerationPhase::MarkComplete);
    auto& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    fx.region1->SetRegionRole(ZPageRole::RecentFull);
    const size_t rootCount = commonRoot && helpers != 0 ? 17 * 64 : 1;
    std::vector<NativeSlot> rootSlots(rootCount, NativeSlot(zpointer::null));
    std::vector<NativeSlot*> roots(rootCount);
    U64 handle = 0;
    AllocBuffer* invisibleBuffer = nullptr;
    bool ownsInvisibleBuffer = false;
    if (finalizable) {
        Heap::GetHeap().GetFinalizerProcessor().RegisterFinalizer(finalizerRoot);
    } else if (markOnly || duplicateRootOrder != 0) {
        ownsInvisibleBuffer = AllocBuffer::GetAllocBuffer() == nullptr;
        invisibleBuffer = AllocBuffer::GetOrCreateAllocBuffer();
    } else if (commonRoot) {
        for (size_t i = 0; i < rootCount; ++i) {
            rootSlots[i].StoreColoured(StoreGoodPointer(array));
            roots[i] = &rootSlots[i];
        }
        Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(roots.data()), static_cast<U32>(rootCount));
    } else {
        handle = Heap::GetHeap().RegisterExportRoot(array);
    }
    const bool wasStarted = Heap::GetHeap().IsGcStarted();
    const GCReason oldReason = Heap::GetHeap().GetZGeneration(
        major ? ZGenerationId::old : ZGenerationId::young).Snapshot().reason;
    auto& activityCycle = Heap::GetHeap().GetZGeneration(major ? ZGenerationId::old : ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    ZGenerationTest::SetReason(Heap::GetHeap().GetZGeneration(major ? ZGenerationId::old : ZGenerationId::young), major ? GC_REASON_USER : GC_REASON_YOUNG);
    ArrayClosureResult result;
    result.region = fx.region1;
    result.array = array;
    result.children = &children;
    result.finalizable = finalizable;
    MarkPort203TestAccess::Collect(collector, major, array, markOnly, duplicateRootOrder);
    ReadArrayMarkState(result);

    const size_t markedChildren = result.markedChildren;
    const bool arrayMarked = result.arrayMarked;
    const auto objects = result.objects;
    const auto bytes = result.bytes;
    // ZMark::follow_work (zMark.cpp:412-415): a duplicate mark returns before
    // follow. The LIFO mark-only entry wins when it was published last.
    const size_t expectedChildren = (markOnly || duplicateRootOrder < 0) ? 0 : childrenCount;
    const size_t expectedObjects = expectedChildren + 1 + (finalizable ? 1 : 0);
    const size_t expectedBytes = arrayBytes + expectedChildren * children[0]->GetSize() +
        (finalizable ? finalizerRoot->GetSize() : 0);
    if (finalizable) {
        Heap::GetHeap().GetFinalizerProcessor().VisitNativePointers([](NativeSlot& root) {
            root.StoreColoured(StoreGoodPointer(nullptr));
        });
    } else if (markOnly || duplicateRootOrder != 0) {
        if (ownsInvisibleBuffer) {
            invisibleBuffer->ClearRegion();
            invisibleBuffer->Fini();
            ThreadLocal::SetAllocBuffer(nullptr);
            delete invisibleBuffer;
        }
    } else if (commonRoot) {
        Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(roots.data()), static_cast<U32>(rootCount));
    } else {
        Heap::GetHeap().RemoveExportObject(handle);
    }
    if (!ownerWasActive) activityCycle.End();
    ZGenerationTest::SetReason(Heap::GetHeap().GetZGeneration(major ? ZGenerationId::old : ZGenerationId::young), oldReason);

    // Worker TLS cleanup must finish while the heap generation owns publication.
    for (auto generation : {ZGenerationId::young, ZGenerationId::old}) {
        Heap::GetHeap().GetZGeneration(generation).StopWorkers();
    }
    MarkPort203TestAccess::Bind(nullptr);
    std::fprintf(stderr, "M2_ARRAY_RESULT variant=%s array=%d children=%zu objects=%u bytes=%zu expected_bytes=%zu\n",
                 variant, arrayMarked, markedChildren, objects, static_cast<size_t>(bytes), expectedBytes);
    GC_EXPECT_EQ(markedChildren, expectedChildren);
    std::fprintf(stderr, "M2_LIVE_ASSERT objects_ok=%d bytes_ok=%d root_order=%d\n",
                 objects == expectedObjects, bytes == expectedBytes, duplicateRootOrder);
    GC_EXPECT_TRUE(objects == expectedObjects && bytes == static_cast<uint64_t>(expectedBytes));
    GC_EXPECT_TRUE(arrayMarked);
    if (finalizable) {
        GC_EXPECT_FALSE(result.arrayStrong);
    }
    if (!major) {
        // Observer publication counts were removed; live accounting is checked above.
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
GC_OTHER_VM_TEST(MarkPort203Entries, SerialInvisibleRootIsLiveWithoutFollowingFields)
{
    RunArrayCollection("serial", 0, true);
}
GC_OTHER_VM_TEST(MarkPort203Entries, StripedInvisibleRootIsLiveWithoutFollowingFields)
{
    RunArrayCollection("striped", 1, true);
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
    RunArrayCollection("serial", 0, false, MarkPartialArray::MIN_LENGTH);
}
GC_OTHER_VM_TEST(MarkPort203Entries, SerialCollectionHandlesOnePastArrayThreshold)
{
    RunArrayCollection("serial", 0, false, MarkPartialArray::MIN_LENGTH + 1);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(MarkPort203Entries, StructArrayCollectionVisitsBothFields)
{
    RunArrayCollection("striped", 1, false, MarkPartialArray::MIN_LENGTH + 1, true);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(MarkPort203Entries, SerialInvisibleThenNormalAccountsOnce)
{
    RunArrayCollection("serial", 0, false, 3 * MarkPartialArray::MIN_LENGTH + 17, false, 1);
}
GC_OTHER_VM_TEST(MarkPort203Entries, SerialNormalThenInvisibleAccountsOnce)
{
    RunArrayCollection("serial", 0, false, 3 * MarkPartialArray::MIN_LENGTH + 17, false, -1);
}
GC_OTHER_VM_TEST(MarkPort203Entries, ParallelInvisibleThenNormalAccountsOnce)
{
    RunArrayCollection("legacy-parallel", 1, false, 3 * MarkPartialArray::MIN_LENGTH + 17, false, 1);
}
GC_OTHER_VM_TEST(MarkPort203Entries, ParallelNormalThenInvisibleAccountsOnce)
{
    RunArrayCollection("legacy-parallel", 1, false, 3 * MarkPartialArray::MIN_LENGTH + 17, false, -1);
}
GC_OTHER_VM_TEST(MarkPort203Entries, StripedInvisibleThenNormalAccountsOnce)
{
    RunArrayCollection("striped", 1, false, 3 * MarkPartialArray::MIN_LENGTH + 17, false, 1);
}
GC_OTHER_VM_TEST(MarkPort203Entries, StripedNormalThenInvisibleAccountsOnce)
{
    RunArrayCollection("striped", 1, false, 3 * MarkPartialArray::MIN_LENGTH + 17, false, -1);
}

namespace {
// Observe the two independent product inputs at the real young phase entry.
// Neither the fixture nor the assertions drain mark work themselves.
void RunCombinedYoungFollow(size_t workers, bool continuation)
{
    setenv("MRT_GC_LOG", "1", 1);
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    MarkPortRuntime runtime(manager);
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->SetRegionRole(ZPageRole::RecentFull);
    auto& heap = Heap::GetHeap();
    auto& young = heap.young();
    MarkPort203TestAccess::Bind(&heap, static_cast<int32_t>(workers));
    young.InitializeWorkers(workers);
    heap.old().InitializeWorkers(workers);
    heap.old().set_phase(ZGenerationPhase::Mark);
    young.set_phase(ZGenerationPhase::MarkComplete);
    ZGenerationTest::SetReason(young, GC_REASON_YOUNG);
    if (!young.Snapshot().active) young.Begin(1);

    MAddress next = reinterpret_cast<MAddress>(fx.obj1);
    auto object = [&]() {
        auto* result = fx.PlaceObject(next);
        HeapSlotAt<>(next + TYPEINFO_PTR_SIZE).StoreColoured(zpointer::null);
        next += 64;
        return result;
    };
    BaseObject* root = object();
    BaseObject* rootChild = object();
    BaseObject* remembered = object();
    BaseObject* rememberedChild = object();
    fx.region1->SetRegionAllocPtr(next);
    HeapSlotAt<>(reinterpret_cast<MAddress>(root) + TYPEINFO_PTR_SIZE)
        .StoreColoured(StoreGoodPointer(rootChild));
    HeapSlotAt<>(reinterpret_cast<MAddress>(remembered) + TYPEINFO_PTR_SIZE)
        .StoreColoured(StoreGoodPointer(rememberedChild));
    auto* oldSlot = reinterpret_cast<volatile zpointer*>(
        reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    HeapSlotAt<>(reinterpret_cast<MAddress>(oldSlot)).StoreColoured(StoreGoodPointer(remembered));
    fx.region0->remember(oldSlot);
    young.register_with_remset(fx.region0);
    const U64 handle = heap.RegisterExportRoot(root);
    YoungTypeSetter type(young, ZYoungType::minor);
    young.pause_mark_start();
    FILE* phaseLog = std::tmpfile();
    GC_EXPECT_TRUE(phaseLog != nullptr);
    const int savedStderr = dup(STDERR_FILENO);
    GC_EXPECT_TRUE(savedStderr >= 0);
    GC_EXPECT_TRUE(dup2(fileno(phaseLog), STDERR_FILENO) >= 0);
    if (continuation) {
        // Roots are already published when mark-end asks for another follow.
        // The remembered page remains a genuine mark-start-flipped input.
        young.produceYoungRoots();
        young.concurrent_mark_continue();
    } else {
        young.concurrent_mark();
    }
    std::fflush(stderr);
    (void)dup2(savedStderr, STDERR_FILENO);
    close(savedStderr);
    std::rewind(phaseLog);
    std::string phases;
    char line[1024];
    while (std::fgets(line, sizeof(line), phaseLog) != nullptr) phases += line;
    std::fclose(phaseLog);
    std::fputs(phases.c_str(), stderr);
    auto phaseCount = [&](const char* name) {
        const std::string token = std::string(" name=") + name + " ";
        size_t count = 0;
        for (size_t pos = 0; (pos = phases.find(token, pos)) != std::string::npos; pos += token.size()) ++count;
        return count;
    };
    const size_t rootWindows = phaseCount("young.root_enum");
    const size_t followWindows = phaseCount("young.mark_follow");
    const size_t splitWindows = phaseCount("young.remset_rescan") + phaseCount("young.mark_closure");
    const bool rootLive = fx.region1->is_object_strongly_live(from_object(rootChild));
    const bool rememberedLive = fx.region1->is_object_strongly_live(from_object(rememberedChild));
    const bool previousCleared = !fx.region0->was_remembered(oldSlot);
    const bool rearmed = fx.region0->is_remembered(oldSlot);
    std::fprintf(stderr,
        "YOUNG828_RESULT workers=%zu continuation=%d root_child=%d remset_child=%d previous_cleared=%d rearmed=%d\n",
        workers, continuation, rootLive, rememberedLive, previousCleared, rearmed);
    heap.RemoveExportObject(handle);
    // One combined target assertion prevents an earlier receipt from hiding
    // either input's contribution to the phase result.
    GC_EXPECT_TRUE(rootLive && rememberedLive && previousCleared && rearmed);
    std::fprintf(stderr, "YOUNG828_PHASE_ASSERT roots=%zu follow=%zu split=%zu\n",
                 rootWindows, followWindows, splitWindows);
    // Existing product phase records are the observation, with roots/follow
    // as positive controls for the absence of the separate serial windows.
    GC_EXPECT_TRUE(rootWindows == 1 && followWindows == 1 && splitWindows == 0);
}
}

GC_OTHER_VM_TEST(YoungCombinedFollow828, InitialSingleWorker)
{
    RunCombinedYoungFollow(1, false);
}
GC_OTHER_VM_TEST(YoungCombinedFollow828, InitialMultipleWorkers)
{
    RunCombinedYoungFollow(2, false);
}
GC_OTHER_VM_TEST(YoungCombinedFollow828, ContinueSingleWorker)
{
    RunCombinedYoungFollow(1, true);
}
GC_OTHER_VM_TEST(YoungCombinedFollow828, ContinueMultipleWorkers)
{
    RunCombinedYoungFollow(2, true);
}
#endif
