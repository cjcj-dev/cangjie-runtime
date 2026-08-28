// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <atomic>
#include <thread>
#include <vector>

#include "Heap/Collector/ReferenceProcessor.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ReferenceProcessor, UnsupportedKindsFailClosed)
{
    ReferenceProcessor processor;
    alignas(8) unsigned char storage[16] = {};
    auto* object = reinterpret_cast<BaseObject*>(storage);

    GC_EXPECT_TRUE(processor.DiscoverReference(object, ReferenceType::SOFT) ==
                   ReferenceStatus::UNSUPPORTED);
    GC_EXPECT_TRUE(processor.DiscoverReference(object, ReferenceType::PHANTOM) ==
                   ReferenceStatus::UNSUPPORTED);
    GC_EXPECT_TRUE(processor.Empty());
    GC_EXPECT_EQ(processor.Discovered(ReferenceType::SOFT), static_cast<size_t>(0));
    GC_EXPECT_EQ(processor.Discovered(ReferenceType::PHANTOM), static_cast<size_t>(0));
}

GC_TEST(ReferenceProcessor, FinalDiscoveryProcessEnqueue)
{
    GcHeapFixture fx;
    ReferenceProcessor processor;
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_FALSE(fx.region0->ResurrectObject(fx.obj0, offset));
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::FINAL) ==
                   ReferenceStatus::DISCOVERED);

    processor.ProcessReferences([](BaseObject*) { return false; });
    BaseObject* enqueued = nullptr;
    processor.EnqueueReferences([&](BaseObject* value) { enqueued = value; });

    GC_EXPECT_TRUE(enqueued == fx.obj0);
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(1));
    GC_EXPECT_TRUE(processor.Empty());
}

GC_TEST(ReferenceProcessor, StrongUpgradeDropsFinalReference)
{
    GcHeapFixture fx;
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_FALSE(fx.region0->ResurrectObject(fx.obj0, offset));
    GC_EXPECT_FALSE(fx.region0->MarkObject(
        fx.region0->GetMarkView<Generation::Old>(), fx.obj0, fx.obj0->GetSize()));

    ReferenceProcessor processor;
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::FINAL) ==
                   ReferenceStatus::DISCOVERED);
    processor.ProcessReferences([](BaseObject*) { return false; });
    size_t enqueued = 0;
    processor.EnqueueReferences([&](BaseObject*) { ++enqueued; });

    GC_EXPECT_EQ(enqueued, static_cast<size_t>(0));
    GC_EXPECT_FALSE(fx.region0->IsResurrectedObject(fx.obj0));
    GC_EXPECT_TRUE(processor.Empty());
}

GC_TEST(ReferenceProcessor, StrongWeakReferentIsNotCleared)
{
    GcHeapFixture fx;
    HeapSlot<>& referent =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj1)));

    ReferenceProcessor processor;
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::WEAK) ==
                   ReferenceStatus::DISCOVERED);
    processor.ProcessReferences([&](BaseObject* value) { return value == fx.obj1; });
    processor.EnqueueReferences([](BaseObject*) {});

    GC_EXPECT_TRUE(to_object(referent.GetTargetObject()) == fx.obj1);
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::WEAK), static_cast<size_t>(0));
}

GC_TEST(ReferenceProcessor, DeadWeakReferentIsCleanedByCas)
{
    GcHeapFixture fx;
    HeapSlot<>& referent =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj1)));

    ReferenceProcessor processor;
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::WEAK) ==
                   ReferenceStatus::DISCOVERED);
    processor.ProcessReferences([](BaseObject*) { return false; });
    processor.EnqueueReferences([](BaseObject*) {});

    GC_EXPECT_TRUE(is_null(referent.GetTargetObject()));
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::WEAK), static_cast<size_t>(1));
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(ReferenceProcessor, EnqueueConsumerReloadsWinningWeakCasValue)
{
    GcHeapFixture fx;
    BaseObject* replacement = fx.PlaceObject(fx.heapStart + 128);
    HeapSlot<>& referent =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj1)));

    ReferenceProcessor processor;
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::WEAK) ==
                   ReferenceStatus::DISCOVERED);
    processor.ProcessReferences([](BaseObject*) { return false; });
    ReferenceProcessor::SetBeforeWeakCleanCasForTest([&] {
        referent.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(replacement)));
    });
    BaseObject* consumerTerminal = nullptr;
    processor.EnqueueReferences([](BaseObject*) {},
        [&](BaseObject*, BaseObject* terminal) { consumerTerminal = terminal; });
    ReferenceProcessor::SetBeforeWeakCleanCasForTest({});

    GC_EXPECT_TRUE(consumerTerminal == replacement);
    GC_EXPECT_TRUE(to_object(referent.GetTargetObject()) == replacement);
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::WEAK), static_cast<size_t>(0));
}
#endif

GC_TEST(ReferenceProcessor, DuplicateWeakPendingAcceptedOnce)
{
    GcHeapFixture fx;
    HeapSlot<>& referent =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj1)));

    ReferenceProcessor processor;
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::WEAK) ==
                   ReferenceStatus::DISCOVERED);
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::WEAK) ==
                   ReferenceStatus::DISCOVERED);
    processor.ProcessReferences([](BaseObject*) { return false; });
    processor.EnqueueReferences([](BaseObject*) {});

    GC_EXPECT_TRUE(is_null(referent.GetTargetObject()));
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::WEAK), static_cast<size_t>(1));
    GC_EXPECT_TRUE(processor.Empty());
}

GC_TEST(ReferenceProcessor, ConcurrentWorkersPublishOnePendingList)
{
    GcHeapFixture fx;
    ReferenceProcessor processor;
    constexpr size_t kWorkers = 8;
    constexpr size_t kPerWorker = 4;
    std::vector<BaseObject*> objects;
    objects.reserve(kWorkers * kPerWorker);
    for (size_t index = 0; index < kWorkers * kPerWorker; ++index) {
        BaseObject* object = fx.PlaceObject(fx.heapStart + 64 + index * 64);
        objects.push_back(object);
        const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(object));
        GC_EXPECT_FALSE(fx.region0->ResurrectObject(object, offset));
    }
    fx.region0->SetRegionAllocPtr(
        reinterpret_cast<MAddress>(objects.back()) + 64);

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
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
    processor.EnqueueReferences([&](BaseObject*) { count.fetch_add(1, std::memory_order_relaxed); });
    GC_EXPECT_EQ(count.load(std::memory_order_relaxed), kWorkers * kPerWorker);
    GC_EXPECT_EQ(processor.Discovered(ReferenceType::FINAL), kWorkers * kPerWorker);
    GC_EXPECT_TRUE(processor.Empty());
}
