// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <atomic>
#include <thread>
#include <vector>

#include "Heap/z/zReferenceProcessor.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/workerThread.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_worker_fixture.hpp"
#include "gc_unittest.hpp"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
struct BoundRefProc {
    ZStatWorkers stats;
    ZWorkers pool;
    ReferenceProcessor processor;
    BoundRefProc() : pool(ZGenerationId::old, 2, &stats), processor(&pool) {}
};
}

GC_TEST(ReferenceProcessor, UnsupportedKindsFailClosed)
{
    WorkerFixture worker(0);
    BoundRefProc bound;
    ReferenceProcessor& processor = bound.processor;
    alignas(8) unsigned char storage[16] = {};
    auto* object = reinterpret_cast<BaseObject*>(storage);

    GC_EXPECT_TRUE(!processor.DiscoverReference(object, ReferenceType::SOFT));
    GC_EXPECT_TRUE(!processor.DiscoverReference(object, ReferenceType::PHANTOM));
    GC_EXPECT_TRUE(processor.Empty());
    GC_EXPECT_EQ(processor.Discovered(ReferenceType::SOFT), static_cast<size_t>(0));
    GC_EXPECT_EQ(processor.Discovered(ReferenceType::PHANTOM), static_cast<size_t>(0));
}

GC_TEST(ReferenceProcessor, FinalDiscoveryProcessEnqueue)
{
    WorkerFixture worker(0);
    GcHeapFixture fx;
    BoundRefProc bound;
    ReferenceProcessor& processor = bound.processor;
    GC_EXPECT_TRUE(GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0));
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::FINAL));

    processor.ProcessReferences([](BaseObject*) { return false; });
    BaseObject* enqueued = nullptr;
    processor.EnqueueReferences([&](BaseObject* value) {
        enqueued = value;
        return true;
    });

    GC_EXPECT_TRUE(enqueued == fx.obj0);
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(1));
    GC_EXPECT_TRUE(processor.Empty());
}

// Native finalizer registrations have no Java discovered field. Re-visiting
// the same referent must retain one original discovery lifecycle.
GC_TEST(ReferenceProcessor, FinalDiscoveryIsClaimedOnce)
{
    WorkerFixture worker(0);
    GcHeapFixture fx;
    BoundRefProc bound;
    ReferenceProcessor& processor = bound.processor;
    GC_EXPECT_TRUE(GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0));
    (void)processor.DiscoverReference(fx.obj0, ReferenceType::FINAL);
    (void)processor.DiscoverReference(fx.obj0, ReferenceType::FINAL);
    const size_t discovered = processor.Discovered(ReferenceType::FINAL);
    processor.ProcessReferences([](BaseObject*) { return false; });
    size_t queued = 0;
    processor.EnqueueReferences([&](BaseObject*) { ++queued; return true; });
    std::fprintf(stderr, "P2_DISCOVERY_RESULT discovered=%zu queued=%zu\n", discovered, queued);
    GC_EXPECT_EQ(discovered, size_t(1));
    GC_EXPECT_EQ(queued, size_t(1));
}

GC_TEST(ReferenceProcessor, StrongUpgradeDropsFinalReference)
{
    WorkerFixture worker(0);
    GcHeapFixture fx;
    GC_EXPECT_TRUE(GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region0, fx.obj0));

    BoundRefProc bound;
    ReferenceProcessor& processor = bound.processor;
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::FINAL));
    processor.ProcessReferences([](BaseObject*) { return false; });
    size_t enqueued = 0;
    processor.EnqueueReferences([&](BaseObject*) {
        ++enqueued;
        return true;
    });

    GC_EXPECT_EQ(enqueued, static_cast<size_t>(0));
    GC_EXPECT_FALSE(RegionSpace::IsResurrectedObject(fx.obj0));
    GC_EXPECT_TRUE(processor.Empty());
}

GC_TEST(ReferenceProcessor, StrongWeakReferentIsNotCleared)
{
    WorkerFixture worker(0);
    GcHeapFixture fx;
    HeapSlot<>& referent =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(GcUnit::StoreGoodPointer(fx.obj1));

    BoundRefProc bound;
    ReferenceProcessor& processor = bound.processor;
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::WEAK));
    processor.ProcessReferences([&](BaseObject* value) { return value == fx.obj1; });
    processor.EnqueueReferences([](BaseObject*) { return true; });

    GC_EXPECT_TRUE(to_object(referent.GetTargetObject()) == fx.obj1);
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::WEAK), static_cast<size_t>(0));
}

GC_TEST(ReferenceProcessor, DeadWeakReferentIsCleanedByCas)
{
    WorkerFixture worker(0);
    GcHeapFixture fx;
    HeapSlot<>& referent =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(GcUnit::StoreGoodPointer(fx.obj1));

    BoundRefProc bound;
    ReferenceProcessor& processor = bound.processor;
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::WEAK));
    processor.ProcessReferences([](BaseObject*) { return false; });
    // Weak clearing precedes the rendezvous/unblock/enqueue boundary.
    GC_EXPECT_TRUE(is_null(referent.GetTargetObject()));
    processor.EnqueueReferences([](BaseObject*) { return true; });

    GC_EXPECT_TRUE(is_null(referent.GetTargetObject()));
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::WEAK), static_cast<size_t>(1));
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(ReferenceProcessor, ProcessConsumerReloadsWinningWeakCasValue)
{
    WorkerFixture worker(0);
    GcHeapFixture fx;
    BaseObject* replacement = fx.PlaceObject(fx.heapStart + 128);
    HeapSlot<>& referent =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(GcUnit::StoreGoodPointer(fx.obj1));

    BoundRefProc bound;
    ReferenceProcessor& processor = bound.processor;
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::WEAK));
    ReferenceProcessor::SetBeforeWeakCleanCasForTest([&] {
        referent.StoreColoured(GcUnit::StoreGoodPointer(replacement));
    });
    BaseObject* consumerTerminal = nullptr;
    processor.ProcessReferences([](BaseObject*) { return false; },
        [&](BaseObject*, BaseObject* terminal) { consumerTerminal = terminal; });
    ReferenceProcessor::SetBeforeWeakCleanCasForTest({});
    processor.EnqueueReferences([](BaseObject*) { return true; });

    GC_EXPECT_TRUE(consumerTerminal == replacement);
    GC_EXPECT_TRUE(to_object(referent.GetTargetObject()) == replacement);
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::WEAK), static_cast<size_t>(0));
}
#endif

GC_TEST(ReferenceProcessor, DuplicateWeakPendingAcceptedOnce)
{
    WorkerFixture worker(0);
    GcHeapFixture fx;
    HeapSlot<>& referent =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(GcUnit::StoreGoodPointer(fx.obj1));

    BoundRefProc bound;
    ReferenceProcessor& processor = bound.processor;
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::WEAK));
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::WEAK));
    processor.ProcessReferences([](BaseObject*) { return false; });
    processor.EnqueueReferences([](BaseObject*) { return true; });

    GC_EXPECT_TRUE(is_null(referent.GetTargetObject()));
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::WEAK), static_cast<size_t>(1));
    GC_EXPECT_TRUE(processor.Empty());
}

GC_TEST(ReferenceProcessor, ProcessReferencesRunsOnBoundWorkers)
{
    ZStatWorkers stats;
    ZWorkers pool(ZGenerationId::old, 2, &stats);
    WorkerFixture worker(0);
    GcHeapFixture fx;
    ReferenceProcessor processor(&pool);
    GC_EXPECT_TRUE(GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0));
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::FINAL));
    processor.ProcessReferences([](BaseObject*) { return false; });
    BaseObject* enqueued = nullptr;
    processor.EnqueueReferences([&](BaseObject* value) {
        enqueued = value;
        return true;
    });
    std::fprintf(stderr, "REFPROC_BOUND_WORKERS enqueued=%p obj0=%p\n", enqueued, fx.obj0);
    GC_EXPECT_TRUE(enqueued == fx.obj0);
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(1));
    GC_EXPECT_TRUE(processor.Empty());
}

GC_TEST(ReferenceProcessor, ConcurrentWorkersPublishOnePendingList)
{
    WorkerFixture worker(0);
    GcHeapFixture fx;
    BoundRefProc bound;
    ReferenceProcessor& processor = bound.processor;
    constexpr size_t kWorkers = 8;
    constexpr size_t kPerWorker = 4;
    std::vector<BaseObject*> objects;
    objects.reserve(kWorkers * kPerWorker);
    for (size_t index = 0; index < kWorkers * kPerWorker; ++index) {
        BaseObject* object = fx.PlaceObject(fx.heapStart + 64 + index * 64);
        objects.push_back(object);
        GC_EXPECT_TRUE(GcHeapFixture::MarkFinalizable(fx.region0, object));
    }
    fx.region0->SetRegionAllocPtr(
        reinterpret_cast<MAddress>(objects.back()) + 64);

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            WorkerFixture id(static_cast<uint32_t>(worker));
            for (size_t i = 0; i < kPerWorker; ++i) {
                (void)processor.DiscoverReference(objects[worker * kPerWorker + i], ReferenceType::FINAL);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    processor.ProcessReferences([](BaseObject*) { return false; });
    std::atomic<size_t> count{ 0 };
    processor.EnqueueReferences([&](BaseObject*) {
        count.fetch_add(1, std::memory_order_relaxed);
        return true;
    });
    GC_EXPECT_EQ(count.load(std::memory_order_relaxed), kWorkers * kPerWorker);
    GC_EXPECT_EQ(processor.Discovered(ReferenceType::FINAL), kWorkers * kPerWorker);
    GC_EXPECT_TRUE(processor.Empty());
}

GC_TEST(ReferenceProcessor, ProcessReferencesFromNonWorkerCaller)
{
    BoundRefProc bound;
    GcHeapFixture fx;
    GC_EXPECT_TRUE(GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0));
    {
        WorkerFixture discover(0);
        GC_EXPECT_TRUE(bound.processor.DiscoverReference(fx.obj0, ReferenceType::FINAL));
    }
    GC_EXPECT_EQ(WorkerThread::worker_id(), UINT32_MAX);
    bound.processor.ProcessReferences([](BaseObject*) { return false; });
    BaseObject* enqueued = nullptr;
    bound.processor.EnqueueReferences([&](BaseObject* value) {
        enqueued = value;
        return true;
    });
    std::fprintf(stderr, "REFPROC_NON_WORKER_CALLER enqueued=%p obj0=%p caller_id=%u\n",
                 enqueued, fx.obj0, WorkerThread::worker_id());
    GC_EXPECT_TRUE(enqueued == fx.obj0);
    GC_EXPECT_EQ(bound.processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(1));
}
