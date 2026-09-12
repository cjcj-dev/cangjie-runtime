// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

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
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/TracingCollector.h"
#include "Heap/GcThreadPool.h"
#include "Heap/Heap.h"
#include "Heap/WCollector/WCollector.h"
#include "Heap/Verify/VerifyMarkingStacks.h"
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

    static void BindThreadPool(CollectorResources& resources, GCThreadPool* threadPool, int32_t threadCount = 1)
    {
        resources.gcThreadPool = threadPool;
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
    route.source->AddLiveByteCount(route.from->GetSize());
    route.source->PrepareForwardableRegion(route.source->GetMarkView<Generation::Old>());
    route.source->RecordRouteStart(sourceOffset);
    route.source->SetRouteState(RegionInfo::RouteState::FORWARDED);
    route.from->SetStateCode(ObjectState::FORWARDED);
    ForwardingTable::Publication publication = ForwardingTable::EnsurePublicationBeforeCopy(
        route.source, reinterpret_cast<MAddress>(route.from));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(
                     publication, reinterpret_cast<MAddress>(route.from),
                     reinterpret_cast<MAddress>(route.to)),
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

uint64_t MarkingBoundaryDelta(const VerifyMarkingStacks::Snapshot& before,
                              const VerifyMarkingStacks::Snapshot& after,
                              VerifyMarkingStacks::MarkingGeneration generation,
                              VerifyMarkingStacks::MarkingBoundary boundary,
                              VerifyMarkingStacks::MarkingContainer container)
{
    return after.BoundaryCount(generation, boundary, container) -
           before.BoundaryCount(generation, boundary, container);
}

void RunYoungWeakVariant(const char* variant, size_t helpers,
                         uint64_t expectedSerial, uint64_t expectedLegacyParallel, uint64_t expectedStriped)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_VERIFY_MARKING", "1", 1), 0);
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
    const VerifyMarkingStacks::Snapshot markingBefore = VerifyMarkingStacks::ReadSnapshot();

    RelocationReceiptTestAccess::RunYoungCollection(collector);
    const VerifyMarkingStacks::Snapshot markingAfter = VerifyMarkingStacks::ReadSnapshot();
    const YoungWeakClosureTestReceipt receipt = ReadYoungWeakClosureTestReceipt();
    const bool strongMarked = graph.IsMarked(graph.strongRoot);
    const bool weakMarked = graph.IsMarked(graph.weak);
    const bool referentMarked = graph.IsMarked(graph.referent);
    const bool childMarked = graph.IsMarked(graph.child);
    using VerifyMarkingStacks::MarkingBoundary;
    using VerifyMarkingStacks::MarkingContainer;
    using VerifyMarkingStacks::MarkingGeneration;
    const auto delta = [&markingBefore, &markingAfter](MarkingBoundary boundary, MarkingContainer container) {
        return MarkingBoundaryDelta(markingBefore, markingAfter, MarkingGeneration::YOUNG, boundary, container);
    };
    const size_t ownerProducer = markingAfter.ProducerMax(MarkingGeneration::YOUNG, MarkingContainer::OWNER);
    const size_t taskProducer = markingAfter.ProducerMax(MarkingGeneration::YOUNG, MarkingContainer::TASK);
    const size_t localProducer = markingAfter.ProducerMax(MarkingGeneration::YOUNG, MarkingContainer::LOCAL);
    const size_t stripeProducer = markingAfter.ProducerMax(MarkingGeneration::YOUNG, MarkingContainer::STRIPE);
    std::fprintf(stderr,
                 "DETAIL young_weak variant=%s serial=%zu legacy_parallel=%zu striped=%zu "
                 "strong_mark=%d weak_mark=%d referent_mark=%d child_mark=%d\n",
                 variant, static_cast<size_t>(receipt.serial), static_cast<size_t>(receipt.legacyParallel),
                 static_cast<size_t>(receipt.striped), static_cast<int>(strongMarked),
                 static_cast<int>(weakMarked), static_cast<int>(referentMarked), static_cast<int>(childMarked));
    std::fprintf(stderr,
                 "DETAIL marking_stack_young variant=%s owner_producer=%zu task_producer=%zu local_producer=%zu "
                 "stripe_producer=%zu start_owner=%llu start_pool=%llu task_exit_task=%llu "
                 "seed_local=%llu termination_stripe=%llu worker_exit_local=%llu join_stripe=%llu "
                 "join_pool=%llu end_owner=%llu end_pool=%llu\n",
                 variant, ownerProducer, taskProducer, localProducer, stripeProducer,
                 static_cast<unsigned long long>(delta(MarkingBoundary::START, MarkingContainer::OWNER)),
                 static_cast<unsigned long long>(delta(MarkingBoundary::START, MarkingContainer::POOL)),
                 static_cast<unsigned long long>(delta(MarkingBoundary::TASK_EXIT, MarkingContainer::TASK)),
                 static_cast<unsigned long long>(delta(MarkingBoundary::SEED_PUBLISH, MarkingContainer::LOCAL)),
                 static_cast<unsigned long long>(delta(MarkingBoundary::TERMINATION, MarkingContainer::STRIPE)),
                 static_cast<unsigned long long>(delta(MarkingBoundary::WORKER_EXIT, MarkingContainer::LOCAL)),
                 static_cast<unsigned long long>(delta(MarkingBoundary::JOIN, MarkingContainer::STRIPE)),
                 static_cast<unsigned long long>(delta(MarkingBoundary::JOIN, MarkingContainer::POOL)),
                 static_cast<unsigned long long>(delta(MarkingBoundary::END, MarkingContainer::OWNER)),
                 static_cast<unsigned long long>(delta(MarkingBoundary::END, MarkingContainer::POOL)));

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
    GC_EXPECT_TRUE(ownerProducer > 0);
    GC_EXPECT_TRUE(delta(MarkingBoundary::START, MarkingContainer::OWNER) > 0);
    GC_EXPECT_TRUE(delta(MarkingBoundary::START, MarkingContainer::POOL) > 0);
    GC_EXPECT_TRUE(delta(MarkingBoundary::END, MarkingContainer::OWNER) > 0);
    GC_EXPECT_TRUE(delta(MarkingBoundary::END, MarkingContainer::POOL) > 0);
    if (std::strcmp(variant, "legacy-parallel") == 0) {
        GC_EXPECT_TRUE(taskProducer > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::TASK_EXIT, MarkingContainer::TASK) > 0);
    }
    if (std::strcmp(variant, "striped") == 0) {
        GC_EXPECT_TRUE(localProducer > 0);
        GC_EXPECT_TRUE(stripeProducer > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::START, MarkingContainer::LOCAL) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::START, MarkingContainer::STRIPE) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::SEED_PUBLISH, MarkingContainer::LOCAL) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::TERMINATION, MarkingContainer::STRIPE) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::WORKER_EXIT, MarkingContainer::LOCAL) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::JOIN, MarkingContainer::STRIPE) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::JOIN, MarkingContainer::POOL) > 0);
    }
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

void RunMajorWeakGraph(MajorRootFamily family, bool runtimeEntry = false, size_t helpers = 0)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    if (runtimeEntry) {
        GC_EXPECT_EQ(setenv("MRT_GCV2_VERIFY_MARKING", "1", 1), 0);
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
    GCThreadPool threadPool("gc-unit-major-weak", static_cast<int32_t>(helpers), GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool, static_cast<int32_t>(helpers + 1));
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
    std::unique_ptr<RootSlot[]> commonRootStorage = std::make_unique<RootSlot[]>(kParallelRootCount);
    std::vector<RootSlot*> commonRoots(kParallelRootCount);
    for (size_t i = 0; i < commonRoots.size(); ++i) {
        commonRoots[i] = &commonRootStorage[i];
    }
    const U32 commonRootCount = runtimeEntry && helpers != 0 ? static_cast<U32>(commonRoots.size()) : 1;
    U32 registeredCommonRootCount = 0;
    U64 exportHandle = 0;
    if (family == MajorRootFamily::COMMON) {
        for (U32 i = 0; i < commonRootCount; ++i) {
            StorePlain(*commonRoots[i], from_object(graph.strongRoot));
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
            StorePlain(*commonRoots[0], from_object(commonSentinel));
            Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(commonRoots.data()), 1);
            registeredCommonRootCount = 1;
        }
        exportHandle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);
    }

    ResetWeakDiscoveryTestReceipt();
    const VerifyMarkingStacks::Snapshot markingBefore = VerifyMarkingStacks::ReadSnapshot();
    if (runtimeEntry) {
        RelocationReceiptTestAccess::RunMajorCollection(collector);
    } else {
        RelocationReceiptTestAccess::RunMajorMark(collector);
    }
    const VerifyMarkingStacks::Snapshot markingAfter = VerifyMarkingStacks::ReadSnapshot();
    using VerifyMarkingStacks::MarkingBoundary;
    using VerifyMarkingStacks::MarkingContainer;
    using VerifyMarkingStacks::MarkingGeneration;
    const auto delta = [&markingBefore, &markingAfter](MarkingBoundary boundary, MarkingContainer container) {
        return MarkingBoundaryDelta(markingBefore, markingAfter, MarkingGeneration::MAJOR, boundary, container);
    };
    const size_t ownerProducer = markingAfter.ProducerMax(MarkingGeneration::MAJOR, MarkingContainer::OWNER);
    const size_t foreignProducer = markingAfter.ProducerMax(MarkingGeneration::MAJOR, MarkingContainer::FOREIGN);
    const size_t taskProducer = markingAfter.ProducerMax(MarkingGeneration::MAJOR, MarkingContainer::TASK);
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
    if (runtimeEntry) {
        std::fprintf(stderr,
                     "DETAIL marking_stack_major mode=%s family=%s owner_producer=%zu foreign_producer=%zu "
                     "task_producer=%zu start_owner=%llu start_foreign=%llu start_pool=%llu "
                     "task_exit_task=%llu termination_owner=%llu termination_pool=%llu join_pool=%llu "
                     "end_owner=%llu end_foreign=%llu end_pool=%llu\n",
                     helpers == 0 ? "serial" : "parallel", family == MajorRootFamily::COMMON ? "common" : "export",
                     ownerProducer, foreignProducer, taskProducer,
                     static_cast<unsigned long long>(delta(MarkingBoundary::START, MarkingContainer::OWNER)),
                     static_cast<unsigned long long>(delta(MarkingBoundary::START, MarkingContainer::FOREIGN)),
                     static_cast<unsigned long long>(delta(MarkingBoundary::START, MarkingContainer::POOL)),
                     static_cast<unsigned long long>(delta(MarkingBoundary::TASK_EXIT, MarkingContainer::TASK)),
                     static_cast<unsigned long long>(delta(MarkingBoundary::TERMINATION, MarkingContainer::OWNER)),
                     static_cast<unsigned long long>(delta(MarkingBoundary::TERMINATION, MarkingContainer::POOL)),
                     static_cast<unsigned long long>(delta(MarkingBoundary::JOIN, MarkingContainer::POOL)),
                     static_cast<unsigned long long>(delta(MarkingBoundary::END, MarkingContainer::OWNER)),
                     static_cast<unsigned long long>(delta(MarkingBoundary::END, MarkingContainer::FOREIGN)),
                     static_cast<unsigned long long>(delta(MarkingBoundary::END, MarkingContainer::POOL)));
        CHECK_DETAIL(family != MajorRootFamily::EXPORT || foreignProducer != 0,
                     "major foreign producer receipt missing after DoGarbageCollection entry");
        CHECK_DETAIL(delta(MarkingBoundary::END, MarkingContainer::OWNER) != 0 &&
                         delta(MarkingBoundary::END, MarkingContainer::POOL) != 0 &&
                         (family != MajorRootFamily::EXPORT ||
                          delta(MarkingBoundary::END, MarkingContainer::FOREIGN) != 0),
                     "major marking scene receipt missing after DoGarbageCollection entry family=%s",
                     family == MajorRootFamily::COMMON ? "common" : "export");
    }

    if (registeredCommonRootCount != 0) {
        Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(commonRoots.data()), registeredCommonRootCount);
    }
    if (family == MajorRootFamily::EXPORT) {
        Heap::GetHeap().RemoveExportObject(exportHandle);
    }
    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    threadPool.Exit();
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);

    if (runtimeEntry) {
        GC_EXPECT_TRUE(delta(MarkingBoundary::START, MarkingContainer::OWNER) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::START, MarkingContainer::POOL) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::TASK_EXIT, MarkingContainer::TASK) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::TERMINATION, MarkingContainer::OWNER) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::TERMINATION, MarkingContainer::POOL) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::JOIN, MarkingContainer::POOL) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::END, MarkingContainer::OWNER) > 0);
        GC_EXPECT_TRUE(delta(MarkingBoundary::END, MarkingContainer::POOL) > 0);
        GC_EXPECT_TRUE(taskProducer > 0);
        if (family == MajorRootFamily::COMMON) {
            GC_EXPECT_TRUE(ownerProducer > 0);
        } else {
            GC_EXPECT_TRUE(foreignProducer > 0);
            GC_EXPECT_TRUE(delta(MarkingBoundary::START, MarkingContainer::FOREIGN) > 0);
            GC_EXPECT_TRUE(delta(MarkingBoundary::END, MarkingContainer::FOREIGN) > 0);
        }
        if (helpers == 0) {
            GC_EXPECT_TRUE(delta(MarkingBoundary::TASK_EXIT, MarkingContainer::TASK) >= 1u);
        } else {
            GC_EXPECT_TRUE(delta(MarkingBoundary::TASK_EXIT, MarkingContainer::TASK) > 1);
        }
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

GC_OTHER_VM_TEST(VerifyMarkingStacksProduct, MajorSerialEntersFromDoGarbageCollection)
{
    RunMajorWeakGraph(MajorRootFamily::COMMON, true, 0);
}

GC_OTHER_VM_TEST(VerifyMarkingStacksProduct, MajorParallelEntersFromDoGarbageCollection)
{
    RunMajorWeakGraph(MajorRootFamily::COMMON, true, 1);
}

GC_OTHER_VM_TEST(VerifyMarkingStacksProduct, MajorForeignEntersFromDoGarbageCollection)
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
    GCThreadPool threadPool("gc-unit-value-root-minor", 0, GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool);
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
    ForwardingTable::PublishMarkCoverage(Generation::Old);
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
    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    threadPool.Exit();
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
    GCThreadPool threadPool("gc-unit-value-root-major", 0, GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool);
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
    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    threadPool.Exit();
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    GC_EXPECT_TRUE(producerCarrier);
    GC_EXPECT_TRUE(rootMarked);
    // This is the target invariant: only the real FindUselessExternObjects
    // consumer paints the foreign value emitted by DFSTraceExportObject.
    GC_EXPECT_TRUE(consumerMarked);
    GC_EXPECT_TRUE(handoffCurrent);
}

#endif // MRT_TESTABLE_INTERNALS
