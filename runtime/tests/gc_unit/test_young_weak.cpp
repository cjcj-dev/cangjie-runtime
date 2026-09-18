// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_worker_fixture.hpp"
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
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
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
#include "young_closure_observation.hpp"

extern "C" int CJ_ScheduleManagerInit();

namespace MapleRuntime {

struct RelocationReceiptTestAccess {
    static void BindCollector(HeapGcState* collector)
    {
        if (collector != nullptr) {
            CHECK(collector == &Heap::GetHeap().GetCollector());
            for (auto generation : {ZGenerationId::young, ZGenerationId::old}) {
                auto& cycle = collector->GetZGeneration(generation);
                if (cycle.Snapshot().active) cycle.End();
                if (cycle.Workers() == nullptr) cycle.InitializeWorkers(2);
            }
            collector->GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::MarkComplete);
            auto& remembered = HeapTestRemset();
            if (!remembered.IsInitialized()) {
                remembered.Initialize(Heap::GetHeapStartAddress(), GcHeapFixture::kUnits * ZPage::UNIT_SIZE);
            }
            InitFwdTables(Heap::GetHeapStartAddress(), GcHeapFixture::kUnits * ZPage::UNIT_SIZE,
                          ZPage::UNIT_SIZE);
        }
    }

    // zArguments: the concurrent worker budget the driver hands each request.
    static void BindWorkerBudget(int32_t threadCount = 1)
    {
        ZCollectedHeap::heap()->set_concurrent_gc_threads_for_test(threadCount);
    }

    static void RunYoungCollection(HeapGcState& collector)
    {
        auto& cycle = collector.GetZGeneration(ZGenerationId::young);
        if (!cycle.Snapshot().active) cycle.SelectReason(GC_REASON_YOUNG);
        YoungTypeSetter type(cycle, ZYoungType::minor);
        collector.DoGarbageCollection(ZGenerationId::young);
    }

    static void PrepareMajorRoots(HeapGcState& collector)
    {
        auto& young = collector.GetZGeneration(ZGenerationId::young);
        if (young.Workers() == nullptr) young.InitializeWorkers(1);
        auto& oldCycle = collector.GetZGeneration(ZGenerationId::old);
        if (oldCycle.Snapshot().active) oldCycle.End();
        oldCycle.SelectReason(GC_REASON_USER);
        auto& remembered = HeapTestRemset();
        if (!remembered.IsInitialized()) {
            remembered.Initialize(Heap::GetHeapStartAddress(), GcHeapFixture::kUnits * ZPage::UNIT_SIZE);
        }
        // ZDriver::gc_major runs the young roots collection before old marking.
        YoungTypeSetter type(young, ZYoungType::major_partial_roots);
        collector.RunGarbageCollection(1, GC_REASON_YOUNG);
    }

    static void RunMajorMark(HeapGcState& collector)
    {
        PrepareMajorRoots(collector);
        auto& cycle = collector.GetZGeneration(ZGenerationId::old);
        if (!cycle.Snapshot().active) cycle.SelectReason(GC_REASON_USER);
        if (!cycle.Snapshot().active) cycle.Begin(1);
        collector.StartOldMarkWork();

        auto& old = Heap::GetHeap().old();
        old.concurrent_mark();
        while (!old.pause_mark_end()) old.concurrent_mark_continue();
        old.process_non_strong_references();
    }

    static void RunPostTrace(HeapGcState& collector) { collector.PostTrace(); }

    static void RunExportMajorMark(HeapGcState& collector)
    {
        // The major-roots young prelude already began and prepared old marking
        // (zGeneration.cpp:118-124). Do not select/restart that active cycle.
        PrepareMajorRoots(collector);
        auto& old = Heap::GetHeap().old();
        old.concurrent_mark();
        while (!old.pause_mark_end()) old.concurrent_mark_continue();
        old.process_non_strong_references();
    }

    static void SeedValueRoots(HeapGcState& collector, BaseObject* value)
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

    static bool AllValueRootsEqual(HeapGcState& collector, BaseObject* value)
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

    static bool CycleHandoffEquals(HeapGcState& collector, BaseObject* key, BaseObject* value, size_t owners = 1)
    {
        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
        auto it = collector.cycleRefWorkStack.find(key);
        return collector.discoveredExternObjects.empty() && collector.cycleRefWorkStack.size() == owners &&
            it != collector.cycleRefWorkStack.end() && it->second.size() == 1 && it->second.front() == value;
    }

    static bool DiscoveredCarrierEquals(HeapGcState& collector, BaseObject* key, BaseObject* value, size_t owners = 1)
    {
        std::lock_guard<std::mutex> lock(collector.externMtx);
        auto it = collector.discoveredExternObjects.find(key);
        return collector.discoveredExternObjects.size() == owners &&
            it != collector.discoveredExternObjects.end() && it->second.size() == 1 &&
            it->second.front() == value;
    }

    static bool MinorFinishedValueRootsEqual(HeapGcState& collector, BaseObject* value)
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

    static void RunMajorCollection(HeapGcState& collector)
    {
        PrepareMajorRoots(collector);
        auto& cycle = collector.GetZGeneration(ZGenerationId::old);
        if (!cycle.Snapshot().active) cycle.SelectReason(GC_REASON_USER);
        collector.DoGarbageCollection(ZGenerationId::old);
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
        // CollectorResources::Init initializes statistics before any phase timer.
        ZStat::Initialize();
    }

    ~WeakClosureTestRuntime() override { runtime = nullptr; }

    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}

private:
    Concurrency concurrency;
};

// Observe the existing product closure boundary before promotion replaces its map.
// ZGenerationYoung completes marking before selecting/relocating pages.
class ValueRootMarkObservation {
public:
    explicit ValueRootMarkObservation(std::function<void()> observe) : observe(std::move(observe))
    {
        current = this;
        SetMarkClosureObserverForTest([](const std::vector<BaseObject*>*) {
            ++current->calls;
            current->observe();
        });
    }
    ~ValueRootMarkObservation()
    {
        SetMarkClosureObserverForTest(nullptr);
        current = nullptr;
    }
    size_t calls = 0;
private:
    std::function<void()> observe;
    inline static ValueRootMarkObservation* current = nullptr;
};

struct ValueRootRoute {
    ZPage* source = nullptr;
    ZPage* destination = nullptr;
    BaseObject* from = nullptr;
    BaseObject* to = nullptr;
};

ValueRootRoute PrepareValueRootRoute(GcHeapFixture& fx, bool destinationYoung)
{
    ValueRootRoute route;
    route.source = fx.region0;
    route.destination = fx.region1;
    route.from = fx.PlaceObject(route.source->GetRegionStart());
    route.to = fx.PlaceObject(route.destination->GetRegionStart());
    route.source->SetRegionAllocPtr(reinterpret_cast<MAddress>(route.from) + route.from->GetSize());
    route.destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(route.to) + route.to->GetSize());
    route.source->reset(PageAge::old);
    route.destination->reset(destinationYoung ? PageAge::eden : PageAge::old);
    if (destinationYoung) {
        route.destination->reset(PageAge::eden);
    }

    route.source->SetRegionListOwner(nullptr);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(route.source, route.from));
    RegionList selected("old-source-value-root");
    selected.PrependRegion(route.source);
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Old, selected));
    (void)selected.TakeHeadRegion();
    route.from->SetStateCode(ObjectState::FORWARDED);
    ZForwarding* publication = forwarding_for_page(
        route.source, reinterpret_cast<MAddress>(route.from));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(UNUSED_InsertMapping(
                     publication, reinterpret_cast<MAddress>(route.from),
                     reinterpret_cast<MAddress>(route.to)),
                 reinterpret_cast<MAddress>(route.to));
    GC_EXPECT_EQ(forwarding_for_page(route.source)->find(reinterpret_cast<MAddress>(route.from)),
                 reinterpret_cast<MAddress>(route.to));

    return route;
}

bool IsValueRootMarked(const ValueRootRoute& route)
{
    return route.destination->is_object_strongly_live(from_object(route.to));
}

struct WeakGraph {
    explicit WeakGraph(GcHeapFixture& fixture, ZPage* region, ZPage* targetRegion = nullptr)
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
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
        return region->is_object_strongly_live(from_object(object));
    }

    GcHeapFixture& fx;
    ZPage* owner;
    ZPage* targetOwner;
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
        return owner->is_object_strongly_live(from_object(object));
    }

    GcHeapFixture& fx;
    ZPage* owner;
    BaseObject* root = nullptr;
    BaseObject* foreign = nullptr;
    alignas(TypeInfo) unsigned char foreignTypeStorage[sizeof(TypeInfo)];
    TypeInfo* foreignType = nullptr;
};

void RunYoungWeakVariant(size_t helpers)
{
    WorkerFixture worker(0);
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);

    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    WeakGraph graph(fx, fx.region1);

    HeapGcState& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
    // zGeneration.cpp: each generation owns its worker pool before collection.
    RelocationReceiptTestAccess::BindCollector(&collector);
    {
        auto& young = collector.GetZGeneration(ZGenerationId::young);
        if (young.Workers() == nullptr) young.InitializeWorkers(helpers + 1);
        else young.Workers()->set_active_workers(helpers + 1u);
    }
    collector.GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::MarkComplete);
    RelocationReceiptTestAccess::BindWorkerBudget();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(graph.child);
    const U64 rootHandle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);

    const bool startedBefore = Heap::GetHeap().IsGcStarted();
    const GCReason reasonBefore = Heap::GetHeap().GetGCStats(ZGenerationId::young).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetZGeneration(ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    Heap::GetHeap().GetGCStats(ZGenerationId::young).reason = GC_REASON_YOUNG;

    YoungClosureObservation closure;
    RelocationReceiptTestAccess::RunYoungCollection(collector);
    GC_EXPECT_TRUE(closure.Calls() > 0);
    const bool strongMarked = closure.Saw(graph.strongRoot);
    const bool weakMarked = closure.Saw(graph.weak);
    const bool referentMarked = closure.Saw(graph.referent);
    const bool childMarked = closure.Saw(graph.child);
    std::fprintf(stderr, "TARGET_YOUNG_STRONG_CLOSURE workers=%zu root=%d weak=%d referent=%d child=%d\n",
                 helpers + 1, strongMarked, weakMarked, referentMarked, childMarked);

    Heap::GetHeap().RemoveExportObject(rootHandle);
    if (!ownerWasActive) activityCycle.End();
    Heap::GetHeap().GetGCStats(ZGenerationId::young).reason = reasonBefore;
    RelocationReceiptTestAccess::BindWorkerBudget();

    // The producer-to-consumer bearing point must deliver the field closure.
    // Check that target before the individual root receipts can end the case.
    std::fprintf(stderr, "P1_FIELD_FOLLOW_ASSERT referent=%d child=%d\n", referentMarked, childMarked);
    GC_EXPECT_TRUE(referentMarked && childMarked);
    GC_EXPECT_TRUE(strongMarked);
    GC_EXPECT_TRUE(weakMarked);
    GC_EXPECT_TRUE(referentMarked);
    GC_EXPECT_TRUE(childMarked);
}

void RunYoungWeakRemsetFlow()
{
    WorkerFixture worker(0);
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);

    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;

    // Record the referent field through the product write barrier while it is
    // a real old->young edge.  The holder then enters this young collection;
    // the remembered slot itself, rather than a test-built weakSlots ledger,
    // is what the product minor path receives.
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    WeakGraph graph(fx, fx.region0, fx.region1);
    *reinterpret_cast<uintptr_t*>(graph.referent) = reinterpret_cast<uintptr_t>(fx.typeInfo);
    *reinterpret_cast<uintptr_t*>(graph.child) = reinterpret_cast<uintptr_t>(fx.typeInfo);
    fx.typeInfo->SetUUID(1);
    TypeInfoManager::GetTypeInfoManager().AddTypeInfo(fx.typeInfo);

    HeapGcState& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
    RelocationReceiptTestAccess::BindCollector(&collector);
    collector.GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::MarkComplete);
    RememberedSet& rememberedSet = HeapTestRemset();
    rememberedSet.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);
    HeapSlot<>& referentField = WeakGraph::Field(graph.weak);
    referentField.StoreColoured(to_zpointer(raw(StoreGoodPointer(graph.referent)) ^ ZPointerMarkedYoungMask));
    ZBarrier::WriteReference(graph.weak, referentField, graph.referent);
    const MAddress weakSlot = reinterpret_cast<MAddress>(&referentField);
    const bool recordedBeforeMinor = rememberedSet.Contains(weakSlot);

    RelocationReceiptTestAccess::BindWorkerBudget();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(graph.strongRoot);
    space.GetRegionManager().AddRawPointerObject(graph.child);
    const U64 rootHandle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);

    const bool startedBefore = Heap::GetHeap().IsGcStarted();
    const GCReason reasonBefore = Heap::GetHeap().GetGCStats(ZGenerationId::young).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetZGeneration(ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    Heap::GetHeap().GetGCStats(ZGenerationId::young).reason = GC_REASON_YOUNG;
    YoungClosureObservation closure;
    RelocationReceiptTestAccess::RunYoungCollection(collector);
    GC_EXPECT_TRUE(closure.Calls() > 0);
    const bool referentMarked = closure.Saw(graph.referent);
    std::fprintf(stderr,
                 "DETAIL young_weak_remset slot=%#zx recorded_before_minor=%d referent_mark=%d\n",
                 static_cast<size_t>(weakSlot), static_cast<int>(recordedBeforeMinor),
                 static_cast<int>(referentMarked));

    Heap::GetHeap().RemoveExportObject(rootHandle);
    if (!ownerWasActive) activityCycle.End();
    Heap::GetHeap().GetGCStats(ZGenerationId::young).reason = reasonBefore;
    RelocationReceiptTestAccess::BindWorkerBudget();

    GC_EXPECT_TRUE(recordedBeforeMinor);
    GC_EXPECT_TRUE(referentMarked);
    GC_EXPECT_TRUE(closure.Saw(graph.child));
}

enum class MajorRootFamily {
    COMMON,
    EXPORT,
};

void RunMajorWeakGraph(MajorRootFamily family, bool runtimeEntry = false, size_t helpers = 0)
{
    WorkerFixture worker(0);
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    if (runtimeEntry) {
    }
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    WeakGraph graph(fx, fx.region0);

    HeapGcState& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
    // zGeneration.cpp: each generation owns its worker pool before collection.
    RelocationReceiptTestAccess::BindCollector(&collector);
    {
        auto& old = collector.GetZGeneration(ZGenerationId::old);
        if (old.Workers() == nullptr) old.InitializeWorkers(helpers + 1);
        else old.Workers()->set_active_workers(helpers + 1u);
    }
    collector.GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTestAccess::BindWorkerBudget(static_cast<int32_t>(helpers + 1));
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region0);
    space.GetRegionManager().AddRawPointerObject(graph.child);
    if (runtimeEntry) {
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

    if (runtimeEntry) {
        RelocationReceiptTestAccess::RunMajorCollection(collector);
    } else {
        RelocationReceiptTestAccess::RunMajorMark(collector);
    }
    const size_t discovered =
        Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor().Discovered(ReferenceType::WEAK);
    const bool referentCleared = is_null(WeakGraph::Field(graph.weak).GetFieldValue());
    const bool strongMarked = graph.IsMarked(graph.strongRoot);
    const bool weakMarked = graph.IsMarked(graph.weak);
    const bool referentMarked = graph.IsMarked(graph.referent);
    const bool childMarked = graph.IsMarked(graph.child);
    std::fprintf(stderr,
                 "DETAIL major_weak family=%s discovered=%zu strong_mark=%d weak_mark=%d "
                 "referent_mark=%d child_mark=%d referent_cleared=%d\n",
                 family == MajorRootFamily::COMMON ? "common" : "export", discovered,
                 static_cast<int>(strongMarked), static_cast<int>(weakMarked), static_cast<int>(referentMarked),
                 static_cast<int>(childMarked), static_cast<int>(referentCleared));
    if (registeredCommonRootCount != 0) {
        Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(commonRoots.data()), registeredCommonRootCount);
    }
    if (family == MajorRootFamily::EXPORT) {
        Heap::GetHeap().RemoveExportObject(exportHandle);
    }
    RelocationReceiptTestAccess::BindWorkerBudget();

    if (runtimeEntry) {
        GC_EXPECT_FALSE(collector.GetCycleSnapshot(ZGenerationId::old).active);
        GC_EXPECT_TRUE(referentCleared);
        return;
    }
    GC_EXPECT_EQ(discovered, 1u);
    GC_EXPECT_TRUE(strongMarked);
    GC_EXPECT_TRUE(weakMarked);
    GC_EXPECT_FALSE(referentMarked);
    GC_EXPECT_FALSE(childMarked);
    GC_EXPECT_TRUE(referentCleared);
}

} // namespace

GC_OTHER_VM_TEST(YoungWeakClosure, SingleWorkerKeepsYoungReferentStrong)
{
    RunYoungWeakVariant(0);
}

// Removed legacy parallel selector: ZGC uses one marking worker task.
// One-worker and two-worker cases retain the weak referent closure assertions.

GC_OTHER_VM_TEST(YoungWeakClosure, StripedKeepsYoungReferentStrong)
{
    RunYoungWeakVariant(1);
}

GC_OTHER_VM_TEST(YoungWeakClosure, OldWeakSlotKeepsYoungReferentStrong)
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
    ZMark old(64, MarkingStacks::MarkingGeneration::MAJOR);
    ZMark young(64, MarkingStacks::MarkingGeneration::YOUNG);
    for (ZMark* domain : {&old, &young}) {
        ZMark& current = *domain;
        ZMark& other = domain == &old ? young : old;
        auto& stacks = ThreadLocal::GetMarkStacks(current);
        stacks.Push(current.Stripes(), 0,
                    MarkStackEntry(uintptr_t(0x1000), true, true, true, false), true);
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
            MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
            MarkStripeStack* published = current.Stripes().At(0).StealStack(smr, 0);
            GC_EXPECT_TRUE(published != nullptr);
            MarkStripeStack::Destroy(published);
            MarkingSMRTestAccess::reclaim(smr);
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
    fx.region0->reset(PageAge::old);
    WeakGraph graph(fx, fx.region0);
    // This grid is deliberately non-weak: the only edge must be followed by
    // the export root family even when the common root stack is empty.
    *reinterpret_cast<uintptr_t*>(graph.weak) = reinterpret_cast<uintptr_t>(fx.typeInfo);
    WeakGraph::Field(graph.weak).StoreColoured(zpointer::null);

    HeapGcState& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
    RelocationReceiptTestAccess::BindCollector(&collector);
    collector.GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTestAccess::BindWorkerBudget();
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
    RelocationReceiptTestAccess::BindWorkerBudget();
    GC_EXPECT_TRUE(rootMarked);
    GC_EXPECT_TRUE(childMarked);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MinorRuntimeDispatchMarksCurrentAndWritesBack)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    ValueRootRoute route = PrepareValueRootRoute(fx, true);

    HeapGcState& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
    RelocationReceiptTestAccess::BindCollector(&collector);
    {
        auto& young = collector.GetZGeneration(ZGenerationId::young);
        if (young.Workers() == nullptr) young.InitializeWorkers(1);
        else young.Workers()->set_active_workers(1u);
    }
    collector.GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::MarkComplete);
    RelocationReceiptTestAccess::BindWorkerBudget();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(route.destination);
    space.GetRegionManager().AddRawPointerObject(route.to);
    RelocationReceiptTestAccess::SeedValueRoots(collector, route.from);

    const bool startedBefore = Heap::GetHeap().IsGcStarted();
    const GCReason reasonBefore = Heap::GetHeap().GetGCStats(ZGenerationId::young).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetZGeneration(ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    Heap::GetHeap().GetGCStats(ZGenerationId::young).reason = GC_REASON_YOUNG;
    bool currentMarked = false;
    ValueRootMarkObservation closure([&] { currentMarked |= IsValueRootMarked(route); });
    RelocationReceiptTestAccess::RunYoungCollection(collector);
    GC_EXPECT_TRUE(closure.calls > 0);
    const bool carrierCurrent =
        RelocationReceiptTestAccess::MinorFinishedValueRootsEqual(collector, route.to);
    Heap::GetHeap().PublishGenerationPhase(ZGenerationId::old, ZGenerationPhase::MarkComplete);
    Heap::GetHeap().GetCollector().GetZGeneration(Generation::Young).reset_relocation_set();
    const auto afterCoverage = LookupTo(reinterpret_cast<MAddress>(route.from), Generation::Young);
    const bool independentAfterCoverage =
        RelocationReceiptTestAccess::MinorFinishedValueRootsEqual(collector, route.to);
    std::fprintf(stderr,
                 "VALUE_ROOT_RUNTIME_ASSERT minor current_marked=%d carrier_current=%d "
                 "after_coverage=%d lookup=%u\n",
                 static_cast<int>(currentMarked), static_cast<int>(carrierCurrent),
                 static_cast<int>(independentAfterCoverage),
                 static_cast<unsigned>(afterCoverage.answer));

    if (!ownerWasActive) activityCycle.End();
    Heap::GetHeap().GetGCStats(ZGenerationId::young).reason = reasonBefore;
    RelocationReceiptTestAccess::BindWorkerBudget();
    GC_EXPECT_TRUE(currentMarked);
    GC_EXPECT_TRUE(carrierCurrent);
    GC_EXPECT_TRUE(independentAfterCoverage);
    (void)route;
}

void RunMajorExportOwnership(bool sharedCycle, bool fullDriver = false)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    ExportForeignGraph graph(fx);

    BaseObject* secondRoot = nullptr;
    if (sharedCycle) {
        secondRoot = fx.PlaceObject(graph.owner->GetRegionStart() + 192);
        graph.owner->SetRegionAllocPtr(reinterpret_cast<MAddress>(secondRoot) + 64);
        WeakGraph::Field(secondRoot).StoreColoured(GcUnit::StoreGoodPointer(graph.root));
        WeakGraph::Field(graph.foreign).StoreColoured(GcUnit::StoreGoodPointer(graph.root));
    }
    const size_t owners = sharedCycle ? 2 : 1;

    HeapGcState& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
    RelocationReceiptTestAccess::BindCollector(&collector);
    collector.GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTestAccess::BindWorkerBudget();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(graph.owner);
    space.GetRegionManager().AddRawPointerObject(graph.foreign);
    const U64 exportHandle = Heap::GetHeap().RegisterExportRoot(graph.root);

    const U64 secondHandle = sharedCycle ? Heap::GetHeap().RegisterExportRoot(secondRoot) : 0;
    const U64 duplicateHandle = sharedCycle ? Heap::GetHeap().RegisterExportRoot(graph.root) : 0;
    bool producerCarrier = false;
    bool rootMarked = false;
    bool consumerMarked = false;
    bool handoffCurrent = false;
    size_t beforeObservations = 0;
    size_t afterObservations = 0;
    bool driverCompleted = false;
    if (fullDriver) {
        // Pin the fixture objects while the real driver completes relocation.
        space.GetRegionManager().AddRawPointerObject(graph.root);
        if (secondRoot != nullptr) space.GetRegionManager().AddRawPointerObject(secondRoot);
        HeapGcState::testExportOwnershipResult = [&](const ExportOwnershipTestObservation& observed) {
            const auto paired = [&](const std::vector<ExportOwnershipTestObservation::Edge>& edges) {
                return edges.size() == owners &&
                    std::count(edges.begin(), edges.end(), std::make_pair(graph.root, graph.foreign)) == 1 &&
                    (!sharedCycle || std::count(edges.begin(), edges.end(),
                                               std::make_pair(secondRoot, graph.foreign)) == 1);
            };
            if (!observed.afterHandoff) {
                ++beforeObservations;
                producerCarrier = observed.discoveredOwners == owners && paired(observed.discovered) &&
                    observed.handoffOwners == 0 && observed.handoff.empty();
                rootMarked = graph.IsMarked(graph.root);
                consumerMarked = graph.IsMarked(graph.foreign);
                std::fprintf(stderr,
                             "EXPORT_OWNER_TARGET before producer_carrier=%d root_marked=%d consumer_marked=%d\n",
                             static_cast<int>(producerCarrier), static_cast<int>(rootMarked),
                             static_cast<int>(consumerMarked));
                GC_EXPECT_TRUE(producerCarrier);
            } else {
                ++afterObservations;
                handoffCurrent = observed.discoveredOwners == 0 && observed.discovered.empty() &&
                    observed.handoffOwners == owners && paired(observed.handoff);
                // Check the handoff here: later relocation requires the discovery
                // map to be empty and would otherwise hide this target assertion.
                std::fprintf(stderr, "EXPORT_OWNER_TARGET after handoff_current=%d\n",
                             static_cast<int>(handoffCurrent));
                GC_EXPECT_TRUE(handoffCurrent);
            }
        };
        RelocationReceiptTestAccess::RunMajorCollection(collector);
        HeapGcState::testExportOwnershipResult = nullptr;
        driverCompleted = !collector.GetCycleSnapshot(ZGenerationId::old).active;
    } else {
        RelocationReceiptTestAccess::RunExportMajorMark(collector);
        producerCarrier =
            RelocationReceiptTestAccess::DiscoveredCarrierEquals(collector, graph.root, graph.foreign, owners) &&
            (!sharedCycle || RelocationReceiptTestAccess::DiscoveredCarrierEquals(
                collector, secondRoot, graph.foreign, owners));
        rootMarked = graph.IsMarked(graph.root);
        consumerMarked = graph.IsMarked(graph.foreign);
        RelocationReceiptTestAccess::RunPostTrace(collector);
        handoffCurrent =
            RelocationReceiptTestAccess::CycleHandoffEquals(collector, graph.root, graph.foreign, owners) &&
            (!sharedCycle || RelocationReceiptTestAccess::CycleHandoffEquals(
                collector, secondRoot, graph.foreign, owners));
    }
    std::fprintf(stderr, "EXPORT_OWNER_DRIVER full=%d completed=%d before=%zu after=%zu owners=%zu\n",
                 static_cast<int>(fullDriver), static_cast<int>(driverCompleted),
                 beforeObservations, afterObservations, owners);
    std::fprintf(stderr,
                 "VALUE_ROOT_RUNTIME_ASSERT major producer_carrier=%d root_marked=%d "
                 "consumer_marked=%d handoff_current=%d\n",
                 static_cast<int>(producerCarrier), static_cast<int>(rootMarked),
                 static_cast<int>(consumerMarked), static_cast<int>(handoffCurrent));

    Heap::GetHeap().RemoveExportObject(exportHandle);
    if (sharedCycle) {
        Heap::GetHeap().RemoveExportObject(secondHandle);
        Heap::GetHeap().RemoveExportObject(duplicateHandle);
    }
    RelocationReceiptTestAccess::BindWorkerBudget();
    RelocationReceiptTestAccess::BindCollector(nullptr);
    GC_EXPECT_TRUE(producerCarrier);
    GC_EXPECT_TRUE(rootMarked);
    GC_EXPECT_TRUE(consumerMarked);
    GC_EXPECT_TRUE(handoffCurrent);
    if (fullDriver) {
        GC_EXPECT_EQ(beforeObservations, size_t{1});
        GC_EXPECT_EQ(afterObservations, size_t{1});
        GC_EXPECT_TRUE(driverCompleted);
    }
}


GC_OTHER_VM_TEST(ValueRootCurrentization, MajorProducerConsumerCurrentizesBeforeMark)
{
    RunMajorExportOwnership(false);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MajorExportOwnersSharePremarkedCycle)
{
    RunMajorExportOwnership(true);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MajorDriverPairsExportOwnersBeforeHandoff)
{
    RunMajorExportOwnership(true, true);
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
    HeapGcState& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
    RelocationReceiptTestAccess::BindCollector(&collector);
    collector.GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    NativeSlot root(StoreGoodPointer(graph.weak));
    NativeSlot* roots[] = { &root };
    Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::unordered_set<BaseObject*> strong;
    std::unordered_set<BaseObject*> inclusive;
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
    Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
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
    HeapGcState& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
    RelocationReceiptTestAccess::BindCollector(&collector);
    collector.GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(graph.weak);
    std::unordered_set<BaseObject*> strong;
    std::unordered_set<BaseObject*> inclusive;
    HeapIterator(false).Iterate([&](BaseObject* object) { strong.insert(object); });
    HeapIterator(true).Iterate([&](BaseObject* object) { inclusive.insert(object); });
    Heap::GetHeap().RemoveExportObject(handle);
    GC_EXPECT_TRUE(strong.count(graph.weak) == 0);
    GC_EXPECT_TRUE(inclusive.count(graph.weak) == 1);
    GC_EXPECT_TRUE(inclusive.count(graph.referent) == 1);
    GC_EXPECT_TRUE(inclusive.count(graph.child) == 1);
}

#endif // MRT_TESTABLE_INTERNALS
