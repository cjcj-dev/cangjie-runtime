#include "marking_smr_test.hpp"
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
#include "Cangjie.h"

#include "gc_heap_fixture.hpp"
#include "selection_cycle_fixture.hpp"
#include "Heap/z/zCrossVM.hpp"
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


#include "gc_generation_test.hpp"
#include "gc_product_access_test.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;



#if defined(MRT_TESTABLE_INTERNALS)

extern "C" int CJ_ScheduleManagerInit();
namespace MapleRuntime { extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize); }

namespace MapleRuntime {

class RelocationReceiptTest {
public:
    static void BindCollector(Heap* collector)
    {
        if (collector != nullptr) {
            CHECK(collector == &Heap::GetHeap());
            for (auto generation : {ZGenerationId::young, ZGenerationId::old}) {
                auto& cycle = Heap::GetHeap().GetZGeneration(generation);
                if (cycle.Snapshot().active) cycle.End();
                if (cycle.Workers() == nullptr) cycle.InitializeWorkers(2);
            }
            Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::MarkComplete);
            auto& remembered = HeapTestRemset();
            if (!remembered.IsInitialized()) {
                remembered.Initialize(Heap::GetHeapStartAddress(), GcHeapFixture::kUnits * ZGranuleSize);
            }
            InitFwdTables();
        }
    }

    // zArguments: the concurrent worker budget the driver hands each request.
    static void BindWorkerBudget(int32_t threadCount = 1)
    {
        ZCollectedHeapTest::SetWorkers(threadCount);
    }

    static void RunYoungCollection(Heap& collector)
    {
        auto& cycle = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
        if (!cycle.Snapshot().active) ZGenerationTest::SetReason(cycle, GC_REASON_YOUNG);
        YoungTypeSetter type(cycle, ZYoungType::minor);
        Heap::GetHeap().young().pause_mark_start();
        Heap::GetHeap().young().concurrent_mark();
    }

    static void PrepareMajorRoots(Heap& collector)
    {
        auto& young = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
        if (young.Workers() == nullptr) young.InitializeWorkers(1);
        auto& oldCycle = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
        if (oldCycle.Snapshot().active) oldCycle.End();
        ZGenerationTest::SetReason(oldCycle, GC_REASON_USER);
        auto& remembered = HeapTestRemset();
        if (!remembered.IsInitialized()) {
            remembered.Initialize(Heap::GetHeapStartAddress(), GcHeapFixture::kUnits * ZGranuleSize);
        }
        // ZDriver::gc_major runs the young roots collection before old marking.
        ZGeneration::young()->collect(ZYoungType::major_partial_roots);
    }

    static void RunMajorMark(Heap& collector)
    {
        PrepareMajorRoots(collector);
        auto& cycle = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
        if (!cycle.Snapshot().active) ZGenerationTest::SetReason(cycle, GC_REASON_USER);
        if (!cycle.Snapshot().active) cycle.Begin(1);
        Heap::GetHeap().old().Mark().BindWorkers(Heap::GetHeap().old().Workers());
        Heap::GetHeap().old().Mark().Start();

        auto& old = Heap::GetHeap().old();
        old.concurrent_mark();
        while (!old.pause_mark_end()) old.concurrent_mark_continue();
        old.process_non_strong_references();
    }

    static void RunPostTrace(Heap& collector) { Heap::GetHeap().old().PostTrace(); }

    static void RunExportMajorMark(Heap& collector, bool oldRootsOnly = false)
    {
        // The major-roots young prelude already began and prepared old marking
        // (zGeneration.cpp:118-124). Do not select/restart that active cycle.
        if (!oldRootsOnly) PrepareMajorRoots(collector);
        auto& old = Heap::GetHeap().old();
        if (oldRootsOnly) {
            if (old.Workers() == nullptr) old.InitializeWorkers(1);
            if (old.Snapshot().active) old.End();
            old.mark_start();
        }
        old.concurrent_mark();
        while (!old.pause_mark_end()) old.concurrent_mark_continue();
        old.process_non_strong_references();
    }

    static void SeedValueRoots(Heap& collector, BaseObject* value)
    {
        {
            std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().resurrectExportMtx);
            Heap::GetHeap().cross_vm().resurrectedExportObjectes.clear();
            Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.clear();
            Heap::GetHeap().cross_vm().resurrectedExportObjectes.insert(value);
            Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.insert(value);
        }
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().cycleWorkStackMtx);
        Heap::GetHeap().cross_vm().cycleRefWorkStack.clear();
        Heap::GetHeap().old().discoveredExternObjects.clear();
        Heap::GetHeap().cross_vm().cycleRefWorkStack[value].push_back(value);
    }

    static bool AllValueRootsEqual(Heap& collector, BaseObject* value)
    {
        {
            std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().resurrectExportMtx);
            if (Heap::GetHeap().cross_vm().resurrectedExportObjectes.size() != 1 ||
                Heap::GetHeap().cross_vm().resurrectedExportObjectes.count(value) != 1 ||
                Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.size() != 1 ||
                Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.count(value) != 1) {
                return false;
            }
        }
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().cycleWorkStackMtx);
        auto it = Heap::GetHeap().cross_vm().cycleRefWorkStack.find(value);
        return Heap::GetHeap().cross_vm().cycleRefWorkStack.size() == 1 && it != Heap::GetHeap().cross_vm().cycleRefWorkStack.end() &&
            it->second.size() == 1 && it->second.front() == value;
    }

    static bool CycleHandoffEquals(Heap& collector, BaseObject* key, BaseObject* value, size_t owners = 1)
    {
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().cycleWorkStackMtx);
        auto it = Heap::GetHeap().cross_vm().cycleRefWorkStack.find(key);
        return Heap::GetHeap().old().discoveredExternObjects.empty() && Heap::GetHeap().cross_vm().cycleRefWorkStack.size() == owners &&
            it != Heap::GetHeap().cross_vm().cycleRefWorkStack.end() && it->second.size() == 1 && it->second.front() == value;
    }

    static bool DiscoveredCarrierEquals(Heap& collector, BaseObject* key, BaseObject* value, size_t owners = 1)
    {
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().externMtx);
        auto it = Heap::GetHeap().old().discoveredExternObjects.find(key);
        return Heap::GetHeap().old().discoveredExternObjects.size() == owners &&
            it != Heap::GetHeap().old().discoveredExternObjects.end() && it->second.size() == 1 &&
            it->second.front() == value;
    }

    static bool MinorFinishedValueRootsEqual(Heap& collector, BaseObject* value)
    {
        {
            std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().resurrectExportMtx);
            if (Heap::GetHeap().cross_vm().resurrectedExportObjectes.size() != 1 ||
                Heap::GetHeap().cross_vm().resurrectedExportObjectes.count(value) != 1 ||
                !Heap::GetHeap().cross_vm().resurrectedExportObjectesForwardPhase.empty()) {
                return false;
            }
        }
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().cycleWorkStackMtx);
        auto it = Heap::GetHeap().cross_vm().cycleRefWorkStack.find(value);
        return Heap::GetHeap().cross_vm().cycleRefWorkStack.size() == 1 && it != Heap::GetHeap().cross_vm().cycleRefWorkStack.end() &&
            it->second.size() == 1 && it->second.front() == value;
    }

    static void RunMajorCollection(Heap& collector)
    {
        PrepareMajorRoots(collector);
        auto& cycle = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
        if (!cycle.Snapshot().active) ZGenerationTest::SetReason(cycle, GC_REASON_USER);
        Heap::GetHeap().old().collect();
    }
};
using RelocationReceiptTestAccess = RelocationReceiptTest;

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
struct WeakGraph {
    template<typename Fixture>
    explicit WeakGraph(Fixture& fx, ZPage* region, ZPage* targetRegion = nullptr)
        : owner(region), targetOwner(targetRegion == nullptr ? region : targetRegion)
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
    template<typename Fixture>
    explicit ExportForeignGraph(Fixture& fx) : owner(fx.region0)
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

    Heap& collector = static_cast<Heap&>(Heap::GetHeap());
    // zGeneration.cpp: each generation owns its worker pool before collection.
    RelocationReceiptTest::BindCollector(&collector);
    {
        auto& young = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
        if (young.Workers() == nullptr) young.InitializeWorkers(helpers + 1);
        else young.Workers()->set_active_workers(helpers + 1u);
    }
    Heap::GetHeap().GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::MarkComplete);
    RelocationReceiptTest::BindWorkerBudget();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    fx.region1->SetRegionRole(ZPageRole::RecentFull);
    const U64 rootHandle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);

    const bool startedBefore = Heap::GetHeap().IsGcStarted();
    const GCReason reasonBefore = Heap::GetHeap().GetZGeneration(ZGenerationId::young).Snapshot().reason;
    auto& activityCycle = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    ZGenerationTest::SetReason(Heap::GetHeap().GetZGeneration(ZGenerationId::young), GC_REASON_YOUNG);

    RelocationReceiptTest::RunYoungCollection(collector);
    const bool strongMarked = graph.IsMarked(graph.strongRoot);
    const bool weakMarked = graph.IsMarked(graph.weak);
    const bool referentMarked = graph.IsMarked(graph.referent);
    const bool childMarked = graph.IsMarked(graph.child);
    std::fprintf(stderr, "TARGET_YOUNG_STRONG_CLOSURE workers=%zu root=%d weak=%d referent=%d child=%d\n",
                 helpers + 1, strongMarked, weakMarked, referentMarked, childMarked);

    Heap::GetHeap().RemoveExportObject(rootHandle);
    if (!ownerWasActive) activityCycle.End();
    ZGenerationTest::SetReason(Heap::GetHeap().GetZGeneration(ZGenerationId::young), reasonBefore);
    RelocationReceiptTest::BindWorkerBudget();

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

    Heap& collector = static_cast<Heap&>(Heap::GetHeap());
    RelocationReceiptTest::BindCollector(&collector);
    Heap::GetHeap().GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::MarkComplete);
    RememberedSet& rememberedSet = HeapTestRemset();
    rememberedSet.Initialize(fx.heapStart, 2 * ZGranuleSize);
    HeapSlot<>& referentField = WeakGraph::Field(graph.weak);
    referentField.StoreColoured(to_zpointer(raw(StoreGoodPointer(graph.referent)) ^ ZPointerMarkedYoungMask));
    ZBarrier::WriteWeakReference(graph.weak, referentField, graph.referent);
    const MAddress weakSlot = reinterpret_cast<MAddress>(&referentField);
    const bool recordedBeforeMinor = rememberedSet.Contains(weakSlot);

    RelocationReceiptTest::BindWorkerBudget();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    fx.region1->SetRegionRole(ZPageRole::RecentFull);
    const U64 rootHandle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);

    const bool startedBefore = Heap::GetHeap().IsGcStarted();
    const GCReason reasonBefore = Heap::GetHeap().GetZGeneration(ZGenerationId::young).Snapshot().reason;
    auto& activityCycle = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    ZGenerationTest::SetReason(Heap::GetHeap().GetZGeneration(ZGenerationId::young), GC_REASON_YOUNG);
    RelocationReceiptTest::RunYoungCollection(collector);
    const bool referentMarked = graph.IsMarked(graph.referent);
    std::fprintf(stderr,
                 "DETAIL young_weak_remset slot=%#zx recorded_before_minor=%d referent_mark=%d\n",
                 static_cast<size_t>(weakSlot), static_cast<int>(recordedBeforeMinor),
                 static_cast<int>(referentMarked));

    Heap::GetHeap().RemoveExportObject(rootHandle);
    if (!ownerWasActive) activityCycle.End();
    ZGenerationTest::SetReason(Heap::GetHeap().GetZGeneration(ZGenerationId::young), reasonBefore);
    RelocationReceiptTest::BindWorkerBudget();

    GC_EXPECT_TRUE(recordedBeforeMinor);
    GC_EXPECT_TRUE(referentMarked);
    GC_EXPECT_TRUE(graph.IsMarked(graph.child));
}

enum class MajorRootFamily {
    COMMON,
    EXPORT,
};

template<typename Fixture = GcHeapFixture>
void RunMajorWeakGraph(MajorRootFamily family, bool runtimeEntry = false, size_t helpers = 0)
{
    WorkerFixture worker(0);
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    if (runtimeEntry) {
    }
    MutatorManager mutatorManager;
    WeakClosureTestRuntime runtime(mutatorManager);
    Fixture fx;
    fx.region0->reset(PageAge::old);
    WeakGraph graph(fx, fx.region0);

    Heap& collector = static_cast<Heap&>(Heap::GetHeap());
    // zGeneration.cpp: each generation owns its worker pool before collection.
    RelocationReceiptTest::BindCollector(&collector);
    {
        auto& old = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
        if (old.Workers() == nullptr) old.InitializeWorkers(helpers + 1);
        else old.Workers()->set_active_workers(helpers + 1u);
    }
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::BindWorkerBudget(static_cast<int32_t>(helpers + 1));
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    fx.region0->SetRegionRole(ZPageRole::RecentFull);
    if (runtimeEntry) {
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
        RelocationReceiptTest::RunMajorCollection(collector);
    } else {
        RelocationReceiptTest::RunMajorMark(collector);
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
    RelocationReceiptTest::BindWorkerBudget();

    if (runtimeEntry) {
        GC_EXPECT_FALSE(Heap::GetHeap().GetCycleSnapshot(ZGenerationId::old).active);
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
    const char* rejectGeneration = std::getenv("GC_UNIT_PRIVATE_STACK_REJECT");
    if (rejectGeneration == nullptr) {
        for (const char* generation : {"old", "young"}) {
            GC_EXPECT_EQ(setenv("GC_UNIT_PRIVATE_STACK_REJECT", generation, 1), 0);
            try {
                RunInOtherVm("MarkingStacksProduct.MarkEndChecksPrivateStacksByGeneration",
                             "Thread marking stack is not empty");
            } catch (...) {
                unsetenv("GC_UNIT_PRIVATE_STACK_REJECT");
                throw;
            }
            GC_EXPECT_EQ(unsetenv("GC_UNIT_PRIVATE_STACK_REJECT"), 0);
        }
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
            other.verify_all_stacks_empty();
            const char* generation = domain == &old ? "old" : "young";
            if (rejectGeneration != nullptr && std::strcmp(rejectGeneration, generation) == 0) {
                signal(SIGABRT, SIG_DFL);
                current.verify_all_stacks_empty();
                return; // Normal completion fails the parent's abort assertion.
            }
            // Verification of the other generation has not flushed this stack.
            GC_EXPECT_FALSE(stacks.IsEmpty());
            GC_EXPECT_TRUE(current.Stripes().IsEmpty());
            GC_EXPECT_TRUE(stacks.Flush(current.Stripes(), true));
            MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
            MarkStripeStack* published = current.Stripes().At(0).StealStack(smr, 0);
            GC_EXPECT_TRUE(published != nullptr);
            MarkStripeStack::Destroy(published);
            MarkingSMRTest::reclaim(smr);
            current.verify_all_stacks_empty();
        }
    }
}

GC_OTHER_VM_TEST(MarkingStacksProduct, MajorSerialEntersFromDoGarbageCollection)
{
    RunMajorWeakGraph<SelectionCycleFixture>(MajorRootFamily::COMMON, true, 0);
}

GC_OTHER_VM_TEST(MarkingStacksProduct, MajorParallelEntersFromDoGarbageCollection)
{
    RunMajorWeakGraph<SelectionCycleFixture>(MajorRootFamily::COMMON, true, 1);
}

GC_OTHER_VM_TEST(MarkingStacksProduct, MajorForeignEntersFromDoGarbageCollection)
{
    RunMajorWeakGraph<SelectionCycleFixture>(MajorRootFamily::EXPORT, true, 0);
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

    Heap& collector = static_cast<Heap&>(Heap::GetHeap());
    RelocationReceiptTest::BindCollector(&collector);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::BindWorkerBudget();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    fx.region0->SetRegionRole(ZPageRole::RecentFull);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(graph.strongRoot);

    RelocationReceiptTest::RunMajorMark(collector);
    const bool rootMarked = graph.IsMarked(graph.strongRoot);
    const bool childMarked = graph.IsMarked(graph.weak);
    std::fprintf(stderr,
                 "DETAIL export_only common_roots=0 foreign_roots=1 root_mark=%d child_mark=%d\n",
                 static_cast<int>(rootMarked), static_cast<int>(childMarked));

    Heap::GetHeap().RemoveExportObject(handle);
    RelocationReceiptTest::BindWorkerBudget();
    GC_EXPECT_TRUE(rootMarked);
    GC_EXPECT_TRUE(childMarked);
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
    Heap& collector = static_cast<Heap&>(Heap::GetHeap());
    RelocationReceiptTest::BindCollector(&collector);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
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
    Heap& collector = static_cast<Heap&>(Heap::GetHeap());
    RelocationReceiptTest::BindCollector(&collector);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
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
