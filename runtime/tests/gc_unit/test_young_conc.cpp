// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Young concurrent-mark window invariants (REPORT-youngconc 6/20).
// These tests exercise the three mutator actions inside the TRACE window:
//   1. TraceBarrier-shaped SATB pre-image (ShouldEnqueue skip after paint)
//   2. Mark publication and concurrent termination
//   3. young→young overwrite (not remset; dirty-holder compensation)
// Shape: ZGC gtest construct-state → assert (test_zLiveMap / test_zBitMap).

// Keep this TU in the same testability configuration as the product SO.  The
// standalone gate supplies MRT_TESTABLE_INTERNALS only when those product
// hooks exist; the top-level guard makes the default build an empty TU.

#include <algorithm>
#include <dlfcn.h>
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
#include "gc_worker_fixture.hpp"
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
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Heap/z/zMark.hpp"
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
    WorkerFixture worker(0);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    HeapSlot<>& referent =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(GcUnit::StoreGoodPointer(fx.obj1));
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    WorkStack workStack;
    collector.DiscoverWeakReference(fx.obj0, workStack);

    GC_EXPECT_TRUE(workStack.empty());
    GC_EXPECT_FALSE(fx.region1->is_object_strongly_live(from_object(fx.obj1)));
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
    static void BindCollector(CollectorResources& resources, CopyCollector* collector)
    {
        if (collector == nullptr && resources.testCollector != nullptr) {
            // Worker TLS teardown flushes through the still-bound collector.
            for (auto generation : {ZGenerationId::young, ZGenerationId::old}) {
                resources.testCollector->GetZGeneration(generation).StopWorkers();
            }
        }
        if (collector != nullptr && resources.testCollector != nullptr) {
            GcUnit::GcHeapFixture::AdoptGenerationIdentity(*collector, *resources.testCollector);
        }
        resources.testCollector = collector;
        if (collector != nullptr) {
            // Product driver startup owns one worker set per generation
            // (zDriver.cpp:408-409; ZGC zGeneration.cpp:205-215).
            for (auto generation : {ZGenerationId::young, ZGenerationId::old}) {
                auto& cycle = collector->GetZGeneration(generation);
                if (cycle.Workers() == nullptr) cycle.InitializeWorkers(2);
            }
        }
    }

    static void FlipYoungMarkForNativeBarrier(WCollector& collector)
    {
        ZGlobalsPointers::flip_young_mark_start();
    }

    static void StartYoungRelocate(WCollector& collector)
    {
        ZGlobalsPointers::flip_young_relocate_start();
    }

    static void RunCollectionDispatch(WCollector& collector)
    {
        auto& cycle = collector.GetZGeneration(ZGenerationId::young);
        if (!cycle.Snapshot().active) cycle.SelectReason(GC_REASON_YOUNG);
        YoungTypeSetter type(cycle, ZYoungType::minor);
        collector.DoGarbageCollection(ZGenerationId::young);
    }
};
} // namespace MapleRuntime

namespace {

class TestCollector final : public Collector {
public:
    void MarkOldObjectIfActive(BaseObject* object, bool gcThread = false) const override
    { MarkPublicationFixture::Current().collector.MarkOldObjectIfActive(object, gcThread); }
    void MarkYoungObjectIfActive(BaseObject* object) const override
    { MarkPublicationFixture::Current().collector.MarkYoungObjectIfActive(object); }
    GCCycleSnapshot GetCycleSnapshot(ZGenerationId generation) const override
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
} // namespace

// 1. SATB / TraceBarrier write: a marked object suppresses duplicate enqueue
//    (ZGC zBarrier.inline.hpp:735-740 mark_and_remember; our SATB skips marked).
GC_TEST(YoungConc, PaintedObjectSkippedByShouldEnqueue)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->reset(PageAge::eden);
    fx.region0->reset(PageAge::eden);

    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region0, fx.obj0));

    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Young>(fx.obj0));

}

// A current page has one owner/livemap pair. Typed closure views do not expose
// a second current bitmap, so either reader observes the owner's existing mark.
GC_TEST(YoungConc, SingleCurrentMarkSuppressesEnqueueForEitherClosure)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->reset(PageAge::eden);
    fx.region0->reset(PageAge::eden);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region0, fx.obj0));
    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Young>(fx.obj0));
    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Old>(fx.obj0));
}

// Paint-then-claim-skip: already-marked MarkObject returns true; without grey ledger
// TraceYoungClosure would drop reachableVec/fields (WCollector.cpp:8110-8136).
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
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    BaseObject* first = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 64);
    BaseObject* second = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 128);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + 64);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::MarkComplete);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(first);
    space.GetRegionManager().AddRawPointerObject(second);
    Mutator producer;
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats(ZGenerationId::young).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetZGeneration(ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats(ZGenerationId::young).reason = GC_REASON_YOUNG;
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
    resources.GetGCStats(ZGenerationId::young).reason = reasonBefore;
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
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
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    BaseObject* child = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(child) + 64);
    auto* holderField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    holderField->StoreColoured(GcUnit::StoreGoodPointer(child));

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::MarkComplete);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(fx.obj1);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats(ZGenerationId::young).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetZGeneration(ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats(ZGenerationId::young).reason = GC_REASON_YOUNG;
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
    resources.GetGCStats(ZGenerationId::young).reason = reasonBefore;
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
}

// ZGC zGeneration.cpp:550-552,897-905: published work prevents mark completion.
// Observe closure before relocation changes the page generation (zPage.cpp:64).
GC_OTHER_VM_TEST(YoungConc, Y2yAfterReleaseBatchForcesContinueAndReachesClosure)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    BaseObject* child = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(child) + 64);
    auto* holderField = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    holderField->StoreColoured(GcUnit::StoreGoodPointer(child));

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::MarkComplete);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(fx.obj1);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats(ZGenerationId::young).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetZGeneration(ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats(ZGenerationId::young).reason = GC_REASON_YOUNG;
    ResetMarkTerminateTestReceipt();
    ResetY2yHandoffTestReceipt();
    Mutator producer;
    ThreadLocal::SetMutator(&producer);
    AllocBuffer::GetOrCreateAllocBuffer()->PushY2yDirtyHolder(fx.obj0);
    ArmY2yAfterReleaseTestReceipt(fx.obj1, 2);
    ThreadLocal::SetMutator(nullptr);

    YoungClosureObservation closure;
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    const auto receipt = ReadMarkTerminateTestReceipt();
    const auto handoff = ReadY2yHandoffTestReceipt();
    // ZGC zGeneration.cpp:550-552,897-905: each publication after worker
    // termination requires another concurrent follow before mark can end.
    std::fprintf(stderr,
                 "TARGET y2y_repeat published=%zu pauses=%zu continues=%zu holder=%d child=%d\n",
                 static_cast<size_t>(handoff.afterStw2), static_cast<size_t>(handoff.phase2),
                 receipt.continues, closure.Saw(fx.obj1), closure.Saw(child));
    // ZGC zGeneration.cpp:550-552: while (!pause_mark_end()) concurrent_mark_continue();
    // Extra STW after y2y release is not a ZGC pause (young has 3 STW only).
    GC_EXPECT_TRUE(receipt.continues >= 1u);
    GC_EXPECT_TRUE(closure.Saw(fx.obj1));
    GC_EXPECT_TRUE(closure.Saw(child));

    if (!ownerWasActive) activityCycle.End();
    resources.GetGCStats(ZGenerationId::young).reason = reasonBefore;
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
}

// Worker termination then mutator leftover y2y before STW.
// Pause must merge leftover and continue; it must not commit mark-end.
GC_OTHER_VM_TEST(YoungConc, LeftoverY2yAfterWorkerForcesContinue)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    BaseObject* y2yHolder = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 128);
    BaseObject* y2yChild = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 192);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(y2yChild) + 64);
    auto* y2yField = &HeapSlotAt<>(reinterpret_cast<MAddress>(y2yHolder) + TYPEINFO_PTR_SIZE);
    y2yField->StoreColoured(GcUnit::StoreGoodPointer(y2yChild));

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::MarkComplete);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(fx.obj1);
    space.GetRegionManager().AddRawPointerObject(y2yHolder);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats(ZGenerationId::young).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetZGeneration(ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats(ZGenerationId::young).reason = GC_REASON_YOUNG;
    ResetMarkTerminateTestReceipt();
    ArmLeftoverBeforePauseTestReceipt(y2yHolder);

    YoungClosureObservation closure;
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    const auto receipt = ReadMarkTerminateTestReceipt();
    std::fprintf(stderr,
                 "DETAIL leftover_mark_end pauses=%zu continues=%zu pauseY2y=%zu\n",
                 receipt.pauses, receipt.continues, receipt.pauseY2y);
    std::fprintf(stderr, "TARGET leftover holder=%d y2yChild=%d continues=%zu\n",
                 closure.Saw(y2yHolder), closure.Saw(y2yChild), receipt.continues);
    GC_EXPECT_TRUE(receipt.continues >= 1u);
    GC_EXPECT_TRUE(receipt.pauseY2y >= 1u);
    GC_EXPECT_TRUE(closure.Saw(y2yHolder));
    GC_EXPECT_TRUE(closure.Saw(y2yChild));

    if (!ownerWasActive) activityCycle.End();
    resources.GetGCStats(ZGenerationId::young).reason = reasonBefore;
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
}

GC_OTHER_VM_TEST(YoungConc, PauseMarkEndNeverRunsClosure)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutatorManager;
    YoungConcTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(fx.obj1) + 64);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    collector.GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::MarkComplete);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().EnlistFullThreadLocalRegion(fx.region1);
    space.GetRegionManager().AddRawPointerObject(fx.obj1);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats(ZGenerationId::young).reason;
    auto& activityCycle = Heap::GetHeap().GetCollector().GetZGeneration(ZGenerationId::young);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats(ZGenerationId::young).reason = GC_REASON_YOUNG;
    ResetMarkTerminateTestReceipt();

    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    const auto receipt = ReadMarkTerminateTestReceipt();
    std::fprintf(stderr, "DETAIL pause_no_closure pauses=%zu closure=%zu\n", receipt.pauses,
                 receipt.closureDuringPause);
    GC_EXPECT_TRUE(receipt.pauses >= 1u);
    GC_EXPECT_EQ(receipt.closureDuringPause, 0u);

    if (!ownerWasActive) activityCycle.End();
    resources.GetGCStats(ZGenerationId::young).reason = reasonBefore;
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
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
    fx.region1->reset(PageAge::eden);
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
    fx.region1->reset(PageAge::eden);
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
    fx.region0->reset(PageAge::eden);
    fx.region0->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(zpointer::null);
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
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

#endif

// old→young still remset (control: TRACE window must not drop the only remset edge).
GC_TEST(YoungConc, OldToYoungStillRecorded)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    YoungConcTestRuntime runtime(manager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    field->StoreColoured(to_zpointer(raw(StoreGoodPointer(fx.obj1)) ^ ZPointerMarkedYoungMask ^ ZPointerMarkedOldMask));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
    if (Mutator* mutator = Mutator::GetMutator(); mutator != nullptr && mutator->GetGCData().storeBarrierBuffer != nullptr) {
        mutator->GetGCData().storeBarrierBuffer->Flush();
    }
    GC_EXPECT_TRUE(SlotPageRemembered(reinterpret_cast<MAddress>(field)));
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
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::old);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(to_zpointer(raw(StoreGoodPointer(fx.obj1)) ^ ZPointerMarkedYoungMask ^ ZPointerMarkedOldMask));
    BaseObject* incoming = nullptr;
    ZBarrier::WriteStruct(fx.obj0, reinterpret_cast<MAddress>(&field), sizeof(incoming),
                        reinterpret_cast<MAddress>(&incoming), sizeof(incoming));
    if (Mutator* mutator = Mutator::GetMutator(); mutator != nullptr && mutator->GetGCData().storeBarrierBuffer != nullptr) {
        mutator->GetGCData().storeBarrierBuffer->Flush();
    }
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
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    BaseObject* incoming = fx.PlaceObject(fx.heapStart + ZPage::UNIT_SIZE + 128);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    // ZBarrier::store_barrier_on_heap_oop_field reads prev before the store
    // (zBarrier.inline.hpp:695-705); stale mark colors force its slow path.
    field.StoreColoured(to_zpointer(raw(StoreGoodPointer(fx.obj1)) ^ ZPointerMarkedYoungMask ^ ZPointerMarkedOldMask));
    ZBarrier::WriteReference(fx.obj0, field, incoming);
    if (Mutator* mutator = Mutator::GetMutator(); mutator != nullptr && mutator->GetGCData().storeBarrierBuffer != nullptr) {
        mutator->GetGCData().storeBarrierBuffer->Flush();
    }
    std::vector<BaseObject*> work;
    markFixture.DrainObjects(work);
    GC_EXPECT_EQ(work.size(), 1u);
    GC_EXPECT_TRUE(work.front() == fx.obj1);
    GC_EXPECT_TRUE(to_object(field.GetTargetObject()) == incoming);
    GC_EXPECT_TRUE(SlotPageRemembered(reinterpret_cast<MAddress>(&field)));
    GC_EXPECT_FALSE(fx.region1->is_object_strongly_live(from_object(incoming)));
}

GC_TEST(YoungConc, IdleStoreDoesNotPublishMarkWork)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    YoungConcTestRuntime runtime(manager);
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    markFixture.collector.GetZGeneration(ZGenerationId::young).PublishPhase(ZGenerationPhase::Relocate);
    markFixture.collector.GetZGeneration(ZGenerationId::old).PublishPhase(ZGenerationPhase::Relocate);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(to_zpointer(raw(StoreGoodPointer(fx.obj1)) ^ ZPointerMarkedYoungMask ^ ZPointerMarkedOldMask));
    ZBarrier::WriteReference(fx.obj0, field, nullptr);
    if (Mutator* mutator = Mutator::GetMutator(); mutator != nullptr && mutator->GetGCData().storeBarrierBuffer != nullptr) {
        mutator->GetGCData().storeBarrierBuffer->Flush();
    }
    std::vector<BaseObject*> work;
    markFixture.DrainObjects(work);
    GC_EXPECT_TRUE(work.empty());
    GC_EXPECT_TRUE(is_null(field.GetTargetObject()));
    GC_EXPECT_TRUE(SlotPageRemembered(reinterpret_cast<MAddress>(&field)));
}

GC_TEST(YoungConc, StackScanIsRequired)
{
    GC_EXPECT_TRUE(MutatorManager::ConcurrentStackScanEnabled());
}

// FlipForMinor is an O(1) handoff: pre-flip records are scanned now while a
// record produced after the flip remains on the active face for the next cycle.
// Product must not return to retired-only termination. Flipping the constant is
// also the deliberate-break red proof for the regression guard below.
GC_TEST(YoungConc, MarkEndDomainContainsPublishedYoungWork)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->reset(PageAge::eden);
    GC_EXPECT_EQ(markFixture.YoungPending(), 0u);
    markFixture.collector.MarkYoungObjectIfActive(fx.obj0);
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
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    RememberedSet remembered;
    remembered.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);
    StoreBarrierBuffer buffer;
    const MAddress slot = reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE;
    const zpointer previous = RefField<>(fx.obj1, ::g_cjStoreGoodMask).GetFieldValue();
    buffer.add(slot, previous);
    GC_EXPECT_EQ(markFixture.YoungPending(), 0u);
    GC_EXPECT_EQ(buffer.Pending(), 1u);
    buffer.Flush();
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

// ZMark::mark_object policy matrix. These calls enter the actual product
// ZGeneration template instantiations, not a test-compiled mark body.
namespace {
void CallMarkObjectIfActive(ZGeneration& cycle, zaddress address, bool resurrect, bool gcThread,
                            bool follow, bool finalizable)
{
    if (!resurrect && !gcThread && !follow && !finalizable) {
        cycle.MarkObjectIfActive<false, false, false, false>(address);
    } else if (!resurrect && !gcThread && !follow && finalizable) {
        cycle.MarkObjectIfActive<false, false, false, true>(address);
    } else if (!resurrect && !gcThread && follow && !finalizable) {
        cycle.MarkObjectIfActive<false, false, true, false>(address);
    } else if (!resurrect && !gcThread && follow && finalizable) {
        cycle.MarkObjectIfActive<false, false, true, true>(address);
    } else if (!resurrect && gcThread && !follow && !finalizable) {
        cycle.MarkObjectIfActive<false, true, false, false>(address);
    } else if (!resurrect && gcThread && !follow && finalizable) {
        cycle.MarkObjectIfActive<false, true, false, true>(address);
    } else if (!resurrect && gcThread && follow && !finalizable) {
        cycle.MarkObjectIfActive<false, true, true, false>(address);
    } else if (!resurrect && gcThread && follow && finalizable) {
        cycle.MarkObjectIfActive<false, true, true, true>(address);
    } else if (resurrect && !gcThread && !follow && !finalizable) {
        cycle.MarkObjectIfActive<true, false, false, false>(address);
    } else if (resurrect && !gcThread && !follow && finalizable) {
        cycle.MarkObjectIfActive<true, false, false, true>(address);
    } else if (resurrect && !gcThread && follow && !finalizable) {
        cycle.MarkObjectIfActive<true, false, true, false>(address);
    } else if (resurrect && !gcThread && follow && finalizable) {
        cycle.MarkObjectIfActive<true, false, true, true>(address);
    } else if (resurrect && gcThread && !follow && !finalizable) {
        cycle.MarkObjectIfActive<true, true, false, false>(address);
    } else if (resurrect && gcThread && !follow && finalizable) {
        cycle.MarkObjectIfActive<true, true, false, true>(address);
    } else if (resurrect && gcThread && follow && !finalizable) {
        cycle.MarkObjectIfActive<true, true, true, false>(address);
    } else {
        cycle.MarkObjectIfActive<true, true, true, true>(address);
    }
}
}

GC_TEST(P1Mark, AllocatingAndRelocatablePolicyMatrix)
{
    for (bool young : {false, true}) {
        for (bool gcThread : {false, true}) {
            for (bool follow : {false, true}) {
                for (bool finalizable : {false, true}) {
                    if (young && finalizable) continue;
                    GcHeapFixture fx;
                    MarkPublicationFixture publication;
                    auto& cycle = publication.collector.GetZGeneration(
                        young ? ZGenerationId::young : ZGenerationId::old);
                    fx.region0->reset(young ? PageAge::eden : PageAge::old);
                    fx.region0->ResetPageSequence();
                    CallMarkObjectIfActive(cycle, from_object(fx.obj0), false, gcThread, follow, finalizable);
                    const size_t pending = young ? publication.YoungPending() : publication.OldPending();
                    std::fprintf(stderr, "P1_ALLOCATING_ASSERT young=%d gc=%d follow=%d finalizable=%d pending=%zu live=%zu\n",
                                 young, gcThread, follow, finalizable, pending,
                                 static_cast<size_t>(fx.region0->live_bytes()));
                    GC_EXPECT_EQ(pending, 0u);
                    GC_EXPECT_FALSE(fx.region0->livemap().is_marked(fx.region0->generation_id()));
                    GcHeapFixture::AdvanceGeneration(young ? Generation::Young : Generation::Old);
                    cycle.PublishPhase(ZGenerationPhase::Mark);
                    CallMarkObjectIfActive(cycle, from_object(fx.obj0), false, gcThread, follow, finalizable);
                    ZMark& domain = young ? *publication.collector.YoungMark()
                                              : *publication.collector.MajorMark();
                    ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), domain);
                    MarkStackEntry entry;
                    size_t entries = 0;
                    for (size_t stripe = 0; stripe < domain.Stripes().Count(); ++stripe) {
                        WorkerFixture worker;
                        while (domain.Stacks().Pop(domain.Smr(), 0, domain.Stripes(), stripe, entry)) {
                            ++entries;
                            GC_EXPECT_TRUE(to_object(ZOffset::address(to_zoffset(entry.object_address()))) == fx.obj0);
                            GC_EXPECT_EQ(entry.object_address(), untype(ZAddress::offset(from_object(fx.obj0))));
                            GC_EXPECT_EQ(entry.mark(), !gcThread);
                            GC_EXPECT_EQ(entry.inc_live(), gcThread);
                            GC_EXPECT_EQ(entry.follow(), follow);
                            GC_EXPECT_EQ(entry.finalizable(), finalizable);
                        }
                    }
                    std::fprintf(stderr, "P1_RELOCATABLE_ASSERT young=%d gc=%d follow=%d finalizable=%d entries=%zu\n",
                                 young, gcThread, follow, finalizable, entries);
                    GC_EXPECT_EQ(entries, 1u);
                }
            }
        }
    }
}

GC_OTHER_VM_TEST(P1Mark, DuplicateAnyThreadStopsAtConsumer)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    ZStat::Initialize();
    MutatorManager manager;
    YoungConcTestRuntime runtime(manager);
    GcHeapFixture fx;
    MarkPublicationFixture publication;
    fx.region0->reset(PageAge::eden);
    fx.region0->ResetPageSequence();
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    auto& cycle = publication.collector.GetZGeneration(ZGenerationId::young);
    cycle.PublishPhase(ZGenerationPhase::Mark);
    CallMarkObjectIfActive(cycle, from_object(fx.obj0), false, false, false, false);
    CallMarkObjectIfActive(cycle, from_object(fx.obj0), false, false, false, false);
    WorkStack work;
    std::vector<BaseObject*> reached;
    publication.FollowYoung(work, reached);
    std::fprintf(stderr, "P1_CONSUMER_ASSERT reached=%zu live=%zu marked=%d\n", reached.size(),
                 static_cast<size_t>(fx.region0->live_bytes()),
                 fx.region0->livemap().is_marked(fx.region0->generation_id()) ? 1 : 0);
    GC_EXPECT_TRUE(fx.region0->livemap().is_marked(fx.region0->generation_id()));
    GC_EXPECT_EQ(fx.region0->live_bytes(), fx.obj0->GetSize());
}


GC_TEST(P1Mark, ResurrectAndInactivePhasePolicies)
{
    GcHeapFixture fx;
    MarkPublicationFixture publication;
    auto& cycle = publication.collector.GetZGeneration(ZGenerationId::old);
    auto& domain = *publication.collector.MajorMark();
    fx.region0->reset(PageAge::old);
    fx.region0->ResetPageSequence();
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    cycle.PublishPhase(ZGenerationPhase::MarkComplete);
    CallMarkObjectIfActive(cycle, from_object(fx.obj0), true, true, true, false);
    GC_EXPECT_EQ(publication.OldPending(), 0u);
    GC_EXPECT_FALSE(domain.Terminate().Resurrected());
    cycle.PublishPhase(ZGenerationPhase::Mark);
    CallMarkObjectIfActive(cycle, from_object(fx.obj0), true, true, true, false);
    std::fprintf(stderr, "P1_RESURRECT_ASSERT pending=%zu resurrected=%d\n",
                 publication.OldPending(), domain.Terminate().Resurrected());
    GC_EXPECT_EQ(publication.OldPending(), 1u);
    GC_EXPECT_TRUE(domain.Terminate().Resurrected());
    domain.Terminate().SetResurrected(false);
    CallMarkObjectIfActive(cycle, from_object(fx.obj0), true, true, true, false);
    GC_EXPECT_EQ(publication.OldPending(), 1u);
    GC_EXPECT_FALSE(domain.Terminate().Resurrected());
}

#endif // MRT_TESTABLE_INTERNALS
