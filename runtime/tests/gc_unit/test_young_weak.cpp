// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Common/Runtime.h"
#include "CjScheduler.h"

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Concurrency/Concurrency.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/TracingCollector.h"
#include "Heap/GcThreadPool.h"
#include "Heap/Heap.h"
#include "Heap/WCollector/WCollector.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(MRT_TESTABLE_INTERNALS)

extern "C" int CJ_ScheduleManagerInit();

namespace MapleRuntime {

struct RelocationReceiptTestAccess {
    static void BindCollector(CollectorResources& resources, TracingCollector* collector)
    {
        resources.collectorProxy.currentCollector = collector;
    }

    static void BindThreadPool(CollectorResources& resources, GCThreadPool* threadPool)
    {
        resources.gcThreadPool = threadPool;
    }

    static void RunYoungCollection(WCollector& collector)
    {
        collector.SetGCReason(GC_REASON_YOUNG);
        collector.DoGarbageCollection();
    }

    static void RunMajorMark(WCollector& collector)
    {
        collector.SetGCReason(GC_REASON_USER);
        collector.TraceHeap();
    }
};

} // namespace MapleRuntime

namespace {

class WeakClosureTestRuntime final : public Runtime {
public:
    explicit WeakClosureTestRuntime(MutatorManager& manager)
    {
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        runtime = this;
        manager.Init();
        const ConcurrencyParam concurrencyParam = { 1024, 64, 1 };
        concurrency.Init(concurrencyParam);
    }

    ~WeakClosureTestRuntime() override { runtime = nullptr; }

    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}

private:
    Concurrency concurrency;
};

struct WeakGraph {
    explicit WeakGraph(GcHeapFixture& fixture, RegionInfo* region, RegionInfo* targetRegion = nullptr)
        : fx(fixture), owner(region), targetOwner(targetRegion == nullptr ? region : targetRegion)
    {
        std::memset(weakTypeStorage, 0, sizeof(weakTypeStorage));
        weakType = reinterpret_cast<TypeInfo*>(weakTypeStorage);
        weakType->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
        weakType->SetFlagHasRefField();
        weakType->SetInstanceSize(sizeof(void*));
        GCTib gctib {};
        gctib.tag = SIGN_BIT | 1;
        weakType->SetGCTib(gctib);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(weakTypeStorage), sizeof(weakTypeStorage));

        const MAddress base = owner->GetRegionStart();
        const MAddress targetBase = targetOwner->GetRegionStart();
        const bool splitRegions = targetOwner != owner;
        strongRoot = fx.PlaceObject(base + 64);
        weak = fx.PlaceObject(base + 128);
        referent = fx.PlaceObject(targetBase + (splitRegions ? 64 : 192));
        child = fx.PlaceObject(targetBase + (splitRegions ? 128 : 256));
        *reinterpret_cast<uintptr_t*>(weak) = reinterpret_cast<uintptr_t>(weakType);
        owner->SetRegionAllocPtr(reinterpret_cast<MAddress>(weak) + 64);
        targetOwner->SetRegionAllocPtr(reinterpret_cast<MAddress>(child) + 64);

        Field(strongRoot).StoreColoured(GcUnit::StoreGoodPointer(weak));
        Field(weak).StoreColoured(GcUnit::StoreGoodPointer(referent));
        Field(referent).StoreColoured(GcUnit::StoreGoodPointer(child));
        Field(child).StoreColoured(zpointer::null);
    }

    static HeapSlot<>& Field(BaseObject* object)
    {
        return HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
    }

    bool IsMarked(BaseObject* object) const
    {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region->IsYoungRegion()) {
            return region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object);
        }
        return region->IsMarkedObject(region->GetMarkView<Generation::Old>(), object);
    }

    GcHeapFixture& fx;
    RegionInfo* owner;
    RegionInfo* targetOwner;
    BaseObject* strongRoot = nullptr;
    BaseObject* weak = nullptr;
    BaseObject* referent = nullptr;
    BaseObject* child = nullptr;
    alignas(TypeInfo) unsigned char weakTypeStorage[sizeof(TypeInfo)];
    TypeInfo* weakType = nullptr;
};

void RunYoungWeakVariant(const char* variant, size_t helpers,
                         uint64_t expectedSerial, uint64_t expectedLegacyParallel, uint64_t expectedStriped)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GC_UNIT_YOUNG_WEAK_VARIANT", variant, 1), 0);
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());
    WeakGraph graph(fx, fx.region1);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    GCThreadPool threadPool("gc-unit-young-weak", static_cast<int32_t>(helpers),
                            GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(graph.child);
    Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    const U64 rootHandle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);

    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;
    ResetYoungWeakClosureTestReceipt();

    RelocationReceiptTestAccess::RunYoungCollection(collector);
    const YoungWeakClosureTestReceipt receipt = ReadYoungWeakClosureTestReceipt();
    const bool strongMarked = graph.IsMarked(graph.strongRoot);
    const bool weakMarked = graph.IsMarked(graph.weak);
    const bool referentMarked = graph.IsMarked(graph.referent);
    const bool childMarked = graph.IsMarked(graph.child);
    std::fprintf(stderr,
                 "DETAIL young_weak variant=%s serial=%zu legacy_parallel=%zu striped=%zu "
                 "strong_mark=%d weak_mark=%d referent_mark=%d child_mark=%d\n",
                 variant, static_cast<size_t>(receipt.serial), static_cast<size_t>(receipt.legacyParallel),
                 static_cast<size_t>(receipt.striped), static_cast<int>(strongMarked),
                 static_cast<int>(weakMarked), static_cast<int>(referentMarked), static_cast<int>(childMarked));

    Heap::GetHeap().RemoveExportObject(rootHandle);
    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;
    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    threadPool.Exit();
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);

    GC_EXPECT_EQ(receipt.serial, expectedSerial);
    GC_EXPECT_EQ(receipt.legacyParallel, expectedLegacyParallel);
    GC_EXPECT_EQ(receipt.striped, expectedStriped);
    GC_EXPECT_TRUE(strongMarked);
    GC_EXPECT_TRUE(weakMarked);
    GC_EXPECT_FALSE(referentMarked);
    GC_EXPECT_FALSE(childMarked);
    (void)live;
}

void RunYoungWeakRemsetFlow()
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GC_UNIT_YOUNG_WEAK_VARIANT", "serial", 1), 0);
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;

    // Record the referent field through the product write barrier while it is
    // a real old->young edge.  The holder then enters this young collection;
    // the remembered slot itself, rather than a test-built weakSlots ledger,
    // is what the product minor path receives.
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    WeakGraph graph(fx, fx.region0, fx.region1);
    *reinterpret_cast<uintptr_t*>(graph.referent) = reinterpret_cast<uintptr_t>(fx.typeInfo);
    *reinterpret_cast<uintptr_t*>(graph.child) = reinterpret_cast<uintptr_t>(fx.typeInfo);
    fx.typeInfo->SetUUID(1);
    TypeInfoManager::GetTypeInfoManager().AddTypeInfo(fx.typeInfo);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    rememberedSet.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rememberedSet);
    HeapSlot<>& referentField = WeakGraph::Field(graph.weak);
    barrier.WriteReference(graph.weak, referentField, graph.referent);
    const MAddress weakSlot = reinterpret_cast<MAddress>(&referentField);
    const bool recordedBeforeMinor = rememberedSet.Contains(weakSlot);

    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* holderLive = fx.PlantLiveInfo(fx.region0);
    LiveInfo* targetLive = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(holderLive, fx.region0->GetRegionSize());
    (void)fx.PlantMarkBitmap<Generation::Young>(targetLive, fx.region1->GetRegionSize());
    GCThreadPool threadPool("gc-unit-young-weak-remset", 0, GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region0);
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(graph.strongRoot);
    space.GetRegionManager().AddRawPointerObject(graph.child);
    const U64 rootHandle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);

    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;
    RelocationReceiptTestAccess::RunYoungCollection(collector);
    const bool referentMarked = graph.IsMarked(graph.referent);
    std::fprintf(stderr,
                 "DETAIL young_weak_remset slot=%#zx recorded_before_minor=%d referent_mark=%d\n",
                 static_cast<size_t>(weakSlot), static_cast<int>(recordedBeforeMinor),
                 static_cast<int>(referentMarked));

    Heap::GetHeap().RemoveExportObject(rootHandle);
    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;
    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    threadPool.Exit();
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);

    GC_EXPECT_TRUE(recordedBeforeMinor);
    GC_EXPECT_FALSE(referentMarked);
    (void)holderLive;
    (void)targetLive;
}

enum class MajorRootFamily {
    COMMON,
    EXPORT,
};

void RunMajorWeakGraph(MajorRootFamily family)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    WeakGraph graph(fx, fx.region0);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    GCThreadPool threadPool("gc-unit-major-weak", 0, GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region0);
    space.GetRegionManager().AddRawPointerObject(graph.child);

    RootSlot commonRoot;
    RootSlot* commonRoots[] = { &commonRoot };
    U64 exportHandle = 0;
    if (family == MajorRootFamily::COMMON) {
        StorePlain(commonRoot, from_object(graph.strongRoot));
        Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(commonRoots), 1);
    } else {
        // Keep the export DFS cut independent from TracingImpl's root-family
        // admission cut.  This sentinel has no edge into the weak graph; the
        // graph itself remains reachable only through the export root.
        BaseObject* commonSentinel = fx.PlaceObject(fx.region0->GetRegionStart() + 320);
        WeakGraph::Field(commonSentinel).StoreColoured(zpointer::null);
        fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(commonSentinel) + 64);
        StorePlain(commonRoot, from_object(commonSentinel));
        Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(commonRoots), 1);
        exportHandle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);
    }

    ResetWeakDiscoveryTestReceipt();
    RelocationReceiptTestAccess::RunMajorMark(collector);
    const WeakDiscoveryTestReceipt receipt = ReadWeakDiscoveryTestReceipt();
    const bool referentCleared = is_null(WeakGraph::Field(graph.weak).GetFieldValue());
    const bool strongMarked = graph.IsMarked(graph.strongRoot);
    const bool weakMarked = graph.IsMarked(graph.weak);
    const bool referentMarked = graph.IsMarked(graph.referent);
    const bool childMarked = graph.IsMarked(graph.child);
    std::fprintf(stderr,
                 "DETAIL major_weak family=%s discovered=%zu strong_mark=%d weak_mark=%d "
                 "referent_mark=%d child_mark=%d referent_cleared=%d\n",
                 family == MajorRootFamily::COMMON ? "common" : "export", receipt.discovered,
                 static_cast<int>(strongMarked), static_cast<int>(weakMarked), static_cast<int>(referentMarked),
                 static_cast<int>(childMarked), static_cast<int>(referentCleared));

    Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(commonRoots), 1);
    if (family == MajorRootFamily::EXPORT) {
        Heap::GetHeap().RemoveExportObject(exportHandle);
    }
    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    threadPool.Exit();
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);

    GC_EXPECT_EQ(receipt.discovered, 1u);
    GC_EXPECT_TRUE(strongMarked);
    GC_EXPECT_TRUE(weakMarked);
    GC_EXPECT_FALSE(referentMarked);
    GC_EXPECT_FALSE(childMarked);
    GC_EXPECT_TRUE(referentCleared);
}

} // namespace

GC_OTHER_VM_TEST(YoungWeakClosure, SerialDiscoversWithoutStrongReferentClosure)
{
    RunYoungWeakVariant("serial", 0, 1, 0, 0);
}

GC_OTHER_VM_TEST(YoungWeakClosure, LegacyParallelDiscoversWithoutStrongReferentClosure)
{
    RunYoungWeakVariant("legacy-parallel", 1, 0, 1, 0);
}

GC_OTHER_VM_TEST(YoungWeakClosure, StripedDiscoversWithoutStrongReferentClosure)
{
    RunYoungWeakVariant("striped", 1, 0, 0, 1);
}

GC_OTHER_VM_TEST(YoungWeakClosure, WeakRemsetSlotFlowsFromClosureToConsumer)
{
    RunYoungWeakRemsetFlow();
}

GC_OTHER_VM_TEST(YoungWeakClosure, CommonMajorRootUsesWeakDiscoveryPolicy)
{
    RunMajorWeakGraph(MajorRootFamily::COMMON);
}

GC_OTHER_VM_TEST(YoungWeakClosure, ExportMajorRootUsesWeakDiscoveryPolicy)
{
    RunMajorWeakGraph(MajorRootFamily::EXPORT);
}

GC_OTHER_VM_TEST(YoungWeakClosure, ExportOnlyMajorRootOwnsItsClosure)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    WeakGraph graph(fx, fx.region0);
    // This grid is deliberately non-weak: the only edge must be followed by
    // the export root family even when the common root stack is empty.
    *reinterpret_cast<uintptr_t*>(graph.weak) = reinterpret_cast<uintptr_t>(fx.typeInfo);
    WeakGraph::Field(graph.weak).StoreColoured(zpointer::null);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    GCThreadPool threadPool("gc-unit-export-only", 0, GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region0);
    space.GetRegionManager().AddRawPointerObject(graph.weak);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);

    RelocationReceiptTestAccess::RunMajorMark(collector);
    const bool rootMarked = graph.IsMarked(graph.strongRoot);
    const bool childMarked = graph.IsMarked(graph.weak);
    std::fprintf(stderr,
                 "DETAIL export_only common_roots=0 foreign_roots=1 root_mark=%d child_mark=%d\n",
                 static_cast<int>(rootMarked), static_cast<int>(childMarked));

    Heap::GetHeap().RemoveExportObject(handle);
    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    threadPool.Exit();
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    GC_EXPECT_TRUE(rootMarked);
    GC_EXPECT_TRUE(childMarked);
}

#endif // MRT_TESTABLE_INTERNALS
