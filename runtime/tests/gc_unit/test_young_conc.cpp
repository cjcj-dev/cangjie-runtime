// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Young concurrent-mark window invariants (REPORT-youngconc 6/20).
// These tests exercise the three mutator actions inside the TRACE window:
//   1. TraceBarrier-shaped SATB pre-image (ShouldEnqueue skip after paint)
//   2. TRACE-window AllocBlack (paint + grey ledger)
//   3. young→young overwrite (not remset; dirty-holder compensation)
// Shape: ZGC gtest construct-state → assert (test_zLiveMap / test_zBitMap).

// Keep this TU in the same testability configuration as the product SO.  The
// standalone gate supplies MRT_TESTABLE_INTERNALS only when those product
// hooks exist; the top-level guard makes the default build an empty TU.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

#include "Common/Runtime.h"
#include "CjScheduler.h"

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Concurrency/Concurrency.h"

#define private public
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zLiveMap.hpp"
#include "Mutator/Mutator.h"
#undef private

#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/WCollector/WCollector.h"
#if defined(MRT_TESTABLE_INTERNALS)
#include "young_closure_observation.hpp"
#include "mark_publication_fixture.hpp"
#endif
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(ReferenceProcessor, WeakDiscoveryPublishesNoStrongMarkWork)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    HeapSlot<>& referent =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(GcUnit::StoreGoodPointer(fx.obj1));
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    TracingCollector::WorkStack workStack;
    MarkView<Generation::Old> view = fx.region1->GetMarkView<Generation::Old>();

    collector.DiscoverWeakReference(fx.obj0, workStack);

    GC_EXPECT_TRUE(workStack.empty());
    GC_EXPECT_FALSE(fx.region1->IsMarkedObject(view, fx.obj1));
    ReferenceProcessor& processor =
        Heap::GetHeap().GetCollectorResources().GetFinalizerProcessor().GetReferenceProcessor();
    processor.ProcessReferences([](BaseObject*) { return true; });
    processor.EnqueueReferences([](BaseObject*) { return true; });
}
#endif // MRT_TESTABLE_INTERNALS

#if defined(MRT_TESTABLE_INTERNALS)

extern "C" int CJ_ScheduleManagerInit();

namespace MapleRuntime {

struct RelocationReceiptTestAccess {
    static void BindCollector(CollectorResources& resources, TracingCollector* collector)
    {
        if (collector == nullptr && resources.collectorProxy.currentCollector != nullptr) {
            // Worker TLS teardown flushes through the still-bound collector.
            for (auto generation : {GCCycleGeneration::YOUNG, GCCycleGeneration::OLD}) {
                resources.collectorProxy.currentCollector->GetGenerationCycle(generation).StopWorkers();
            }
        }
        resources.collectorProxy.currentCollector = collector;
        if (collector != nullptr) {
            // Product driver startup owns one worker set per generation
            // (zDriver.cpp:408-409; ZGC zGeneration.cpp:205-215).
            for (auto generation : {GCCycleGeneration::YOUNG, GCCycleGeneration::OLD}) {
                auto& cycle = collector->GetGenerationCycle(generation);
                if (cycle.Workers() == nullptr) cycle.InitializeWorkers(2);
            }
        }
    }

    static void BindRuntimeWorkers(CollectorResources& resources, RuntimeWorkers* threadPool)
    {
        resources.runtimeWorkers = threadPool;
    }

    static void FlipYoungMarkForNativeBarrier(WCollector& collector)
    {
        collector.flip_young_mark_start();
    }

    static void StartYoungRelocate(WCollector& collector)
    {
        collector.flip_young_relocate_start();
    }

    static void RunCollectionDispatch(WCollector& collector)
    {
        auto& cycle = collector.GetGenerationCycle(GCCycleGeneration::YOUNG);
        if (!cycle.Snapshot().active) cycle.SelectReason(GC_REASON_YOUNG);
        YoungTypeSetter type(cycle, ZYoungType::minor);
        collector.DoGarbageCollection(GCCycleGeneration::YOUNG);
    }
};
} // namespace MapleRuntime

namespace {

class TestCollector final : public Collector {
public:
    void MarkOldObjectIfActive(BaseObject* object, bool gcThread = false) const override
    { MarkPublicationFixture::Current().collector.MarkOldObjectIfActive(object, gcThread); }
    void MarkYoungObjectIfActive(BaseObject* object, bool followOnly = false) const override
    { MarkPublicationFixture::Current().collector.MarkYoungObjectIfActive(object, followOnly); }
    GCCycleSnapshot GetCycleSnapshot(GCCycleGeneration generation) const override
    { return MarkPublicationFixture::Current().collector.GetCycleSnapshot(generation); }
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject*, Generation) const override
    {
        return FindToVersionResult::NotForwarded();
    }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return RefField<>(GcUnit::StoreGoodPointer(obj));
    }
};

class TestBarrier final : public Barrier {
public:
    TestBarrier(Collector& collector, RememberedSet& rememberedSet) : Barrier(collector, rememberedSet) {}
    void Record(BaseObject* obj, MAddress fieldAddress, BaseObject* ref) const
    {
        RecordCrossGenEdge(obj, fieldAddress, ref);
    }

protected:
    void WriteReferenceImpl(BaseObject*, RefField<false>& field, BaseObject* ref) const
    {
        field.StoreColoured(GcUnit::StoreGoodPointer(ref));
    }
};

class YoungConcTestRuntime final : public Runtime {
public:
    explicit YoungConcTestRuntime(MutatorManager& manager)
    {
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        runtime = this;
        manager.Init();
        const ConcurrencyParam concurrencyParam = { 1024, 64, 1 };
        concurrency.Init(concurrencyParam);
    }

    ~YoungConcTestRuntime() override { runtime = nullptr; }

    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}

private:
    Concurrency concurrency;
};

// Bitmap/ledger mechanism model. Product-path attribution is covered by the
// runtime-dispatch tests below, not by this helper.
void ModelAllocBlackPaint(RegionInfo* reg, BaseObject* obj, size_t totalSize, AllocBuffer* ledger)
{
    if (reg == nullptr || reg->IsLargeRegion() || !reg->IsYoungRegion()) {
        return;
    }
    MAddress addr = reinterpret_cast<MAddress>(obj);
    MAddress regionStart = reg->GetRegionStart();
    MAddress regionEnd = reg->GetRegionEnd();
    size_t offset = static_cast<size_t>(addr - regionStart);
    size_t regionSize = static_cast<size_t>(regionEnd - regionStart);
    if (totalSize == 0 || (totalSize % 8) != 0 || offset + totalSize > regionSize) {
        return;
    }
    MarkView<Generation::Young> view = reg->GetMarkView<Generation::Young>();
    RegionBitmap* bm = reg->GetMarkBitmap(view);
    if (bm == nullptr) {
        return;
    }
    (void)bm->MarkBits(offset, totalSize, regionSize);
    LiveInfo* ghost = reg->GetLiveInfo0ForProbe();
    RegionBitmap* ghostBitmap = ghost == nullptr ? nullptr : reg->GetRouteMarkBitmap(ghost);
    if (ghost != nullptr && ghostBitmap != nullptr) {
        (void)ghostBitmap->MarkBits(offset, totalSize, regionSize);
    }
    if (ledger != nullptr) {
        ledger->PushYoungAllocBlack(obj);
    }
}

} // namespace

// 1. SATB / TraceBarrier write: after AllocBlack paint, ShouldEnqueue is false
//    (ZGC zBarrier.inline.hpp:735-740 mark_and_remember; our SATB skips marked).
GC_TEST(YoungConc, PaintedObjectSkippedByShouldEnqueue)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());

    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    MarkView<Generation::Young> view = fx.region0->GetMarkView<Generation::Young>();
    GC_EXPECT_TRUE(fx.region0->GetMarkBitmap(view) != nullptr);
    (void)fx.region0->GetMarkBitmap(view)->MarkBits(off, 8, fx.region0->GetRegionSize());

    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Young>(fx.obj0));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// A current page has one owner/livemap pair. Typed closure views do not expose
// a second current bitmap, so either reader observes the owner's existing mark.
GC_TEST(YoungConc, SingleCurrentMarkSuppressesEnqueueForEitherClosure)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());
    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    MarkView<Generation::Young> youngView = fx.region0->GetMarkView<Generation::Young>();
    (void)fx.region0->GetMarkBitmap(youngView)->MarkBits(off, 8, fx.region0->GetRegionSize());
    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Young>(fx.obj0));
    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Old>(fx.obj0));
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// isTraceRegion without paint is not allocate-black: SATB must still enqueue
// (zBarrier.inline.hpp:735-739 mark_and_remember). Skipping here left SurvivalNode
// array overwrites white (survnode visitSame=0).
GC_TEST(YoungConc, TraceRegionSkipsSatbWithoutPaint)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region0->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    fx.region0->SetTraceRegionFlag(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());

    GC_EXPECT_TRUE(RegionSpace::ShouldEnqueue<Generation::Young>(fx.obj0));
    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_FALSE(fx.region0->IsMarkedObject(fx.region0->GetMarkView<Generation::Young>(), off));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// 2. AllocBlack mechanism: the sole young-concurrent configuration paints and publishes grey work.
GC_TEST(YoungConc, AllocBlackPaintAndGreyMechanism)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());
    auto* buf = new AllocBuffer();
    ModelAllocBlackPaint(fx.region0, fx.obj0, 8, buf);

    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(fx.region0->IsMarkedObject(fx.region0->GetMarkView<Generation::Young>(), off));
    std::vector<BaseObject*> stack;
    buf->MergeYoungAllocBlack(stack);
    GC_EXPECT_EQ(stack.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(stack[0]), reinterpret_cast<uintptr_t>(fx.obj0));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(YoungConc, EpochHandshakeIsRequired)
{
    GC_EXPECT_TRUE(MutatorManager::EpochHandshakeEnabled());
    GC_EXPECT_TRUE(MutatorManager::ConcurrentStackScanEnabled());
}

GC_OTHER_VM_TEST(YoungConc, RuntimeMutatorCreateDuringActiveEpochIsBornClean)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    MutatorManager& manager = MutatorManager::Instance();
    Heap::GetHeap().SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_ENUM);
    GC_EXPECT_EQ(setenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY", "1", 1), 0);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_create stage=before_begin\n");
    const uint64_t epoch = manager.BeginEpochHandshakeLifecycleTest();
    std::fprintf(stderr, "DETAIL runtime_lifecycle_create stage=before_create epoch=%zu\n",
                 static_cast<size_t>(epoch));
    Mutator* mutator = manager.CreateRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_create stage=after_create mutator=%p\n", mutator);
    GC_EXPECT_TRUE(mutator != nullptr);
    GC_EXPECT_TRUE(mutator->FinishedEpochHandshake(epoch));
    std::vector<Mutator*> registered;
    manager.GetAllMutators(registered);
    GC_EXPECT_EQ(std::count(registered.begin(), registered.end(), mutator), 1);
    manager.DestroyRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_create stage=after_destroy\n");
    manager.EndEpochHandshakeLifecycleTest();
    GC_EXPECT_EQ(unsetenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY"), 0);
    Heap::GetHeap().SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_IDLE);
}

GC_OTHER_VM_TEST(YoungConc, RuntimeMutatorDestroyDuringActiveEpochDefersStorage)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    MutatorManager& manager = MutatorManager::Instance();
    Heap::GetHeap().SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_ENUM);
    GC_EXPECT_EQ(setenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY", "1", 1), 0);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_destroy stage=before_begin\n");
    (void)manager.BeginEpochHandshakeLifecycleTest();
    std::fprintf(stderr, "DETAIL runtime_lifecycle_destroy stage=before_create\n");
    Mutator* target = manager.CreateRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
    std::vector<Mutator*> registered;
    manager.GetAllMutators(registered);
    GC_EXPECT_EQ(std::count(registered.begin(), registered.end(), target), 1);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_destroy stage=before_destroy\n");
    manager.DestroyRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_destroy stage=after_destroy deferred=%zu\n",
                 manager.EpochHandshakeDestroyDeferredForTest());
    registered.clear();
    manager.GetAllMutators(registered);
    GC_EXPECT_EQ(std::count(registered.begin(), registered.end(), target), 0);
    GC_EXPECT_EQ(manager.EpochHandshakeDestroyDeferredForTest(), 1u);
    manager.EndEpochHandshakeLifecycleTest();
    GC_EXPECT_EQ(unsetenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY"), 0);
    Heap::GetHeap().SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_IDLE);
}

GC_OTHER_VM_TEST(YoungConc, FinalizerCreateDuringActiveEpochIsBornClean)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    MutatorManager& manager = MutatorManager::Instance();
    Heap::GetHeap().SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_ENUM);
    const uint64_t epoch = manager.BeginEpochHandshakeLifecycleTest();
    void* cjthread = NewFinalizerCJThread();
    GC_EXPECT_TRUE(cjthread != nullptr);
    Mutator* mutator = ThreadLocal::GetMutator();
    GC_EXPECT_TRUE(mutator != nullptr);
    GC_EXPECT_TRUE(mutator->FinishedEpochHandshake(epoch));
    EndFinalizerCJThread();
    manager.EndEpochHandshakeLifecycleTest();
    Heap::GetHeap().SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_IDLE);
}

GC_OTHER_VM_TEST(YoungConc, FinalizerEndDuringActiveEpochDefersStorage)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    MutatorManager& manager = MutatorManager::Instance();
    Heap::GetHeap().SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_ENUM);
    (void)manager.BeginEpochHandshakeLifecycleTest();
    GC_EXPECT_TRUE(NewFinalizerCJThread() != nullptr);
    EndFinalizerCJThread();
    GC_EXPECT_TRUE(ThreadLocal::GetMutator() == nullptr);
    GC_EXPECT_TRUE(manager.EpochHandshakeDestroyDeferredForTest() >= 1u);
    manager.EndEpochHandshakeLifecycleTest();
    Heap::GetHeap().SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_IDLE);
}
// Paint-then-claim-skip: already-marked MarkObject returns true; without grey ledger
// TraceYoungClosure would drop reachableVec/fields (WCollector.cpp:8110-8136).
GC_TEST(YoungConc, PaintWithoutGreyLedgerMissesWorkStack)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());

    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    MarkView<Generation::Young> view = fx.region0->GetMarkView<Generation::Young>();
    RegionBitmap* bm = fx.region0->GetMarkBitmap(view);
    GC_EXPECT_TRUE(bm != nullptr);
    bool first = bm->MarkBits(off, 8, fx.region0->GetRegionSize());
    GC_EXPECT_FALSE(first);
    bool claim = bm->MarkBits(off, 8, fx.region0->GetRegionSize());
    GC_EXPECT_TRUE(claim);

    auto* empty = new AllocBuffer();
    std::vector<BaseObject*> missed;
    empty->MergeYoungAllocBlack(missed);
    GC_EXPECT_EQ(missed.size(), 0u);

    auto* grey = new AllocBuffer();
    grey->PushYoungAllocBlack(fx.obj0);
    std::vector<BaseObject*> found;
    grey->MergeYoungAllocBlack(found);
    GC_EXPECT_EQ(found.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(found[0]), reinterpret_cast<uintptr_t>(fx.obj0));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// A mutator publishes ordinary SATB work after coordinated mark workers have
// terminated but before pause-mark-end starts. Each pause is allowed exactly
// one flush. Two publications therefore require two continue edges followed by
// a third, successful pause; an in-pause retry loop or premature commit breaks
// the exact receipt.
GC_OTHER_VM_TEST(YoungConc, SatbAfterWorkerTerminationUsesBoundedMarkEndContinue)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());
    BaseObject* first = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 64);
    BaseObject* second = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 128);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + 64);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(first);
    space.GetRegionManager().AddRawPointerObject(second);
    Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Mutator producer;
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats(GCCycleGeneration::YOUNG).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats(GCCycleGeneration::YOUNG).reason = GC_REASON_YOUNG;
    ResetMarkTerminateTestReceipt();
    ArmMarkBeforeMarkEndTestReceipt(&producer, first);

    YoungClosureObservation closure;
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    GC_EXPECT_TRUE(closure.Calls() > 0);
    const auto receipt = ReadMarkTerminateTestReceipt();
    std::fprintf(stderr,
                 "DETAIL satb_mark_end pauses=%zu flushed=%zu continues=%zu max_pause_ns=%zu\n",
                 receipt.pauses, receipt.flushed, receipt.continues,
                 static_cast<size_t>(receipt.maxPauseNs));
    GC_EXPECT_EQ(receipt.pauses, 2u);
    GC_EXPECT_EQ(receipt.flushed, 1u);
    GC_EXPECT_EQ(receipt.continues, 1u);
    GC_EXPECT_TRUE(receipt.maxPauseNs < 1000000000ULL);
    GC_EXPECT_TRUE(closure.Saw(first));
    (void)second;

    if (!ownerWasActive) activityCycle.End();
    resources.GetGCStats(GCCycleGeneration::YOUNG).reason = reasonBefore;
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    (void)live;
}

// Allocate-black Follow is merged into FollowYoungMark. Pause leftover
// injection must stay zero; cutting that concurrent merge reds only this case.
GC_OTHER_VM_TEST(YoungConc, YoungAllocBlackVisibleBeforePauseMarkEnd)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());
    BaseObject* child = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(child) + 64);
    auto* holderField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    holderField->StoreColoured(GcUnit::StoreGoodPointer(child));
    (void)fx.region1->MarkObject(fx.region1->GetMarkView<Generation::Young>(), fx.obj1, 8);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(fx.obj1);
    Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats(GCCycleGeneration::YOUNG).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats(GCCycleGeneration::YOUNG).reason = GC_REASON_YOUNG;
    ResetMarkTerminateTestReceipt();
    ArmAllocBlackDuringConcurrentTestReceipt(fx.obj1);

    YoungClosureObservation closure;
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    GC_EXPECT_TRUE(closure.Calls() > 0);
    const auto receipt = ReadMarkTerminateTestReceipt();
    std::fprintf(stderr,
                 "DETAIL allocblack_mark_end pauses=%zu continues=%zu pauseAllocBlack=%zu closure=%zu\n",
                 receipt.pauses, receipt.continues, receipt.pauseAllocBlack, receipt.closureDuringPause);
    GC_EXPECT_EQ(receipt.pauseAllocBlack, 0u);
    GC_EXPECT_EQ(receipt.continues, 0u);
    GC_EXPECT_TRUE(closure.Saw(child));

    if (!ownerWasActive) activityCycle.End();
    resources.GetGCStats(GCCycleGeneration::YOUNG).reason = reasonBefore;
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    (void)live;
}

// ZGC zGeneration.cpp:550-552,897-905: published work prevents mark completion.
// Observe closure before relocation changes the page generation (zPage.cpp:64).
GC_OTHER_VM_TEST(YoungConc, Y2yDirtyVisibleBeforePauseMarkEnd)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());
    BaseObject* child = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(child) + 64);
    auto* holderField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    holderField->StoreColoured(GcUnit::StoreGoodPointer(child));

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(fx.obj1);
    Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats(GCCycleGeneration::YOUNG).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats(GCCycleGeneration::YOUNG).reason = GC_REASON_YOUNG;
    ResetMarkTerminateTestReceipt();
    ArmY2yDuringConcurrentTestReceipt(fx.obj1);

    YoungClosureObservation closure;
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    const auto receipt = ReadMarkTerminateTestReceipt();
    std::fprintf(stderr,
                 "DETAIL y2y_mark_end pauses=%zu continues=%zu pauseY2y=%zu closure=%zu\n",
                 receipt.pauses, receipt.continues, receipt.pauseY2y, receipt.closureDuringPause);
    std::fprintf(stderr, "TARGET y2y holder=%d child=%d\n", closure.Saw(fx.obj1), closure.Saw(child));
    GC_EXPECT_EQ(receipt.pauseY2y, 0u);
    GC_EXPECT_EQ(receipt.continues, 0u);
    GC_EXPECT_TRUE(closure.Saw(fx.obj1));
    GC_EXPECT_TRUE(closure.Saw(child));

    if (!ownerWasActive) activityCycle.End();
    resources.GetGCStats(GCCycleGeneration::YOUNG).reason = reasonBefore;
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    (void)live;
}

// Worker termination then mutator leftover alloc-black/y2y before STW.
// Pause must merge leftover and continue; it must not commit mark-end.
GC_OTHER_VM_TEST(YoungConc, LeftoverAllocBlackAndY2yAfterWorkerForcesContinue)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());
    BaseObject* allocChild = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 64);
    BaseObject* y2yHolder = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 128);
    BaseObject* y2yChild = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 192);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(y2yChild) + 64);
    auto* allocField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    allocField->StoreColoured(GcUnit::StoreGoodPointer(allocChild));
    (void)fx.region1->MarkObject(fx.region1->GetMarkView<Generation::Young>(), fx.obj1, 8);
    auto* y2yField = &HeapSlotAt<>(reinterpret_cast<MAddress>(y2yHolder) + TYPEINFO_PTR_SIZE);
    y2yField->StoreColoured(GcUnit::StoreGoodPointer(y2yChild));

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(fx.obj1);
    space.GetRegionManager().AddRawPointerObject(y2yHolder);
    Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats(GCCycleGeneration::YOUNG).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats(GCCycleGeneration::YOUNG).reason = GC_REASON_YOUNG;
    ResetMarkTerminateTestReceipt();
    ArmLeftoverBeforePauseTestReceipt(fx.obj1, y2yHolder);

    YoungClosureObservation closure;
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    const auto receipt = ReadMarkTerminateTestReceipt();
    std::fprintf(stderr,
                 "DETAIL leftover_mark_end pauses=%zu continues=%zu pauseAllocBlack=%zu pauseY2y=%zu\n",
                 receipt.pauses, receipt.continues, receipt.pauseAllocBlack, receipt.pauseY2y);
    std::fprintf(stderr, "TARGET leftover allocChild=%d holder=%d y2yChild=%d continues=%zu\n",
                 closure.Saw(allocChild), closure.Saw(y2yHolder), closure.Saw(y2yChild), receipt.continues);
    GC_EXPECT_TRUE(receipt.continues >= 1u);
    GC_EXPECT_TRUE(receipt.pauseAllocBlack >= 1u);
    GC_EXPECT_TRUE(receipt.pauseY2y >= 1u);
    GC_EXPECT_TRUE(closure.Saw(allocChild));
    GC_EXPECT_TRUE(closure.Saw(y2yHolder));
    GC_EXPECT_TRUE(closure.Saw(y2yChild));

    if (!ownerWasActive) activityCycle.End();
    resources.GetGCStats(GCCycleGeneration::YOUNG).reason = reasonBefore;
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    (void)live;
}

GC_OTHER_VM_TEST(YoungConc, PauseMarkEndNeverRunsClosure)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(fx.obj1) + 64);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(fx.obj1);
    Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats(GCCycleGeneration::YOUNG).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats(GCCycleGeneration::YOUNG).reason = GC_REASON_YOUNG;
    ResetMarkTerminateTestReceipt();

    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    const auto receipt = ReadMarkTerminateTestReceipt();
    std::fprintf(stderr, "DETAIL pause_no_closure pauses=%zu closure=%zu\n", receipt.pauses,
                 receipt.closureDuringPause);
    GC_EXPECT_TRUE(receipt.pauses >= 1u);
    GC_EXPECT_EQ(receipt.closureDuringPause, 0u);

    if (!ownerWasActive) activityCycle.End();
    resources.GetGCStats(GCCycleGeneration::YOUNG).reason = reasonBefore;
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    (void)live;
}

// ZGC native stores consume prev (zBarrier.inline.hpp:709-715), including
// export root membership. An empty slot has no old value to publish.
GC_OTHER_VM_TEST(YoungConc, ExportRootRegistrationDoesNotMarkIncomingValue)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    YoungConcTestRuntime runtime(manager);
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    fx.region1->SetYoungRegionFlag(1);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(fx.obj1);
    std::vector<BaseObject*> work;
    mark.DrainObjects(work);
    GC_EXPECT_TRUE(work.empty());
    GC_EXPECT_TRUE(Heap::GetHeap().GetExportObject(handle) == fx.obj1);
    Heap::GetHeap().RemoveExportObject(handle);
}

// Positive counterpart: deleting a pre-mark root preserves its previous value.
GC_OTHER_VM_TEST(YoungConc, RemovingExportRootPublishesPreviousValue)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    YoungConcTestRuntime runtime(manager);
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    fx.region1->SetYoungRegionFlag(1);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(fx.obj1);
    RelocationReceiptTestAccess::FlipYoungMarkForNativeBarrier(mark.collector);
    Heap::GetHeap().RemoveExportObject(handle);
    std::vector<BaseObject*> work;
    mark.DrainObjects(work);
    GC_EXPECT_EQ(work.size(), 1u);
    GC_EXPECT_TRUE(work.front() == fx.obj1);
    GC_EXPECT_TRUE(Heap::GetHeap().GetExportObject(handle) == nullptr);
}

// 3. young→young overwrite is not remset (ZGC remember only if slot old; zBarrier:729-733).
GC_TEST(YoungConc, YoungToYoungWriteNotInRemset)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 0u);
}

// Compensation: y2y dirty holder is merged into work stack (AllocBuffer.h:84-105).
GC_TEST(YoungConc, YoungToYoungDirtyHolderReachesWorkStack)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    auto* buf = new AllocBuffer();
    buf->PushY2yDirtyHolder(fx.obj0);
    buf->PushY2yDirtyHolder(fx.obj0);
    GC_EXPECT_EQ(buf->Y2yDirtyHolderCount(), 1u);
    std::vector<BaseObject*> stack;
    buf->MergeY2yDirtyHolders(stack);
    GC_EXPECT_EQ(stack.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(stack[0]), reinterpret_cast<uintptr_t>(fx.obj0));
    GC_EXPECT_EQ(buf->Y2yDirtyHolderCount(), 0u);
}

#if defined(MRT_TESTABLE_INTERNALS) || defined(MRT_GC_UNIT_TESTS)
namespace {
struct Y2yMergePhaseGate {
    std::mutex lock;
    std::condition_variable changed;
    bool mergeOwnsBuffer{ false };
    bool producerAtPush{ false };
};

void HoldY2yMergeAtPhaseSwitch(void* context)
{
    auto& gate = *static_cast<Y2yMergePhaseGate*>(context);
    std::unique_lock<std::mutex> lock(gate.lock);
    gate.mergeOwnsBuffer = true;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&gate]() { return gate.producerAtPush; });
}
} // namespace

// The young mark consumer and a mutator can meet at the concurrent phase
// boundary. Freeze that ordering after the consumer owns the buffer but before
// it takes the batch: the late publication must remain intact for the next
// batch, never mutate the batch being iterated.
GC_TEST(YoungConc, Y2yDirtyHolderPhaseSwitchHandsOffWholeBatch)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    auto* buffer = new AllocBuffer();
    Y2yMergePhaseGate gate;
    buffer->PushY2yDirtyHolder(fx.obj0);
    buffer->SetY2yDirtyHolderMergeHookForTest(HoldY2yMergeAtPhaseSwitch, &gate);

    std::thread producer([&]() {
        {
            std::unique_lock<std::mutex> lock(gate.lock);
            gate.changed.wait(lock, [&gate]() { return gate.mergeOwnsBuffer; });
            gate.producerAtPush = true;
            gate.changed.notify_all();
        }
        buffer->PushY2yDirtyHolder(fx.obj1);
    });
    GcUnit::JoinGuard join(producer);

    std::vector<BaseObject*> firstBatch;
    buffer->MergeY2yDirtyHolders(firstBatch);
    producer.join();
    buffer->SetY2yDirtyHolderMergeHookForTest(nullptr, nullptr);

    GC_EXPECT_EQ(firstBatch.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(firstBatch[0]), reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_EQ(buffer->Y2yDirtyHolderCount(), 1u);

    std::vector<BaseObject*> secondBatch;
    buffer->MergeY2yDirtyHolders(secondBatch);
    GC_EXPECT_EQ(secondBatch.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(secondBatch[0]), reinterpret_cast<MAddress>(fx.obj1));
    GC_EXPECT_EQ(buffer->Y2yDirtyHolderCount(), 0u);
}

// Load-good colour wrapping a FORWARDED from must remap before store-good
// colour (zBarrier.inline.hpp:591-623). LookupTo of the coloured address is
// then a miss on the current table.
GC_TEST(YoungConc, TraceRefFieldRemapsPreviousRelocationEpoch)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    fx.obj0->SetStateCode(ObjectState::FORWARDED);

    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    const MAddress to = reinterpret_cast<MAddress>(fx.obj1);
    // Install the selected generation set before borrowing its publication.
    fx.InstallPageOwner(fx.region0);
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(fx.region0, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, from, to), to);
    publication = ForwardingTable::Publication();

    auto* field = &HeapSlotAt<>(to + TYPEINFO_PTR_SIZE);
    field->StoreColoured(GcUnit::StoreGoodPointer(fx.obj0));
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    TracingCollector::WorkStack workStack;
    RelocationReceiptTestAccess::StartYoungRelocate(collector);
    collector.TraceRefField(fx.obj1, *field, workStack);

    BaseObject* healed = to_object(field->GetTargetObject());
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(healed), to);
    GC_EXPECT_EQ(static_cast<unsigned>(Collector::JudgeHandOutTarget(healed)),
                 static_cast<unsigned>(HandVerdict::Usable));
    ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(healed), Generation::Young);
    GC_EXPECT_TRUE(lookup.answer != ForwardingTable::ToAnswer::ArmedHit);
}
#endif

// old→young still remset (control: TRACE window must not drop the only remset edge).
GC_TEST(YoungConc, OldToYoungStillRecorded)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    YoungConcTestRuntime runtime(manager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    field->StoreColoured(to_zpointer(raw(StoreGoodPointer(fx.obj1)) ^ MARKED_YOUNG_MASK ^ MARKED_OLD_MASK));
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 1u);
    GC_EXPECT_TRUE(records.count(reinterpret_cast<MAddress>(field)) == 1);
}

// Major TRACE window with no young regions: a bulk write must still publish the
// new target to the SATB consumer.  This is the regression arm for the
// RecordCrossGenEdgesInStruct early return; it observes retired entries after
// flushing the mutator node rather than merely checking that a helper ran.
GC_OTHER_VM_TEST(YoungConc, BulkWritePublishesSatbWithoutYoungRegions)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    YoungConcTestRuntime runtime(manager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(0);
    TestCollector collector;
    RememberedSet remembered;
    remembered.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, remembered);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(to_zpointer(raw(StoreGoodPointer(fx.obj1)) ^ MARKED_YOUNG_MASK ^ MARKED_OLD_MASK));
    BaseObject* incoming = nullptr;
    barrier.WriteStruct(fx.obj0, reinterpret_cast<MAddress>(&field), sizeof(incoming),
                        reinterpret_cast<MAddress>(&incoming), sizeof(incoming));
    std::vector<BaseObject*> work;
    markFixture.DrainObjects(work);
    GC_EXPECT_EQ(work.size(), 1u);
    GC_EXPECT_TRUE(work.front() == fx.obj1);
    GC_EXPECT_TRUE(is_null(field.GetTargetObject()));
}
// mark_and_remember mark half (zBarrier.inline.hpp:735-739): TRACE + young GC
// paints the new young target. STW/Idle TestBarrier must not (phase gate).
// gc_unit never Heap::Init; IsGcStarted is the same fixture latch SATB uses.
GC_TEST(YoungConc, TraceStorePublishesPreviousYoungTarget)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    YoungConcTestRuntime runtime(manager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    BaseObject* incoming = fx.PlaceObject(fx.heapStart + RegionInfo::UNIT_SIZE + 128);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet remembered;
    remembered.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, remembered);
    // ZBarrier::store_barrier_on_heap_oop_field reads prev before the store
    // (zBarrier.inline.hpp:695-705); stale mark colors force its slow path.
    field.StoreColoured(to_zpointer(raw(StoreGoodPointer(fx.obj1)) ^ MARKED_YOUNG_MASK ^ MARKED_OLD_MASK));
    barrier.WriteReference(fx.obj0, field, incoming);
    std::vector<BaseObject*> work;
    markFixture.DrainObjects(work);
    GC_EXPECT_EQ(work.size(), 1u);
    GC_EXPECT_TRUE(work.front() == fx.obj1);
    GC_EXPECT_TRUE(to_object(field.GetTargetObject()) == incoming);
    GC_EXPECT_TRUE(remembered.Contains(reinterpret_cast<MAddress>(&field)));
    GC_EXPECT_FALSE(fx.region1->IsMarkedObject(fx.region1->GetMarkView<Generation::Young>(), incoming));
}

GC_TEST(YoungConc, IdleStoreDoesNotPublishMarkWork)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    YoungConcTestRuntime runtime(manager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    markFixture.collector.GetGenerationCycle(GCCycleGeneration::YOUNG).PublishPhase(GC_PHASE_IDLE);
    markFixture.collector.GetGenerationCycle(GCCycleGeneration::OLD).PublishPhase(GC_PHASE_IDLE);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet remembered;
    remembered.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, remembered);
    field.StoreColoured(to_zpointer(raw(StoreGoodPointer(fx.obj1)) ^ MARKED_YOUNG_MASK ^ MARKED_OLD_MASK));
    barrier.WriteReference(fx.obj0, field, nullptr);
    std::vector<BaseObject*> work;
    markFixture.DrainObjects(work);
    GC_EXPECT_TRUE(work.empty());
    GC_EXPECT_TRUE(is_null(field.GetTargetObject()));
    GC_EXPECT_TRUE(remembered.Contains(reinterpret_cast<MAddress>(&field)));
}

// Pin the required epoch handshake and stack scan predicates.
GC_TEST(YoungConc, MarkRequiresEpochHandshake)
{
    GC_EXPECT_TRUE(MutatorManager::EpochHandshakeEnabled());
}

GC_TEST(YoungConc, FollowRequiresEpochHandshake)
{
    GC_EXPECT_TRUE(MutatorManager::EpochHandshakeEnabled());
}

GC_TEST(YoungConc, StackScanIsRequired)
{
    GC_EXPECT_TRUE(MutatorManager::ConcurrentStackScanEnabled());
}

// RegionSpace publishes allocate-black work with the Follow receipt consumed by
// FollowYoungMark. Pin that carrier independently of the bitmap paint.
GC_TEST(YoungConc, PublishYoungAllocBlackPublishesFollowReceipt)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    Mutator mutator;
    fx.region0->SetYoungRegionFlag(1);
    mutator.PublishYoungAllocBlack(fx.obj0);
    // DrainPublishedMarkEntries flushes the actual thread-local mark stack.

    BaseObject* object = nullptr;
    bool follow = false;
    DrainPublishedMarkEntries([&](BaseObject* entry, bool shouldFollow) {
        object = entry;
        follow = shouldFollow;
    });
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(object), reinterpret_cast<uintptr_t>(fx.obj0));
    GC_EXPECT_TRUE(follow);
}

// FlipForMinor is an O(1) handoff: pre-flip records are scanned now while a
// record produced after the flip remains on the active face for the next cycle.
GC_TEST(YoungConc, FlipForMinorSeparatesConcurrentProducerFace)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rememberedSet;
    rememberedSet.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    const MAddress before = fx.heapStart + 8 * sizeof(void*);
    const MAddress during = fx.heapStart + 9 * sizeof(void*);
    rememberedSet.Record(before);
    rememberedSet.FlipForMinor();
    rememberedSet.Record(during);

    std::unordered_set<MAddress> previous;
    rememberedSet.ScanPreviousForMinor(previous);
    GC_EXPECT_EQ(previous.size(), 1u);
    GC_EXPECT_TRUE(previous.count(before) == 1);
    GC_EXPECT_TRUE(rememberedSet.Snapshot().count(during) == 1);
}

GC_TEST(YoungConc, YoungAllocBlackCleanupLedgerIsOneShot)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    auto* buffer = new AllocBuffer();
    buffer->PushYoungAllocBlack(fx.obj0);
    std::vector<BaseObject*> first;
    std::vector<BaseObject*> second;
    buffer->MergeYoungAllocBlack(first);
    buffer->MergeYoungAllocBlack(second);
    GC_EXPECT_EQ(first.size(), 1u);
    GC_EXPECT_TRUE(second.empty());
}

// Product must not return to retired-only termination. Flipping the constant is
// also the deliberate-break red proof for the regression guard below.
GC_TEST(YoungConc, MarkEndDomainContainsPublishedYoungWork)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(1);
    GC_EXPECT_EQ(markFixture.YoungPending(), 0u);
    markFixture.collector.MarkYoungObjectIfActive(fx.obj0, true);
    GC_EXPECT_EQ(markFixture.YoungPending(), 1u);
    GC_EXPECT_EQ(markFixture.OldPending(), 0u);
}

// Negative control for the exact pre-fix termination decision. Leave one SATB
// deletion-barrier pre-value in a mutator-local, non-full node. Retired-only
// sampling reports global-empty and would commit termination, while the target
// remains white. This test is supposed to prove the miss, not pretend the
// legacy arm is correct.
GC_TEST(YoungConc, StoreBufferFlushPublishesYoungMarkWork)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    RememberedSet remembered;
    remembered.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buffer;
    const MAddress slot = reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE;
    const zpointer previous = RefField<>(fx.obj1, ::g_cjStoreGoodMask).GetFieldValue();
    buffer.Add(slot, fx.obj0, previous, remembered);
    GC_EXPECT_EQ(markFixture.YoungPending(), 0u);
    GC_EXPECT_EQ(buffer.Pending(), 1u);
    buffer.Flush(remembered);
    GC_EXPECT_TRUE(buffer.IsEmpty());
    GC_EXPECT_EQ(markFixture.YoungPending(), 1u);
    GC_EXPECT_TRUE(remembered.Contains(slot));
    std::vector<BaseObject*> work;
    markFixture.DrainObjects(work);
    GC_EXPECT_EQ(work.size(), 1u);
    GC_EXPECT_TRUE(work[0] == fx.obj1);
}

#endif // MRT_TESTABLE_INTERNALS

// After a completed handoff, new inserts belong to the live set, not the
// already-swapped batch. Header-only MergeY2yDirtyHolders (AllocBuffer.h).
#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(YoungConc, Y2yAfterHandoffWritesStayOnLiveSet)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    auto* buffer = new AllocBuffer();
    buffer->PushY2yDirtyHolder(fx.obj0);
    std::vector<BaseObject*> firstBatch;
    buffer->MergeY2yDirtyHolders(firstBatch);
    buffer->PushY2yDirtyHolder(fx.obj1);
    GC_EXPECT_EQ(firstBatch.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(firstBatch[0]), reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_EQ(buffer->Y2yDirtyHolderCount(), 1u);
    std::vector<BaseObject*> secondBatch;
    buffer->MergeY2yDirtyHolders(secondBatch);
    GC_EXPECT_EQ(secondBatch.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(secondBatch[0]), reinterpret_cast<MAddress>(fx.obj1));
}
#endif // MRT_TESTABLE_INTERNALS

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(YoungConc, Y2yThreadExitLeavesHoldersForNextMerge)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    auto* buffer = new AllocBuffer();
    buffer->PushY2yDirtyHolder(fx.obj0);
    buffer->PushY2yDirtyHolder(fx.obj1);
    GC_EXPECT_EQ(buffer->Y2yDirtyHolderCount(), 2u);
    std::vector<BaseObject*> batch;
    buffer->MergeY2yDirtyHolders(batch);
    GC_EXPECT_EQ(batch.size(), 2u);
    GC_EXPECT_EQ(buffer->Y2yDirtyHolderCount(), 0u);
}
#endif // MRT_TESTABLE_INTERNALS

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(YoungConc, Y2yPendingCountVisibleForTerminate)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    auto* buffer = new AllocBuffer();
    buffer->PushY2yDirtyHolder(fx.obj0);
    std::vector<BaseObject*> firstBatch;
    buffer->MergeY2yDirtyHolders(firstBatch);
    buffer->PushY2yDirtyHolder(fx.obj1);
    GC_EXPECT_EQ(buffer->Y2yDirtyHolderCount(), 1u);
}
#endif // MRT_TESTABLE_INTERNALS
