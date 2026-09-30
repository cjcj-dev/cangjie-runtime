// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <thread>
#include <unordered_set>
#include "Heap/z/zReferenceProcessor.hpp"
#include "Heap/z/zResurrection.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/workerThread.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "gc_unittest.hpp"
#include "reference_layout_fixture.hpp"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace {
struct BoundRefProc {
    GcHeapFixture heap;
    ReferenceLayoutFixture layout;
    ZStatWorkers stats;
    MapleRuntime::GcUnit::WorkerBudgetFixture budget{2};
    ZWorkers pool;
    ReferenceProcessor processor;
    BaseObject* reference;
    explicit BoundRefProc(bool final = false)
        : layout(final), pool(ZGenerationId::old, &stats), processor(&pool),
          reference(layout.Place(reinterpret_cast<MAddress>(heap.obj0), heap.obj1))
    {
        ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
        // The fixture supplies an old mark epoch. Discovery/processing itself
        // is the linked product implementation, not a liveness callback model.
        ZGlobalsPointers::flip_old_mark_start();
        if (final) {
            GcHeapFixture::MarkFinalizable(heap.region1(), heap.obj1);
            auto* slot = MReference::referent_addr(reference);
            slot->StoreColoured(ZBarrier::ColorFinalizableGood(from_object(heap.obj1), slot->GetFieldValue()));
        }
    }
    ~BoundRefProc() { ZResurrection::unblock(); ZGlobalsPointers::flip_old_mark_start(); }
    BaseObject* Process()
    {
        ZResurrection::block();
        processor.process_references();
        processor.enqueue_references();
        return Heap::GetHeap().GetFinalizerProcessor().SwapPendingList(nullptr);
    }
};
void CheckFinalQueue(bool nonWorker)
{
    BoundRefProc fixture(true);
    {
        WorkerFixture worker(0);
        GC_EXPECT_TRUE(fixture.processor.discover_reference(fixture.reference, ReferenceType::FINAL));
    }
    if (nonWorker) { GC_EXPECT_EQ(WorkerThread::worker_id(), UINT32_MAX); }
    BaseObject* pending = fixture.Process();
    const bool carrier = pending == fixture.reference;
    const bool inactive = MReference::next(fixture.reference) == fixture.reference;
    const bool retained = to_object(MReference::referent_addr(fixture.reference)->GetTargetObject()) == fixture.heap.obj1;
    std::fprintf(stderr, "REFERENCE1356_FINAL carrier=%d inactive=%d retained=%d enqueued=%zu\n",
                 carrier, inactive, retained, fixture.processor.Enqueued(ReferenceType::FINAL));
    GC_EXPECT_TRUE(carrier && inactive && retained);
    GC_EXPECT_EQ(fixture.processor.Enqueued(ReferenceType::FINAL), size_t(1));
    GC_EXPECT_TRUE(fixture.processor.Empty());
}
}
GC_OTHER_VM_TEST(ReferenceProcessor, UnsupportedKindsFailClosed)
{
    WorkerFixture worker(0);
    BoundRefProc fixture;
    GC_EXPECT_FALSE(fixture.processor.discover_reference(fixture.reference, ReferenceType::SOFT));
    GC_EXPECT_FALSE(fixture.processor.discover_reference(fixture.reference, ReferenceType::PHANTOM));
    GC_EXPECT_TRUE(fixture.processor.Empty());
    GC_EXPECT_EQ(fixture.processor.Discovered(ReferenceType::SOFT), size_t(0));
    GC_EXPECT_EQ(fixture.processor.Discovered(ReferenceType::PHANTOM), size_t(0));
}
GC_OTHER_VM_TEST(ReferenceProcessor, FinalDiscoveryProcessEnqueue) { CheckFinalQueue(false); }
GC_OTHER_VM_TEST(ReferenceProcessor, ProcessReferencesRunsOnBoundWorkers) { CheckFinalQueue(false); }
GC_OTHER_VM_TEST(ReferenceProcessor, ProcessReferencesFromNonWorkerCaller) { CheckFinalQueue(true); }
GC_OTHER_VM_TEST(ReferenceProcessor, StrongUpgradeDropsFinalReference)
{
    BoundRefProc fixture(true);
    WorkerFixture worker(0);
    GC_EXPECT_TRUE(fixture.processor.discover_reference(fixture.reference, ReferenceType::FINAL));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fixture.heap.region1(), fixture.heap.obj1));
    BaseObject* pending = fixture.Process();
    GC_EXPECT_TRUE(pending == nullptr);
    GC_EXPECT_TRUE(MReference::next(fixture.reference) == nullptr);
    GC_EXPECT_FALSE(RegionSpace::IsResurrectedObject(fixture.heap.obj1));
    GC_EXPECT_EQ(fixture.processor.Enqueued(ReferenceType::FINAL), size_t(0));
    GC_EXPECT_TRUE(fixture.processor.Empty());
}
GC_OTHER_VM_TEST(ReferenceProcessor, StrongWeakReferentIsNotCleared)
{
    BoundRefProc fixture;
    WorkerFixture worker(0);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fixture.heap.region1(), fixture.heap.obj1));
    // zReferenceProcessor.cpp:188: strong liveness rejects discovery itself.
    GC_EXPECT_FALSE(fixture.processor.discover_reference(fixture.reference, ReferenceType::WEAK));
    GC_EXPECT_TRUE(fixture.Process() == nullptr);
    GC_EXPECT_TRUE(to_object(MReference::referent_addr(fixture.reference)->GetTargetObject()) == fixture.heap.obj1);
    GC_EXPECT_EQ(fixture.processor.Enqueued(ReferenceType::WEAK), size_t(0));
}
GC_OTHER_VM_TEST(ReferenceProcessor, DeadWeakReferentIsCleanedByCas)
{
    BoundRefProc fixture;
    WorkerFixture worker(0);
    GC_EXPECT_TRUE(fixture.processor.discover_reference(fixture.reference, ReferenceType::WEAK));
    ZResurrection::block();
    fixture.processor.process_references();
    const bool clearedBeforeEnqueue = is_null(MReference::referent_addr(fixture.reference)->GetTargetObject());
    fixture.processor.enqueue_references();
    BaseObject* pending = Heap::GetHeap().GetFinalizerProcessor().SwapPendingList(nullptr);
    const bool cleared = is_null(MReference::referent_addr(fixture.reference)->GetTargetObject());
    std::fprintf(stderr, "REFERENCE1356_WEAK before_enqueue=%d cleared=%d enqueued=%zu\n", clearedBeforeEnqueue,
                 cleared, fixture.processor.Enqueued(ReferenceType::WEAK));
    GC_EXPECT_TRUE(clearedBeforeEnqueue && cleared && pending == fixture.reference);
    GC_EXPECT_EQ(fixture.processor.Enqueued(ReferenceType::WEAK), size_t(1));
}
GC_OTHER_VM_TEST(ReferenceProcessor, ProcessConsumerPreservesReplacementReferent)
{
    BoundRefProc fixture;
    WorkerFixture worker(0);
    GC_EXPECT_TRUE(fixture.processor.discover_reference(fixture.reference, ReferenceType::WEAK));
    BaseObject* replacement = fixture.heap.PlaceObject(fixture.heap.heapStart + 128);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fixture.heap.region0(), replacement));
    // Mutator replacement between discovery and processing is a real slot
    // input. The deleted callback-injected mid-load race has no product API.
    MReference::referent_addr(fixture.reference)->StoreColoured(StoreGoodPointer(replacement));
    GC_EXPECT_TRUE(fixture.Process() == nullptr);
    GC_EXPECT_TRUE(to_object(MReference::referent_addr(fixture.reference)->GetTargetObject()) == replacement);
    GC_EXPECT_EQ(fixture.processor.Enqueued(ReferenceType::WEAK), size_t(0));
}
GC_OTHER_VM_TEST(ReferenceProcessor, ConcurrentWorkersPublishOnePendingList)
{
    BoundRefProc fixture(true);
    constexpr size_t count = 32;
    std::vector<BaseObject*> references;
    for (size_t i = 0; i < count; ++i) {
        BaseObject* target = fixture.heap.PlaceObject(fixture.heap.heapStart + ZGranuleSize + 128 + i * 64);
        GC_EXPECT_TRUE(GcHeapFixture::MarkFinalizable(fixture.heap.region1(), target));
        auto* reference = fixture.layout.Place(fixture.heap.heapStart + 128 + i * 64, target);
        auto* slot = MReference::referent_addr(reference);
        slot->StoreColoured(ZAddress::finalizable_good(from_object(target), slot->GetFieldValue()));
        references.push_back(reference);
    }
    fixture.heap.region0()->SetRegionAllocPtr(fixture.heap.heapStart + 128 + count * 64);
    fixture.heap.region1()->SetRegionAllocPtr(fixture.heap.heapStart + ZGranuleSize + 128 + count * 64);
    std::vector<std::thread> workers;
    for (size_t id = 0; id < 8; ++id) {
        workers.emplace_back([&, id] {
            ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
            WorkerFixture worker(id);
            for (size_t i = id; i < count; i += 8) {
                (void)fixture.processor.discover_reference(references[i], ReferenceType::FINAL);
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    std::unordered_set<BaseObject*> pending;
    for (BaseObject* p = fixture.Process(); p != nullptr; p = MReference::discovered(p)) {
        GC_EXPECT_TRUE(pending.insert(p).second);
    }
    std::fprintf(stderr, "REFERENCE1356_PENDING_LIST actual=%zu expected=%zu\n", pending.size(), count);
    GC_EXPECT_EQ(pending.size(), count);
    for (BaseObject* reference : references) { GC_EXPECT_TRUE(pending.count(reference) == 1); }
    GC_EXPECT_EQ(fixture.processor.Discovered(ReferenceType::FINAL), count);
    GC_EXPECT_TRUE(fixture.processor.Empty());
}
