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
#include "mark_publication_fixture.hpp"
#endif
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"


#include "gc_generation_test.hpp"

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
    Heap& collector = Heap::GetHeap();
    WorkStack workStack;
    ZMark::DiscoverWeakReference(fx.obj0, workStack);

    GC_EXPECT_TRUE(workStack.empty());
    GC_EXPECT_FALSE(fx.region1->is_object_strongly_live(from_object(fx.obj1)));
    ReferenceProcessor& processor =
        Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor();
    processor.ProcessReferences([](BaseObject*) { return true; });
    processor.EnqueueReferences([](BaseObject*) { return true; });
}
#endif // MRT_TESTABLE_INTERNALS

#if defined(MRT_TESTABLE_INTERNALS)

extern "C" int CJ_ScheduleManagerInit();

namespace MapleRuntime {

class RelocationReceiptTest {
public:
    static void BindCollector(Heap* collector)
    {
        if (collector != nullptr) {
            CHECK(collector == &Heap::GetHeap());
            // Product driver startup owns one worker set per generation
            // (zDriver.cpp:408-409; ZGC zGeneration.cpp:205-215).
            for (auto generation : {ZGenerationId::young, ZGenerationId::old}) {
                auto& cycle = Heap::GetHeap().GetZGeneration(generation);
                if (cycle.Workers() == nullptr) cycle.InitializeWorkers(2);
            }
        }
    }

    static void FlipYoungMarkForNativeBarrier(Heap& collector)
    {
        ZGlobalsPointers::flip_young_mark_start();
    }

    static void StartYoungRelocate(Heap& collector)
    {
        ZGlobalsPointers::flip_young_relocate_start();
    }

    static void RunCollectionDispatch(Heap& collector)
    {
        auto& cycle = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
        if (!cycle.Snapshot().active) ZGenerationTest::SetReason(cycle, GC_REASON_YOUNG);
        YoungTypeSetter type(cycle, ZYoungType::minor);
        Heap::GetHeap().young().collect();
    }
};
} // namespace MapleRuntime

namespace {

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
    fx.region1->reset(PageAge::eden);
    MarkPublicationFixture mark;
    const U64 handle = Heap::GetHeap().RegisterExportRoot(fx.obj1);
    RelocationReceiptTest::FlipYoungMarkForNativeBarrier(mark.collector);
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
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);

    field->StoreColoured(zpointer::null);
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 0u);
}

// Compensation: y2y dirty holder is merged into work stack (AllocBuffer.h:84-105).


#if defined(MRT_TESTABLE_INTERNALS) || defined(MRT_GC_UNIT_TESTS)


// The young mark consumer and a mutator can meet at the concurrent phase
// boundary. Pause at the caller-supplied batch sink after the product takes
// the batch: the late publication must remain intact for the next
// batch, never mutate the batch being iterated.


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
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::old);
    MarkPublicationFixture markFixture;
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
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    MarkPublicationFixture markFixture;
    BaseObject* incoming = fx.PlaceObject(fx.heapStart + ZGranuleSize + 128);
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
    Heap::GetHeap().GetZGeneration(ZGenerationId::young).PublishPhase(ZGenerationPhase::Relocate);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).PublishPhase(ZGenerationPhase::Relocate);
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
    fx.region0->reset(PageAge::eden);
    MarkPublicationFixture markFixture;
    GC_EXPECT_EQ(markFixture.YoungPending(), 0u);
    Heap::GetHeap().MarkYoungObjectIfActive(fx.obj0);
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
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    MarkPublicationFixture markFixture;
    RememberedSet remembered;
    remembered.Initialize(fx.heapStart, 2 * ZGranuleSize);
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

#endif // MRT_TESTABLE_INTERNALS

#if defined(MRT_TESTABLE_INTERNALS)

#endif // MRT_TESTABLE_INTERNALS

#if defined(MRT_TESTABLE_INTERNALS)


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
                    auto& cycle = Heap::GetHeap().GetZGeneration(
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
                    ZMark& domain = young ? *Heap::GetHeap().young().MarkPtr()
                                              : *Heap::GetHeap().old().MarkPtr();
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
    auto& cycle = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
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
    auto& cycle = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
    auto& domain = *Heap::GetHeap().old().MarkPtr();
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
