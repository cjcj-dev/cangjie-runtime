#include "Heap/Collector/FinalizerProcessor.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Mutator/Mutator.h"

#include <condition_variable>
#include <mutex>
#include <thread>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_OTHER_VM_TEST(FnlzRoots, RegistrationTransferPreservesSlotAndYoungEpoch)
{
    // A native handle is stored at creation, not recolored when its owning
    // list changes: weakHandle.cpp:39-50, zBarrierSet.inline.hpp:258-265.
    Mutator mutator;
    FinalizerProcessor processor;
    alignas(8) unsigned char storage[16] = {};
    mutator.AddLocalFinalizer(reinterpret_cast<BaseObject*>(storage));
    auto& local = mutator.GetLocalFinalizers();
    NativeSlot* const originalSlot = &local.front();
    const zpointer originalWord = originalSlot->GetFieldValue();
    struct RestoreMasks {
        unsigned long mark = ::g_cjMarkBadMask;
        unsigned long store = ::g_cjStoreGoodMask;
        unsigned long bad = ::g_cjStoreBadMask;
        ~RestoreMasks()
        {
            ::g_cjMarkBadMask = mark;
            ::g_cjStoreGoodMask = store;
            ::g_cjStoreBadMask = bad;
        }
    } masks;
    ::g_cjMarkBadMask ^= MARKED_YOUNG_MASK;
    ::g_cjStoreGoodMask ^= MARKED_YOUNG_MASK;
    ::g_cjStoreBadMask ^= MARKED_YOUNG_MASK;
    processor.RegisterFinalizers(local);
    size_t seen = 0;
    processor.VisitFinalizers([&](NativeSlot& slot) {
        ++seen;
        std::fprintf(stderr, "B19_REGISTRATION_EPOCH_ASSERT observed=%#lx expected=%#lx\n",
                     raw(slot.GetFieldValue()), raw(originalWord));
        GC_EXPECT_EQ(raw(slot.GetFieldValue()), raw(originalWord));
        GC_EXPECT_TRUE(&slot == originalSlot);
        GC_EXPECT_FALSE(ColourPredicates::is_marked_young(raw(slot.GetFieldValue()), ::g_cjMarkBadMask));
    });
    GC_EXPECT_EQ(seen, size_t(1));
    GC_EXPECT_TRUE(local.empty());
}

GC_TEST(FnlzRoots, RegisteredFinalizerIsRawPointerButNotStrongRoot)
{
    FinalizerProcessor fp;
    alignas(8) unsigned char storage[16] = {};
    auto* obj = reinterpret_cast<BaseObject*>(storage);
    fp.RegisterFinalizer(obj);

    size_t strongRoots = 0;
    fp.VisitGCRoots([&](NativeSlot&) { ++strongRoots; });
    GC_EXPECT_EQ(strongRoots, static_cast<size_t>(0));

    size_t rawPointers = 0;
    fp.VisitNativePointers([&](NativeSlot&) { ++rawPointers; });
    GC_EXPECT_EQ(rawPointers, static_cast<size_t>(1));
}

GC_TEST(FnlzRoots, VisitFinalizersCountMatchesRegister)
{
    FinalizerProcessor fp;
    alignas(8) unsigned char a[16] = {};
    alignas(8) unsigned char b[16] = {};
    fp.RegisterFinalizer(reinterpret_cast<BaseObject*>(a));
    fp.RegisterFinalizer(reinterpret_cast<BaseObject*>(b));

    U32 finalizers = fp.VisitFinalizers([](NativeSlot&) {});
    GC_EXPECT_EQ(finalizers, static_cast<U32>(2));
}

GC_OTHER_VM_TEST(FnlzRoots, RegistryMissDoesNotCountAsFinalEnqueue)
{
    GcHeapFixture fx;
    // ZReferenceProcessor::is_strongly_live (zReferenceProcessor.cpp:157):
    // reference processing operates on objects belonging to the installed heap.
    // This test owns the synthetic reservation only inside its child VM.
    Heap::OnHeapCreated(fx.heapStart);
    Heap::OnHeapExtended(fx.heapStart + GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    GC_EXPECT_TRUE(Heap::IsHeapAddress(fx.obj0));
    FinalizerProcessor fp;
    ReferenceProcessor& processor = fp.GetReferenceProcessor();
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(fx.region0->ResurrectObject(fx.obj0, offset));
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::FINAL) ==
                   ReferenceStatus::DISCOVERED);

    fp.ProcessReferences([](BaseObject*) { return false; });
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(0));
    fp.EnqueueReferences();

    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(0));
    size_t queuedRoots = 0;
    fp.VisitGCRoots([&](NativeSlot&) { ++queuedRoots; });
    GC_EXPECT_EQ(queuedRoots, static_cast<size_t>(0));
}

GC_OTHER_VM_TEST(FnlzRoots, RegisteredFinalizerMovesAndCountsExactlyOnce)
{
    GcHeapFixture fx;
    // ZReferenceProcessor::is_strongly_live (zReferenceProcessor.cpp:157):
    // reference processing operates on objects belonging to the installed heap.
    // This test owns the synthetic reservation only inside its child VM.
    Heap::OnHeapCreated(fx.heapStart);
    Heap::OnHeapExtended(fx.heapStart + GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    GC_EXPECT_TRUE(Heap::IsHeapAddress(fx.obj0));
    FinalizerProcessor fp;
    ReferenceProcessor& processor = fp.GetReferenceProcessor();
    fp.RegisterFinalizer(fx.obj0);
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(fx.region0->ResurrectObject(fx.obj0, offset));
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::FINAL) ==
                   ReferenceStatus::DISCOVERED);

    fp.ProcessReferences([](BaseObject*) { return false; });
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(0));
    fp.EnqueueReferences();

    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(1));
    size_t queuedRoots = 0;
    fp.VisitGCRoots([&](NativeSlot&) { ++queuedRoots; });
    GC_EXPECT_EQ(queuedRoots, static_cast<size_t>(1));
    GC_EXPECT_EQ(fp.VisitFinalizers([](NativeSlot&) {}), static_cast<U32>(0));
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(FnlzRoots, EnqueueBetweenIdleCheckAndCommitKeepsJobVisible)
{
    FinalizerProcessor fp;
    alignas(8) unsigned char storage[16] = {};
    auto* obj = reinterpret_cast<BaseObject*>(storage);
    std::mutex lock;
    std::condition_variable condition;
    bool workerAtOldEmptyToClearGap = false;
    bool releaseWorker = false;

    fp.SetBeforeFinalizableIdleCheckForTest([&] {
        std::unique_lock<std::mutex> guard(lock);
        workerAtOldEmptyToClearGap = true;
        condition.notify_one();
        condition.wait(guard, [&] { return releaseWorker; });
    });

    std::thread worker([&] { fp.FinishFinalizableBatchForTest(); });
    {
        std::unique_lock<std::mutex> guard(lock);
        condition.wait(guard, [&] { return workerAtOldEmptyToClearGap; });
    }

    // This is the review's exact old :303 -> enqueue -> :304 ordering.
    fp.EnqueueFinalizableForTest(obj);
    {
        std::lock_guard<std::mutex> guard(lock);
        releaseWorker = true;
    }
    condition.notify_one();
    worker.join();

    GC_EXPECT_TRUE(fp.HasFinalizableJobForTest());
    size_t queuedRoots = 0;
    fp.VisitGCRoots([&](NativeSlot&) { ++queuedRoots; });
    GC_EXPECT_EQ(queuedRoots, static_cast<size_t>(1));
}
#endif
