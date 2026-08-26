// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Young concurrent-mark window invariants (REPORT-youngconc 6/20).
// Product YOUNG_CONC_MARK stays pinned-off (RegionSpace.cpp:307-310).
// These tests reconstruct the three mutator actions inside the TRACE window:
//   1. TraceBarrier-shaped SATB pre-image (ShouldEnqueue skip after paint)
//   2. TRACE-window AllocBlack (paint + grey ledger)
//   3. young→young overwrite (not remset; dirty-holder compensation)
// Shape: ZGC gtest construct-state → assert (test_zLiveMap / test_zBitMap).
//
// Compile-time arm (not MRT_GCV2_* env):
//   GC_UNIT_YOUNG_CONC_MARK_ON=0 default — models product pin
//   =1 — models allocate-black paint+ledger as if the constexpr were true

#ifndef GC_UNIT_YOUNG_CONC_MARK_ON
#define GC_UNIT_YOUNG_CONC_MARK_ON 0
#endif

// This TU needs the existing test-peer friendship to reach the private product
// consumer.  Product builds do not compile this file, and no product shape or
// export is changed by enabling the test-only declarations here.
#ifndef MRT_TESTABLE_INTERNALS
#define MRT_TESTABLE_INTERNALS 1
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "Common/Runtime.h"

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
#include "Heap/Verify/Stw2CurrentAudit.h"
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

#if defined(MRT_TESTABLE_INTERNALS)
    static void InitCollectorProxy(CollectorResources& resources)
    {
        resources.collectorProxy.Init();
    }
#endif

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

constexpr bool kYoungConcMarkOn = (GC_UNIT_YOUNG_CONC_MARK_ON != 0);

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

// Product AllocBlack (RegionSpace.cpp:326-346) when youngConcMarkOn.
void ProductAllocBlackPaint(RegionInfo* reg, BaseObject* obj, size_t totalSize, AllocBuffer* ledger)
{
    if (!kYoungConcMarkOn || reg == nullptr || reg->IsLargeRegion() || !reg->IsYoungRegion()) {
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

// 2. AllocBlack: pin-off leaves white; constexpr-on paints + greys (RegionSpace.cpp:299-346).
GC_TEST(YoungConc, AllocBlackWhiteWhenConcMarkOff)
{
    if (kYoungConcMarkOn) {
        return;
    }
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());
    auto* buf = new AllocBuffer();
    ProductAllocBlackPaint(fx.region0, fx.obj0, 8, buf);

    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_FALSE(fx.region0->IsMarkedObject(fx.region0->GetMarkView<Generation::Young>(), off));
    std::vector<BaseObject*> stack;
    buf->MergeYoungAllocBlack(stack);
    GC_EXPECT_EQ(stack.size(), 0u);

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(YoungConc, AllocBlackPaintAndGreyWhenConcMarkOn)
{
    if (!kYoungConcMarkOn) {
        return;
    }
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());
    auto* buf = new AllocBuffer();
    ProductAllocBlackPaint(fx.region0, fx.obj0, 8, buf);

    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(fx.region0->IsMarkedObject(fx.region0->GetMarkView<Generation::Young>(), off));
    std::vector<BaseObject*> stack;
    buf->MergeYoungAllocBlack(stack);
    GC_EXPECT_EQ(stack.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(stack[0]), reinterpret_cast<uintptr_t>(fx.obj0));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
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
    GC_EXPECT_EQ(setenv("MRT_GCV2_YOUNG_CONC_MARK", "1", 1), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_YOUNG_CONC_FOLLOW", "0", 1), 0);
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

// H receipt arm: the same product dispatch is run with FOLLOW enabled and a
// real mutator-local y2y holder.  The expected beforeRelease value is tied to
// the compile-time fixed arm, so swapping only the product SO is a precise
// 0 -> red -> 0 control.
GC_OTHER_VM_TEST(YoungConc, Y2yBeforeReleaseReceiptIsMeasured)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_YOUNG_CONC_MARK", "1", 1), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_YOUNG_CONC_FOLLOW", "1", 1), 0);
    GC_EXPECT_EQ(setenv("MRT_GCV2_EPOCH_HANDSHAKE", "1", 1), 0);
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
    AllocBuffer::GetOrCreateAllocBuffer()->PushY2yDirtyHolder(fx.obj0);
#endif
    auto* holderField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    barrier.Record(fx.obj0, reinterpret_cast<MAddress>(holderField), fx.obj1);
    mutator.FlushSatbBuffer();
    ThreadLocal::SetMutator(nullptr);
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
#if defined(MRT_GC_UNIT_TESTS)
    const auto receipt = ReadY2yHandoffTestReceipt();
    std::fprintf(stderr,
                 "DETAIL y2y_h_receipt before_release=%zu after_root=%zu after_stw2=%zu "
                 "phase0=%zu phase1=%zu phase2=%zu\n",
                 static_cast<size_t>(receipt.beforeRelease), static_cast<size_t>(receipt.afterRoot),
                 static_cast<size_t>(receipt.afterStw2), static_cast<size_t>(receipt.phase0),
                 static_cast<size_t>(receipt.phase1), static_cast<size_t>(receipt.phase2));
    // The fixed arm consumes before release; the default arm only observes.
#if defined(MRT_WAVE8_Y2Y_FIXED_ARM)
    GC_EXPECT_EQ(receipt.beforeRelease, 0u);
#else
    GC_EXPECT_TRUE(receipt.beforeRelease > 0);
#endif
    GC_EXPECT_TRUE(receipt.phase0 >= 1);
    GC_EXPECT_EQ(receipt.afterRoot, 0u);
    GC_EXPECT_EQ(receipt.afterStw2, 0u);
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
#if defined(MRT_TESTABLE_INTERNALS)
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
#endif

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

// Peek must not consume the grey-list MergeYoungAllocBlack still owns (Stw2CurrentAudit).
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

// Positive control: ArmInject forces uncovered+=1 so a zero cannot mean dead probe.
// Product default kStw2CurrentAudit=false (rec=stw |Δ|=6.4%); skip when off.
GC_TEST(YoungConc, Stw2CurrentInjectForcesUncovered)
{
    if (!Stw2CurrentAudit::Enabled()) {
        return;
    }
    const size_t before = Stw2CurrentAudit::Uncovered();
    Stw2CurrentAudit::ArmInject();
    std::unordered_set<MAddress> empty;
    Stw2CurrentAudit::Census(empty, nullptr);
    GC_EXPECT_EQ(Stw2CurrentAudit::Uncovered(), before + 1);
}

GC_TEST(YoungConc, ClassifyWaterBeatsMark)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region0->metadata.markStartAllocPtr = reinterpret_cast<uintptr_t>(fx.obj0);
    std::unordered_set<BaseObject*> none;
    GC_EXPECT_EQ(static_cast<unsigned>(Stw2CurrentAudit::ClassifyTarget(fx.obj0, none, none)),
                 static_cast<unsigned>(Stw2CurrentAudit::Stw2Cover::Water));
}

GC_TEST(YoungConc, ClassifyMarkedWhenNoWater)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region0->metadata.markStartAllocPtr = 0;
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());
    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    MarkView<Generation::Young> view = fx.region0->GetMarkView<Generation::Young>();
    bool first = fx.region0->GetMarkBitmap(view)->MarkBits(off, 8, fx.region0->GetRegionSize());
    GC_EXPECT_FALSE(first);
    std::unordered_set<BaseObject*> none;
    GC_EXPECT_EQ(static_cast<unsigned>(Stw2CurrentAudit::ClassifyTarget(fx.obj0, none, none)),
                 static_cast<unsigned>(Stw2CurrentAudit::Stw2Cover::Marked));
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(YoungConc, ClassifyAllocBlackWhenUnmarked)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region0->metadata.markStartAllocPtr = 0;
    std::unordered_set<BaseObject*> allocBlack;
    allocBlack.insert(fx.obj0);
    std::unordered_set<BaseObject*> none;
    GC_EXPECT_EQ(static_cast<unsigned>(Stw2CurrentAudit::ClassifyTarget(fx.obj0, allocBlack, none)),
                 static_cast<unsigned>(Stw2CurrentAudit::Stw2Cover::AllocBlack));
}

GC_TEST(YoungConc, ClassifyUncoveredYoung)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region0->metadata.markStartAllocPtr = 0;
    std::unordered_set<BaseObject*> none;
    GC_EXPECT_EQ(static_cast<unsigned>(Stw2CurrentAudit::ClassifyTarget(fx.obj0, none, none)),
                 static_cast<unsigned>(Stw2CurrentAudit::Stw2Cover::Uncovered));
}

GC_TEST(YoungConc, ClassifySkipOldHolderTarget)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region0->metadata.markStartAllocPtr = 0;
    std::unordered_set<BaseObject*> none;
    GC_EXPECT_EQ(static_cast<unsigned>(Stw2CurrentAudit::ClassifyTarget(fx.obj0, none, none)),
                 static_cast<unsigned>(Stw2CurrentAudit::Stw2Cover::Skip));
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
