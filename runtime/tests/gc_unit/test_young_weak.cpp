// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "Common/Runtime.h"
#include "CjScheduler.h"

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Concurrency/Concurrency.h"
#include "Base/Log.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zBarrier.hpp"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/WCollector/WCollector.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/ThreadLocal.h"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zVerify.hpp"
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

    static void BindRuntimeWorkers(CollectorResources& resources, RuntimeWorkers* threadPool, int32_t threadCount = 1)
    {
        resources.runtimeWorkers = threadPool;
        resources.gcThreadCount = threadCount;
        resources.concurrentGcThreadCount = threadCount;
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

    static void RunPostTrace(WCollector& collector) { collector.PostTrace(); }

    static void SeedValueRoots(WCollector& collector, BaseObject* value)
    {
        {
            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
            collector.resurrectedExportObjectes.clear();
            collector.resurrectedExportObjectesForwardPhase.clear();
            collector.resurrectedExportObjectes.insert(value);
            collector.resurrectedExportObjectesForwardPhase.insert(value);
        }
        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
        collector.cycleRefWorkStack.clear();
        collector.discoveredExternObjects.clear();
        collector.cycleRefWorkStack[value].push_back(value);
    }

    static bool AllValueRootsEqual(WCollector& collector, BaseObject* value)
    {
        {
            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
            if (collector.resurrectedExportObjectes.size() != 1 ||
                collector.resurrectedExportObjectes.count(value) != 1 ||
                collector.resurrectedExportObjectesForwardPhase.size() != 1 ||
                collector.resurrectedExportObjectesForwardPhase.count(value) != 1) {
                return false;
            }
        }
        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
        auto it = collector.cycleRefWorkStack.find(value);
        return collector.cycleRefWorkStack.size() == 1 && it != collector.cycleRefWorkStack.end() &&
            it->second.size() == 1 && it->second.front() == value;
    }

    static bool CycleHandoffEquals(WCollector& collector, BaseObject* key, BaseObject* value)
    {
        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
        auto it = collector.cycleRefWorkStack.find(key);
        return collector.discoveredExternObjects.empty() && collector.cycleRefWorkStack.size() == 1 &&
            it != collector.cycleRefWorkStack.end() && it->second.size() == 1 && it->second.front() == value;
    }

    static bool DiscoveredCarrierEquals(WCollector& collector, BaseObject* key, BaseObject* value)
    {
        std::lock_guard<std::mutex> lock(collector.externMtx);
        auto it = collector.discoveredExternObjects.find(key);
        return collector.discoveredExternObjects.size() == 1 &&
            it != collector.discoveredExternObjects.end() && it->second.size() == 1 &&
            it->second.front() == value;
    }

    static bool MinorFinishedValueRootsEqual(WCollector& collector, BaseObject* value)
    {
        {
            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
            if (collector.resurrectedExportObjectes.size() != 1 ||
                collector.resurrectedExportObjectes.count(value) != 1 ||
                !collector.resurrectedExportObjectesForwardPhase.empty()) {
                return false;
            }
        }
        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
        auto it = collector.cycleRefWorkStack.find(value);
        return collector.cycleRefWorkStack.size() == 1 && it != collector.cycleRefWorkStack.end() &&
            it->second.size() == 1 && it->second.front() == value;
    }

    static void RunMajorCollection(WCollector& collector)
    {
        collector.SetGCReason(GC_REASON_USER);
        collector.DoGarbageCollection();
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

struct ValueRootRoute {
    RegionInfo* source = nullptr;
    RegionInfo* destination = nullptr;
    BaseObject* from = nullptr;
    BaseObject* to = nullptr;
    LiveInfo* sourceLive = nullptr;
    LiveInfo* destinationLive = nullptr;
};

ValueRootRoute PrepareValueRootRoute(GcHeapFixture& fx, bool destinationYoung)
{
    ValueRootRoute route;
    route.source = fx.region0;
    route.destination = fx.region1;
    route.from = fx.PlaceObject(route.source->GetRegionStart());
    route.to = fx.PlaceObject(route.destination->GetRegionStart());
    route.source->SetRegionAllocPtr(reinterpret_cast<MAddress>(route.from) + 64);
    route.destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(route.to) + 64);
    route.source->SetYoungRegionFlag(0);
    route.destination->SetYoungRegionFlag(destinationYoung ? 1 : 0);
    if (destinationYoung) {
        route.destination->SetYoungAge(1);
    }

    route.source->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    route.sourceLive = fx.PlantLiveInfo(route.source);
    RegionBitmap* sourceBitmap =
        fx.PlantMarkBitmap<Generation::Old>(route.sourceLive, route.source->GetRegionSize());
    const size_t sourceOffset = route.source->GetAddressOffset(reinterpret_cast<MAddress>(route.from));
    (void)sourceBitmap->MarkBits(sourceOffset, route.from->GetSize(), route.source->GetRegionSize());
    route.source->PrepareForwardableRegion(route.source->GetMarkView<Generation::Old>());
    route.from->SetStateCode(ObjectState::FORWARDED);
    ForwardingTable::Publication publication = ForwardingTable::EnsurePublicationBeforeCopy(
        route.source, reinterpret_cast<MAddress>(route.from));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(
                     publication, reinterpret_cast<MAddress>(route.from),
                     reinterpret_cast<MAddress>(route.to)),
                 reinterpret_cast<MAddress>(route.to));
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(route.from)),
                 reinterpret_cast<MAddress>(route.to));

    route.destinationLive = fx.PlantLiveInfo(route.destination);
    (void)fx.PlantMarkBitmap<Generation::Young>(route.destinationLive,
                                                route.destination->GetRegionSize());
    return route;
}

bool IsValueRootMarked(const ValueRootRoute& route)
{
    return route.destination->IsYoungRegion()
        ? route.destination->IsMarkedObject(route.destination->GetMarkView<Generation::Young>(), route.to)
        : route.destination->IsMarkedObject(route.destination->GetMarkView<Generation::Old>(), route.to);
}

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

struct ExportForeignGraph {
    explicit ExportForeignGraph(GcHeapFixture& fixture) : fx(fixture), owner(fixture.region0)
    {
        std::memset(foreignTypeStorage, 0, sizeof(foreignTypeStorage));
        foreignType = reinterpret_cast<TypeInfo*>(foreignTypeStorage);
        foreignType->SetType(TypeKind::TYPE_KIND_FOREIGN_PROXY);
        foreignType->SetFlagHasRefField();
        foreignType->SetInstanceSize(sizeof(void*));
        GCTib gctib {};
        gctib.tag = SIGN_BIT | 1;
        foreignType->SetGCTib(gctib);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(foreignTypeStorage), sizeof(foreignTypeStorage));

        root = fx.PlaceObject(owner->GetRegionStart() + 64);
        foreign = fx.PlaceObject(owner->GetRegionStart() + 128);
        *reinterpret_cast<uintptr_t*>(foreign) = reinterpret_cast<uintptr_t>(foreignType);
        owner->SetRegionAllocPtr(reinterpret_cast<MAddress>(foreign) + 64);
        WeakGraph::Field(root).StoreColoured(GcUnit::StoreGoodPointer(foreign));
        WeakGraph::Field(foreign).StoreColoured(zpointer::null);
    }

    bool IsMarked(BaseObject* object) const
    {
        return owner->IsMarkedObject(owner->GetMarkView<Generation::Old>(), object);
    }

    GcHeapFixture& fx;
    RegionInfo* owner;
    BaseObject* root = nullptr;
    BaseObject* foreign = nullptr;
    alignas(TypeInfo) unsigned char foreignTypeStorage[sizeof(TypeInfo)];
    TypeInfo* foreignType = nullptr;
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
    RuntimeWorkers threadPool(helpers + 1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
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
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
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
    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
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
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
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

void RunMajorWeakGraph(MajorRootFamily family, bool runtimeEntry = false, size_t helpers = 0)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    if (runtimeEntry) {
    }
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    WeakGraph graph(fx, fx.region0);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RuntimeWorkers threadPool(helpers + 1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool, static_cast<int32_t>(helpers + 1));
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region0);
    space.GetRegionManager().AddRawPointerObject(graph.child);
    if (runtimeEntry) {
        Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
        space.GetRegionManager().AddRawPointerObject(graph.strongRoot);
        space.GetRegionManager().AddRawPointerObject(graph.weak);
        space.GetRegionManager().AddRawPointerObject(graph.referent);
    }

    // MarkStack::size() counts 64-entry buffers. Seventeen buffers cross the
    // product's MAX_MARKING_WORK_SIZE=16 parallel admission threshold.
    constexpr size_t kParallelRootCount = 17 * 64;
    std::vector<NativeSlot> commonRootStorage(kParallelRootCount, NativeSlot(zpointer::null));
    std::vector<NativeSlot*> commonRoots(kParallelRootCount);
    for (size_t i = 0; i < commonRoots.size(); ++i) {
        commonRoots[i] = &commonRootStorage[i];
    }
    const U32 commonRootCount = runtimeEntry && helpers != 0 ? static_cast<U32>(commonRoots.size()) : 1;
    U32 registeredCommonRootCount = 0;
    U64 exportHandle = 0;
    if (family == MajorRootFamily::COMMON) {
        for (U32 i = 0; i < commonRootCount; ++i) {
            commonRoots[i]->StoreColoured(StoreGoodPointer(graph.strongRoot));
        }
        Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(commonRoots.data()), commonRootCount);
        registeredCommonRootCount = commonRootCount;
    } else {
        if (!runtimeEntry) {
            // Keep the export DFS cut independent from TracingImpl's root-family
            // admission cut. This sentinel has no edge into the weak graph; the
            // graph itself remains reachable only through the export root.
            BaseObject* commonSentinel = fx.PlaceObject(fx.region0->GetRegionStart() + 320);
            WeakGraph::Field(commonSentinel).StoreColoured(zpointer::null);
            fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(commonSentinel) + 64);
            commonRoots[0]->StoreColoured(StoreGoodPointer(commonSentinel));
            Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(commonRoots.data()), 1);
            registeredCommonRootCount = 1;
        }
        exportHandle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);
    }

    ResetWeakDiscoveryTestReceipt();
    if (runtimeEntry) {
        RelocationReceiptTestAccess::RunMajorCollection(collector);
    } else {
        RelocationReceiptTestAccess::RunMajorMark(collector);
    }
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
    if (registeredCommonRootCount != 0) {
        Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(commonRoots.data()), registeredCommonRootCount);
    }
    if (family == MajorRootFamily::EXPORT) {
        Heap::GetHeap().RemoveExportObject(exportHandle);
    }
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);

    if (runtimeEntry) {
        // The receipt matrix was removed with the old verifier. Observe actual
        // collection completion and reference processing instead.
        GC_EXPECT_FALSE(collector.GetCycleSnapshot(GCCycleGeneration::OLD).active);
        GC_EXPECT_TRUE(referentCleared);
        return;
    }
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

// zMark.cpp:1016-1028: private stacks must be checked independently of
// shared stripes and only for the generation completing marking.
GC_OTHER_VM_TEST(MarkingStacksProduct, MarkEndChecksPrivateStacksByGeneration)
{
    if (!ZVerifyMarking) {
        GC_EXPECT_EQ(setenv("ZVerifyMarking", "1", 1), 0);
        RunInOtherVm("MarkingStacksProduct.MarkEndChecksPrivateStacksByGeneration");
        return;
    }
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    WeakClosureTestRuntime runtime(manager);
    MarkDomain old(64, MarkingStacks::MarkingGeneration::MAJOR);
    MarkDomain young(64, MarkingStacks::MarkingGeneration::YOUNG);
    for (MarkDomain* domain : {&old, &young}) {
        MarkDomain& current = *domain;
        MarkDomain& other = domain == &old ? young : old;
        auto& stacks = ThreadLocal::GetMarkStacks(current);
        stacks.Push(current.Stripes(), 0,
                    MarkStackEntry::MarkAndFollow(reinterpret_cast<BaseObject*>(0x1000)), true);
        GC_EXPECT_TRUE(current.Stripes().IsEmpty());
        GC_EXPECT_FALSE(stacks.IsEmpty());
        {
            ScopedStopTheWorld stw("mark stacks verification test", false);
            MarkingStacks::VerifyAllEmpty(other);
            const pid_t child = fork();
            GC_EXPECT_TRUE(child >= 0);
            if (child == 0) {
                signal(SIGABRT, SIG_DFL);
                MarkingStacks::VerifyAllEmpty(current);
                _exit(0);
            }
            int status = 0;
            GC_EXPECT_EQ(waitpid(child, &status, 0), child);
            GC_EXPECT_TRUE(WIFSIGNALED(status));
            GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
            // Verification of the other generation has not flushed this stack.
            GC_EXPECT_FALSE(stacks.IsEmpty());
            GC_EXPECT_TRUE(current.Stripes().IsEmpty());
            GC_EXPECT_TRUE(stacks.Flush(current.Stripes(), true));
            MarkingSMR smr(1);
            MarkStripeStack* published = current.Stripes().At(0).StealStack(smr, 0);
            GC_EXPECT_TRUE(published != nullptr);
            MarkStripeStack::Destroy(published);
            smr.Reclaim(0);
            MarkingStacks::VerifyAllEmpty(current);
        }
    }
}

GC_OTHER_VM_TEST(MarkingStacksProduct, MajorSerialEntersFromDoGarbageCollection)
{
    RunMajorWeakGraph(MajorRootFamily::COMMON, true, 0);
}

GC_OTHER_VM_TEST(MarkingStacksProduct, MajorParallelEntersFromDoGarbageCollection)
{
    RunMajorWeakGraph(MajorRootFamily::COMMON, true, 1);
}

GC_OTHER_VM_TEST(MarkingStacksProduct, MajorForeignEntersFromDoGarbageCollection)
{
    RunMajorWeakGraph(MajorRootFamily::EXPORT, true, 0);
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
    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
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
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    GC_EXPECT_TRUE(rootMarked);
    GC_EXPECT_TRUE(childMarked);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MinorRuntimeDispatchMarksCurrentAndWritesBack)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_MARKPAR_FORCE_SERIAL", "1", 1), 0);
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    ValueRootRoute route = PrepareValueRootRoute(fx, true);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(route.destination);
    space.GetRegionManager().AddRawPointerObject(route.to);
    Heap::GetHeap().GetRememberedSet().Initialize(
        fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    RelocationReceiptTestAccess::SeedValueRoots(collector, route.from);

    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;
    RelocationReceiptTestAccess::RunYoungCollection(collector);

    const bool currentMarked = IsValueRootMarked(route);
    const bool carrierCurrent =
        RelocationReceiptTestAccess::MinorFinishedValueRootsEqual(collector, route.to);
    Heap::GetHeap().GetCollector().PublishGenerationPhase(GCCycleGeneration::OLD, GC_PHASE_MARK_COMPLETE);
    ForwardingTable::ReclaimRetired("value-root-minor-runtime-dispatch");
    const auto afterCoverage = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(route.from));
    const bool independentAfterCoverage =
        RelocationReceiptTestAccess::MinorFinishedValueRootsEqual(collector, route.to);
    std::fprintf(stderr,
                 "VALUE_ROOT_RUNTIME_ASSERT minor current_marked=%d carrier_current=%d "
                 "after_coverage=%d lookup=%u\n",
                 static_cast<int>(currentMarked), static_cast<int>(carrierCurrent),
                 static_cast<int>(independentAfterCoverage),
                 static_cast<unsigned>(afterCoverage.answer));

    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    GC_EXPECT_TRUE(currentMarked);
    GC_EXPECT_TRUE(carrierCurrent);
    GC_EXPECT_TRUE(independentAfterCoverage);
    (void)route;
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MajorProducerConsumerCurrentizesBeforeMark)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    ExportForeignGraph graph(fx);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(graph.owner);
    space.GetRegionManager().AddRawPointerObject(graph.foreign);
    const U64 exportHandle = Heap::GetHeap().RegisterExportRoot(graph.root);

    RelocationReceiptTestAccess::RunMajorMark(collector);
    const bool producerCarrier =
        RelocationReceiptTestAccess::DiscoveredCarrierEquals(collector, graph.root, graph.foreign);
    const bool rootMarked = graph.IsMarked(graph.root);
    const bool consumerMarked = graph.IsMarked(graph.foreign);
    RelocationReceiptTestAccess::RunPostTrace(collector);
    const bool handoffCurrent =
        RelocationReceiptTestAccess::CycleHandoffEquals(collector, graph.root, graph.foreign);
    std::fprintf(stderr,
                 "VALUE_ROOT_RUNTIME_ASSERT major producer_carrier=%d root_marked=%d "
                 "consumer_marked=%d handoff_current=%d\n",
                 static_cast<int>(producerCarrier), static_cast<int>(rootMarked),
                 static_cast<int>(consumerMarked), static_cast<int>(handoffCurrent));

    Heap::GetHeap().RemoveExportObject(exportHandle);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    GC_EXPECT_TRUE(producerCarrier);
    GC_EXPECT_TRUE(rootMarked);
    // This is the target invariant: only the real FindUselessExternObjects
    // consumer paints the foreign value recorded by the export ABI view.
    GC_EXPECT_TRUE(consumerMarked);
    GC_EXPECT_TRUE(handoffCurrent);
}


// Directed port test for zHeapIterator.cpp:195-229 (no upstream standalone graph test):
// W --weak--> R --strong--> C. The public iterator must report R itself.
GC_OTHER_VM_TEST(HeapIterator, StrongAndWeakInclusiveGraphs)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    WeakClosureTestRuntime runtime(manager);
    GcHeapFixture fx;
    WeakGraph graph(fx, fx.region0);
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GC_PHASE_IDLE);
    NativeSlot root(StoreGoodPointer(graph.weak));
    NativeSlot* roots[] = { &root };
    Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::unordered_set<BaseObject*> strong;
    std::unordered_set<BaseObject*> inclusive;
    {
        ScopedStopTheWorld stw("heap iterator test", false);
        HeapIterator(false).Iterate([&](BaseObject* object) { strong.insert(object); });
        HeapIterator(true).Iterate([&](BaseObject* object) { inclusive.insert(object); });
        const void* edge = nullptr;
        bool visitedReferent = false;
        HeapIterator(true, true).Iterate([&](BaseObject* object) {
            if (object == graph.referent) {
                GC_EXPECT_TRUE(edge == &WeakGraph::Field(graph.weak));
                visitedReferent = true;
            }
        }, [&](BaseObject*, const void* slot, uintptr_t) { edge = slot; });
        GC_EXPECT_TRUE(visitedReferent);
    }
    Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    GC_EXPECT_TRUE(strong.count(graph.weak) == 1);
    GC_EXPECT_TRUE(strong.count(graph.referent) == 0);
    GC_EXPECT_TRUE(strong.count(graph.child) == 0);
    GC_EXPECT_TRUE(inclusive.count(graph.weak) == 1);
    GC_EXPECT_TRUE(inclusive.count(graph.referent) == 1);
    GC_EXPECT_TRUE(inclusive.count(graph.child) == 1);
    // This allocated object is not a root. Inventory enumeration would include it.
    GC_EXPECT_TRUE(inclusive.count(graph.strongRoot) == 0);
}

GC_OTHER_VM_TEST(HeapIterator, WeakRootIsIncludedOnlyInWeakInclusiveMode)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    WeakClosureTestRuntime runtime(manager);
    GcHeapFixture fx;
    WeakGraph graph(fx, fx.region0);
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GC_PHASE_IDLE);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(graph.weak);
    std::unordered_set<BaseObject*> strong;
    std::unordered_set<BaseObject*> inclusive;
    {
        ScopedStopTheWorld stw("heap iterator weak root", false);
        HeapIterator(false).Iterate([&](BaseObject* object) { strong.insert(object); });
        HeapIterator(true).Iterate([&](BaseObject* object) { inclusive.insert(object); });
    }
    Heap::GetHeap().RemoveExportObject(handle);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    GC_EXPECT_TRUE(strong.count(graph.weak) == 0);
    GC_EXPECT_TRUE(inclusive.count(graph.weak) == 1);
    GC_EXPECT_TRUE(inclusive.count(graph.referent) == 1);
    GC_EXPECT_TRUE(inclusive.count(graph.child) == 1);
}

#endif // MRT_TESTABLE_INTERNALS
