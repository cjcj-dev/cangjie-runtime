#if defined(MRT_TESTABLE_INTERNALS)

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

#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <sched.h>
#include <thread>
#include <unordered_set>
#include <vector>

#include "Common/Runtime.h"
#include "CjScheduler.h"

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Concurrency/Concurrency.h"

#define private public
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/LiveInfo.h"
#include "Mutator/Mutator.h"
#undef private

#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/TracingCollector.h"
#include "Heap/WCollector/WCollector.h"
#include "Mutator/SatbBuffer.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

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

    static void InitCollectorProxy(CollectorResources& resources)
    {
        resources.collectorProxy.Init();
    }

    static bool ConsumeYoungSatbAndReach(WCollector& collector, BaseObject* expected)
    {
        TracingCollector::WorkStack workStack;
        WCollector::MinorObjectSet reachableObjects;
        std::vector<BaseObject*> reachableVec;
        WCollector::MinorSlotSet reachableSlots;
        WCollector::MinorSlotSet weakSlots;
        const bool drained = collector.MarkYoungSatbBuffer(workStack, false, reachableObjects, reachableVec,
                                                           reachableSlots, weakSlots, true);
        return drained && std::find(reachableVec.begin(), reachableVec.end(), expected) != reachableVec.end();
    }

    static void RunCollectionDispatch(WCollector& collector)
    {
        collector.SetGCReason(GC_REASON_YOUNG);
        collector.DoGarbageCollection();
    }
};

} // namespace MapleRuntime

namespace {

class TestCollector final : public Collector {
public:
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject*) const override
    {
        return FindToVersionResult::NotForwarded();
    }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return RefField<>(to_zpointer(reinterpret_cast<MAddress>(obj)));
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
        field.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(ref)));
    }
};

class TestTraceBarrier final : public Barrier {
public:
    TestTraceBarrier(Collector& collector, RememberedSet& rememberedSet)
        : Barrier(collector, rememberedSet, BarrierPhase::TRACE) {}
    void Record(BaseObject* obj, MAddress fieldAddress, BaseObject* ref) const
    {
        RecordCrossGenEdge(obj, fieldAddress, ref);
    }

protected:
    void WriteReferenceImpl(BaseObject*, RefField<false>& field, BaseObject* ref) const
    {
        field.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(ref)));
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
    bool already = bm->MarkBits(offset, totalSize, regionSize);
    if (!already) {
        reg->AddLiveByteCount(totalSize);
    }
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
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
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
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
    GC_EXPECT_EQ(setenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY", "1", 1), 0);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_create stage=before_begin\n");
    const uint64_t epoch = manager.BeginEpochHandshakeLifecycleTest();
    std::fprintf(stderr, "DETAIL runtime_lifecycle_create stage=before_create epoch=%zu\n",
                 static_cast<size_t>(epoch));
    Mutator* mutator = manager.CreateRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_create stage=after_create mutator=%p\n", mutator);
    GC_EXPECT_TRUE(mutator != nullptr);
    GC_EXPECT_TRUE(mutator->FinishedEpochHandshake(epoch));
    GC_EXPECT_EQ(manager.RuntimeMutatorRegistrySizeForTest(), 1u);
    manager.DestroyRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_create stage=after_destroy\n");
    manager.EndEpochHandshakeLifecycleTest();
    GC_EXPECT_EQ(unsetenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY"), 0);
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_IDLE);
}

GC_OTHER_VM_TEST(YoungConc, RuntimeMutatorDestroyDuringActiveEpochDefersStorage)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    MutatorManager& manager = MutatorManager::Instance();
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
    GC_EXPECT_EQ(setenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY", "1", 1), 0);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_destroy stage=before_begin\n");
    (void)manager.BeginEpochHandshakeLifecycleTest();
    std::fprintf(stderr, "DETAIL runtime_lifecycle_destroy stage=before_create\n");
    (void)manager.CreateRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_destroy stage=before_destroy\n");
    manager.DestroyRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
    std::fprintf(stderr, "DETAIL runtime_lifecycle_destroy stage=after_destroy deferred=%zu\n",
                 manager.EpochHandshakeDestroyDeferredForTest());
    GC_EXPECT_EQ(manager.RuntimeMutatorRegistrySizeForTest(), 0u);
    GC_EXPECT_EQ(manager.EpochHandshakeDestroyDeferredForTest(), 1u);
    manager.EndEpochHandshakeLifecycleTest();
    GC_EXPECT_EQ(unsetenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY"), 0);
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_IDLE);
}

// Deterministic cross-thread interleaving: publish the epoch first, then let a
// foreign runtime thread complete the real create/exit/destroy path while the
// epoch remains active.  The main thread only closes the epoch after the worker
// has retired its entry, so the participant pin and deferred-storage decision
// are exercised without relying on a probabilistic sleep window.
GC_OTHER_VM_TEST(YoungConc, RuntimeMutatorActiveEpochCreateDestroyInterleavingIsSerialized)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    MutatorManager& manager = MutatorManager::Instance();
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
    GC_EXPECT_EQ(setenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY", "1", 1), 0);
    const uint64_t epoch = manager.BeginEpochHandshakeLifecycleTest();
    std::atomic<bool> created{ false };
    std::atomic<bool> finished{ false };
    std::thread worker([&]() {
        Mutator* mutator = manager.CreateRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
        created.store(mutator != nullptr && mutator->FinishedEpochHandshake(epoch), std::memory_order_release);
        manager.DestroyRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
        finished.store(true, std::memory_order_release);
    });
    while (!created.load(std::memory_order_acquire)) {
        (void)sched_yield();
    }
    while (!finished.load(std::memory_order_acquire)) {
        (void)sched_yield();
    }
    worker.join();
    GC_EXPECT_EQ(manager.RuntimeMutatorRegistrySizeForTest(), 0u);
    GC_EXPECT_TRUE(manager.EpochHandshakeDestroyDeferredForTest() >= 1u);
    manager.EndEpochHandshakeLifecycleTest();
    GC_EXPECT_EQ(unsetenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY"), 0);
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_IDLE);
}

GC_OTHER_VM_TEST(YoungConc, ForcedEpochHandshakeReportsPositiveRequested)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    MutatorManager& manager = MutatorManager::Instance();
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
    GC_EXPECT_EQ(setenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY", "1", 1), 0);
    Mutator* mutator = manager.CreateRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
    GC_EXPECT_TRUE(mutator != nullptr);
    (void)mutator->EnterSaferegion(true);
    EpochHandshakeStats stats = manager.RunEpochHandshake("r6-forced-minor");
    std::fprintf(stderr,
                 "DETAIL forced_epoch requested=%zu scanned=%zu fallback=%zu epoch=%zu\n",
                 stats.requested, stats.stackScanned, stats.stackFallback,
                 static_cast<size_t>(stats.epoch));
    GC_EXPECT_TRUE(stats.requested > 0);
    GC_EXPECT_EQ(stats.stackScanned + stats.stackFallback, stats.requested);
    manager.DestroyRuntimeMutator(ThreadType::HOT_UPDATE_THREAD);
    GC_EXPECT_EQ(unsetenv("MRT_GC_UNIT_RUNTIME_MUTATOR_LIFECYCLE_ONLY"), 0);
}

GC_OTHER_VM_TEST(YoungConc, FinalizerCreateDuringActiveEpochIsBornClean)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    MutatorManager& manager = MutatorManager::Instance();
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
    const uint64_t epoch = manager.BeginEpochHandshakeLifecycleTest();
    void* cjthread = NewFinalizerCJThread();
    GC_EXPECT_TRUE(cjthread != nullptr);
    Mutator* mutator = ThreadLocal::GetMutator();
    GC_EXPECT_TRUE(mutator != nullptr);
    GC_EXPECT_TRUE(mutator->FinishedEpochHandshake(epoch));
    EndFinalizerCJThread();
    manager.EndEpochHandshakeLifecycleTest();
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_IDLE);
}

GC_OTHER_VM_TEST(YoungConc, FinalizerEndDuringActiveEpochDefersStorage)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    MutatorManager& manager = MutatorManager::Instance();
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
    (void)manager.BeginEpochHandshakeLifecycleTest();
    GC_EXPECT_TRUE(NewFinalizerCJThread() != nullptr);
    EndFinalizerCJThread();
    GC_EXPECT_TRUE(ThreadLocal::GetMutator() == nullptr);
    GC_EXPECT_TRUE(manager.EpochHandshakeDestroyDeferredForTest() >= 1u);
    manager.EndEpochHandshakeLifecycleTest();
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_IDLE);
}
// Paint-then-claim-skip: already-marked MarkObject returns true; without grey ledger
// TraceYoungClosure would drop reachableVec/fields (WCollector.cpp:8110-8136).
GC_TEST(YoungConc, PaintWithoutGreyLedgerMissesWorkStack)
{
    GcHeapFixture fx;
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

// Late-edge positive control: the TRACE-window producer publishes an
// already-painted object with an explicit Follow receipt.  The consumer must
// see that bit after the retired-node handoff; treating this as ordinary SATB
// would lose the child traversal because the young mark claim already won.
GC_TEST(YoungConc, LateEdgeFollowReceiptSurvivesSatbHandoff)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet rememberedSet;
    rememberedSet.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestTraceBarrier barrier(collector, rememberedSet);
    Mutator mutator;
    ThreadLocal::SetMutator(&mutator);
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;

    // Product barrier path: this is a late TRACE-window store, not a direct
    // call to the publication helper.
    barrier.Record(fx.obj0, reinterpret_cast<MAddress>(field), fx.obj1);
    mutator.FlushSatbBuffer();

    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;
    ThreadLocal::SetMutator(nullptr);

    bool sawFollow = false;
    BaseObject* sawObject = nullptr;
    SatbBuffer::Instance().GetRetiredEntries([&](BaseObject* object, bool follow) {
        sawObject = object;
        sawFollow = follow;
    });
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(sawObject), reinterpret_cast<uintptr_t>(fx.obj1));
    GC_EXPECT_TRUE(sawFollow);

    fx.region1->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// Consumer-side control retained from the preceding delivery. Keep this in
// another VM because MarkYoungSatbBuffer closes its product termination domain
// with a real STW. The direct entry deliberately isolates the internal
// FollowOnly-to-workStack contract from the dispatch fallback set tested below.
GC_OTHER_VM_TEST(YoungConc, LateEdgeFollowReceiptReachesYoungMarkConsumer)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);

    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());

    BaseObject* child = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(child) + 64);
    auto* parentField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    parentField->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(child)));

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    GCThreadPool threadPool("gc-unit-young-consumer", 0, GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool);
    GC_EXPECT_EQ(setenv("MRT_GCV2_MARKPAR_FORCE_SERIAL", "1", 1), 0);

    RememberedSet rememberedSet;
    rememberedSet.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestTraceBarrier barrier(collector, rememberedSet);
    Mutator mutator;
    ThreadLocal::SetMutator(&mutator);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;

    auto* holderField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    barrier.Record(fx.obj0, reinterpret_cast<MAddress>(holderField), fx.obj1);
    mutator.FlushSatbBuffer();
    ThreadLocal::SetMutator(nullptr);

    std::fprintf(stderr, "DETAIL late_edge_consumer stage=before_consume\n");
    const bool childReached = RelocationReceiptTestAccess::ConsumeYoungSatbAndReach(collector, child);
    std::fprintf(stderr, "DETAIL late_edge_consumer stage=after_consume child_reached=%d\n",
                 static_cast<int>(childReached));

    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;
    GC_EXPECT_TRUE(childReached);

    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    threadPool.Exit();
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    fx.region1->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// End-to-end dispatch control: unlike the consumer-side item above, this one
// enters at WCollector::DoGarbageCollection and lets the young cycle choose any
// of its product SATB consumers.  Cutting the complete young-consumer set must
// therefore make this item fail while any remaining fallback may keep it green.
GC_OTHER_VM_TEST(YoungConc, LateEdgeFollowReceiptReachesYoungRuntimeDispatch)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_MARKPAR_FORCE_SERIAL", "1", 1), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);

    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());

    BaseObject* child = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(child) + 64);
    auto* parentField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    parentField->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(child)));

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    // The product collection needs a pool object. Zero helpers leaves all
    // marking and relocation work on the GC caller in this isolated process.
    GCThreadPool threadPool("gc-unit-young-consumer", 0, GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool);

    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    // Keep the candidate in place after marking. The gc_unit heap has no free
    // region geometry for relocation, and raw-pointer pinning is the product
    // reason a marked candidate legitimately skips that later phase.
    space.GetRegionManager().AddRawPointerObject(child);

    RememberedSet& runtimeRememberedSet = Heap::GetHeap().GetRememberedSet();
    runtimeRememberedSet.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    RememberedSet producerRememberedSet;
    producerRememberedSet.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestTraceBarrier barrier(collector, producerRememberedSet);
    Mutator mutator;
    ThreadLocal::SetMutator(&mutator);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;

    auto* holderField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    barrier.Record(fx.obj0, reinterpret_cast<MAddress>(holderField), fx.obj1);
    mutator.FlushSatbBuffer();
    ThreadLocal::SetMutator(nullptr);

    std::fprintf(stderr, "DETAIL late_edge_consumer stage=before_young_runtime_entry\n");
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    const bool stayedYoung = fx.region1->IsYoungRegion();
    const bool childMarked = stayedYoung
        ? fx.region1->IsMarkedObject(fx.region1->GetMarkView<Generation::Young>(), child)
        : fx.region1->IsMarkedObject(fx.region1->GetMarkView<Generation::Old>(), child);
    std::fprintf(stderr,
                 "DETAIL late_edge_consumer stage=after_young_runtime_entry child_marked=%d stayed_young=%d\n",
                 static_cast<int>(childMarked), static_cast<int>(stayedYoung));

    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;
    GC_EXPECT_TRUE(childMarked);

    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    std::fprintf(stderr, "DETAIL late_edge_consumer stage=before_pool_exit\n");
    threadPool.Exit();
    std::fprintf(stderr, "DETAIL late_edge_consumer stage=after_pool_exit\n");
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    // The isolated process owns the candidate region and its collection
    // metadata until exit; do not free that state behind RegionManager.
    (void)live;
}

// H receipt arm: default product dispatch with a real mutator-local y2y
// holder. The pre-window batch must be empty at the release boundary.
GC_OTHER_VM_TEST(YoungConc, Y2yAfterReleaseBatchForcesContinueAndReachesClosure)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_MARKPAR_FORCE_SERIAL", "1", 1), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());
    BaseObject* child = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(child) + 64);
    auto* parentField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    parentField->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(child)));

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    GCThreadPool threadPool("gc-unit-y2y-receipt", 0, GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(child);
    RememberedSet& runtimeRememberedSet = Heap::GetHeap().GetRememberedSet();
    runtimeRememberedSet.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    RememberedSet producerRememberedSet;
    producerRememberedSet.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestTraceBarrier barrier(collector, producerRememberedSet);
    Mutator mutator;
    ThreadLocal::SetMutator(&mutator);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;
#if defined(MRT_GC_UNIT_TESTS)
    ResetY2yHandoffTestReceipt();
    // The old holder exercises the pre-release merge. The young holder is
    // injected only after stw.reset(), so the first mark-end must fail and the
    // existing continue edge must follow it before a second mark-end succeeds.
    AllocBuffer::GetOrCreateAllocBuffer()->PushY2yDirtyHolder(fx.obj0);
    ArmY2yAfterReleaseTestReceipt(fx.obj1, 2);
#endif
    ThreadLocal::SetMutator(nullptr);
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
#if defined(MRT_GC_UNIT_TESTS)
    const auto receipt = ReadY2yHandoffTestReceipt();
    std::fprintf(stderr,
                 "DETAIL y2y_h_receipt before_release=%zu after_root=%zu mark_end_batch=%zu "
                 "phase0=%zu phase1=%zu phase2=%zu\n",
                 static_cast<size_t>(receipt.beforeRelease), static_cast<size_t>(receipt.afterRoot),
                 static_cast<size_t>(receipt.afterStw2), static_cast<size_t>(receipt.phase0),
                 static_cast<size_t>(receipt.phase1), static_cast<size_t>(receipt.phase2));
    GC_EXPECT_EQ(receipt.beforeRelease, 0u);
    GC_EXPECT_TRUE(receipt.phase0 >= 1);
    // phase1 proves that zero is the sampled post-consumption root-container
    // state, rather than the reset value from an omitted receipt.
    GC_EXPECT_TRUE(receipt.phase1 >= 1);
    GC_EXPECT_EQ(receipt.afterRoot, 0u);
    GC_EXPECT_TRUE(receipt.afterStw2 >= 2);
    GC_EXPECT_TRUE(receipt.phase2 >= 3);
#endif
    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;
    const bool stayedYoung = fx.region1->IsYoungRegion();
    const bool childMarked = stayedYoung
        ? fx.region1->IsMarkedObject(fx.region1->GetMarkView<Generation::Young>(), child)
        : fx.region1->IsMarkedObject(fx.region1->GetMarkView<Generation::Old>(), child);
    GC_EXPECT_TRUE(childMarked);
    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    threadPool.Exit();
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    (void)live;
}

// A mutator publishes ordinary SATB work after coordinated mark workers have
// terminated but before pause-mark-end starts. Each pause is allowed exactly
// one flush. Two publications therefore require two continue edges followed by
// a third, successful pause; an in-pause retry loop or premature commit breaks
// the exact receipt.
GC_OTHER_VM_TEST(YoungConc, SatbAfterWorkerTerminationUsesBoundedMarkEndContinue)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_MARKPAR_FORCE_SERIAL", "1", 1), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
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
    collector.SetGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    GCThreadPool threadPool("gc-unit-satb-mark-end", 0, GCPoolThread::GC_THREAD_PRIORITY);
    RelocationReceiptTestAccess::BindThreadPool(resources, &threadPool);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(first);
    space.GetRegionManager().AddRawPointerObject(second);
    Heap::GetHeap().GetRememberedSet().Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Mutator producer;
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;
    ResetMarkTerminateTestReceipt();
    ArmSatbBeforeMarkEndTestReceipt(&producer, first, second);

    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    const auto receipt = ReadMarkTerminateTestReceipt();
    std::fprintf(stderr,
                 "DETAIL satb_mark_end pauses=%zu flushed=%zu continues=%zu max_pause_ns=%zu\n",
                 receipt.pauses, receipt.flushed, receipt.continues,
                 static_cast<size_t>(receipt.maxPauseNs));
    GC_EXPECT_EQ(receipt.pauses, 3u);
    GC_EXPECT_EQ(receipt.flushed, 2u);
    GC_EXPECT_EQ(receipt.continues, 2u);
    GC_EXPECT_TRUE(receipt.maxPauseNs < 1000000000ULL);
    const auto view = fx.region1->GetMarkView<Generation::Young>();
    GC_EXPECT_TRUE(fx.region1->IsMarkedObject(view, first));
    GC_EXPECT_TRUE(fx.region1->IsMarkedObject(view, second));

    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;
    RelocationReceiptTestAccess::BindThreadPool(resources, nullptr);
    threadPool.Exit();
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    (void)live;
}

// 3. young→young overwrite is not remset (ZGC remember only if slot old; zBarrier:729-733).
GC_TEST(YoungConc, YoungToYoungWriteNotInRemset)
{
    GcHeapFixture fx;
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

// old→young still remset (control: TRACE window must not drop the only remset edge).
GC_TEST(YoungConc, OldToYoungStillRecorded)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
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
    GC_EXPECT_EQ(records.size(), 1u);
    GC_EXPECT_TRUE(records.count(reinterpret_cast<MAddress>(field)) == 1);
}

// Major TRACE window with no young regions: a bulk write must still publish the
// new target to the SATB consumer.  This is the regression arm for the
// RecordCrossGenEdgesInStruct early return; it observes retired entries after
// flushing the mutator node rather than merely checking that a helper ran.
GC_OTHER_VM_TEST(YoungConc, BulkWritePublishesSatbWithoutYoungRegions)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(0);

    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    RelocationReceiptTestAccess::InitCollectorProxy(resources);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    const GCPhase heapPhaseBefore = Heap::GetHeap().GetGCPhase();
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_USER;
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_TRACE);

    Mutator mutator;
    mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
    ThreadLocal::SetMutator(&mutator);

    BaseObject* source = fx.obj1;
    MAddress dstField = reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE;
    barrier.WriteStruct(fx.obj0, dstField, sizeof(source), reinterpret_cast<MAddress>(&source), sizeof(source));

    mutator.FlushSatbBuffer();
    std::vector<BaseObject*> retired;
    SatbBuffer::Instance().GetRetiredObjects(retired);

    ThreadLocal::SetMutator(nullptr);
    Heap::GetHeap().SetGCPhase(heapPhaseBefore);
    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(retired[0]), reinterpret_cast<uintptr_t>(fx.obj1));
}
// mark_and_remember mark half (zBarrier.inline.hpp:735-739): TRACE + young GC
// paints the new young target. STW/Idle TestBarrier must not (phase gate).
// gc_unit never Heap::Init; IsGcStarted is the same fixture latch SATB uses.
GC_TEST(YoungConc, TraceStoreMarksNewYoungTarget)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestTraceBarrier barrier(collector, rs);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;

    field->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj1)));
    // RecordCrossGenEdge is the remember half; TRACE phase adds the mark half.
    // Do not WriteReference: DispatchPhase would static_cast to product TraceBarrier
    // and SATB-push with a null mutator (gc_unit has no Mutator).
    barrier.Record(fx.obj0, reinterpret_cast<MAddress>(field), fx.obj1);

    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;

    MarkView<Generation::Young> view = fx.region1->GetMarkView<Generation::Young>();
    GC_EXPECT_TRUE(fx.region1->IsMarkedObject(view, fx.obj1));
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 1u);

    fx.region1->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(YoungConc, IdleStoreDoesNotMarkNewYoungTarget)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    // Do not SetGcStarted: heap-phase fallback would Heap::GetGCPhase() against
    // a null CollectorProxy::currentCollector (gc_unit never Init). Phase STW
    // plus !IsGcStarted is the product Idle/STW gate.
    field->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj1)));
    barrier.Record(fx.obj0, reinterpret_cast<MAddress>(field), fx.obj1);

    MarkView<Generation::Young> view = fx.region1->GetMarkView<Generation::Young>();
    GC_EXPECT_FALSE(fx.region1->IsMarkedObject(view, fx.obj1));

    fx.region1->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// Peek must not consume the local cleanup ledger drained after mark termination.
GC_TEST(YoungConc, PeekYoungAllocBlackDoesNotConsume)
{
    GcHeapFixture fx;
    auto* buf = new AllocBuffer();
    buf->PushYoungAllocBlack(fx.obj0);
    std::vector<BaseObject*> peeked;
    buf->PeekYoungAllocBlack(peeked);
    GC_EXPECT_EQ(peeked.size(), 1u);
    std::vector<BaseObject*> merged;
    buf->MergeYoungAllocBlack(merged);
    GC_EXPECT_EQ(merged.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(merged[0]), reinterpret_cast<uintptr_t>(fx.obj0));
}

// The three retired runtime switches are not alternate configurations. These
// guards pin the required predicates even when a parent process still exports
// a stale value.
GC_TEST(YoungConc, LegacyMarkEnvCannotDisableRequiredEpochHandshake)
{
    GC_EXPECT_EQ(setenv("MRT_GCV2_YOUNG_CONC_MARK", "0", 1), 0);
    GC_EXPECT_TRUE(MutatorManager::EpochHandshakeEnabled());
    GC_EXPECT_EQ(unsetenv("MRT_GCV2_YOUNG_CONC_MARK"), 0);
}

GC_TEST(YoungConc, LegacyFollowEnvCannotDisableRequiredEpochHandshake)
{
    GC_EXPECT_EQ(setenv("MRT_GCV2_YOUNG_CONC_FOLLOW", "0", 1), 0);
    GC_EXPECT_TRUE(MutatorManager::EpochHandshakeEnabled());
    GC_EXPECT_EQ(unsetenv("MRT_GCV2_YOUNG_CONC_FOLLOW"), 0);
}

GC_TEST(YoungConc, LegacyStackScanEnvCannotDisableRequiredStackScan)
{
    GC_EXPECT_EQ(setenv("MRT_GCV2_CONCURRENT_STACK_SCAN", "0", 1), 0);
    GC_EXPECT_TRUE(MutatorManager::ConcurrentStackScanEnabled());
    GC_EXPECT_EQ(unsetenv("MRT_GCV2_CONCURRENT_STACK_SCAN"), 0);
}

// RegionSpace publishes allocate-black work with the Follow receipt consumed by
// MarkYoungSatbBuffer. Pin that carrier independently of the bitmap paint.
GC_TEST(YoungConc, PublishYoungAllocBlackRetiresFollowReceipt)
{
    GcHeapFixture fx;
    Mutator mutator;
    mutator.satbNode = new SatbBuffer::Node();
    mutator.PublishYoungAllocBlack(fx.obj0);
    mutator.FlushSatbBuffer();

    BaseObject* object = nullptr;
    bool follow = false;
    SatbBuffer::Instance().GetRetiredEntries([&](BaseObject* entry, bool shouldFollow) {
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
GC_TEST(YoungConc, MarkTerminateProtocolDefault)
{
    GC_EXPECT_TRUE(kMarkTerminateInPause);
    GC_EXPECT_TRUE(MarkTerminateInPauseEnabled());
}

// Negative control for the exact pre-fix termination decision. Leave one SATB
// deletion-barrier pre-value in a mutator-local, non-full node. Retired-only
// sampling reports global-empty and would commit termination, while the target
// remains white. This test is supposed to prove the miss, not pretend the
// legacy arm is correct.
GC_TEST(YoungConc, LegacyTerminationMissesUnfullSatbNode)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());
    (void)fx.PlantMarkBitmap<Generation::Old>(live, fx.region0->GetRegionSize());

    Mutator mutator;
    auto* node = new SatbBuffer::Node();
    GC_EXPECT_TRUE(node->Push(fx.obj0, nullptr));
    mutator.satbNode = node;
    GC_EXPECT_TRUE(mutator.PeekSatbNode() == node);
    GC_EXPECT_FALSE(node->IsEmpty());
    GC_EXPECT_FALSE(node->IsFull());

    std::vector<BaseObject*> markWork;
    SatbBuffer::Instance().GetRetiredObjects(markWork);
    const bool legacyWouldTerminate = markWork.empty();
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    const auto view = fx.region0->GetMarkView<Generation::Young>();

    GC_EXPECT_TRUE(legacyWouldTerminate);
    GC_EXPECT_FALSE(fx.region0->IsMarkedObject(view, offset));

    // Strict mark-end cut (ZGC zMark.cpp:954-971, :998-1006): mutators are
    // frozen before their partial nodes are handed over, then the same retired
    // queue is sampled. The discovered grey forces mark-end continue.
    mutator.FlushSatbBuffer();
    GC_EXPECT_TRUE(mutator.PeekSatbNode() == nullptr);
    SatbBuffer::Instance().GetRetiredObjects(markWork);
    const bool strictWouldTerminate = markWork.empty();

    GC_EXPECT_FALSE(strictWouldTerminate);
    GC_EXPECT_EQ(markWork.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(markWork[0]), reinterpret_cast<uintptr_t>(fx.obj0));

    while (!markWork.empty()) {
        BaseObject* grey = markWork.back();
        markWork.pop_back();
        (void)fx.region0->MarkObject(view, grey, 8);
    }
    GC_EXPECT_TRUE(fx.region0->IsMarkedObject(view, offset));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

#endif // MRT_TESTABLE_INTERNALS
