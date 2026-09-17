#include "Heap/Collector/FinalizerProcessor.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Mutator/Mutator.h"

#include <algorithm>
#include <array>
#include <vector>
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
    auto& processor = Heap::GetHeap().GetFinalizerProcessor();
    alignas(8) unsigned char storage[16] = {};
    mutator.AddLocalFinalizer(reinterpret_cast<BaseObject*>(storage));
    auto& local = mutator.GetLocalFinalizers();
    NativeSlot* const originalSlot = &local.front();
    const zpointer originalWord = originalSlot->GetFieldValue();
    ZGlobalsPointers::flip_young_mark_start();
    processor.RegisterFinalizers(local);
    size_t seen = 0;
    processor.VisitFinalizers([&](NativeSlot& slot) {
        ++seen;
        std::fprintf(stderr, "B19_REGISTRATION_EPOCH_ASSERT observed=%#lx expected=%#lx\n",
                     raw(slot.GetFieldValue()), raw(originalWord));
        GC_EXPECT_EQ(raw(slot.GetFieldValue()), raw(originalWord));
        GC_EXPECT_TRUE(&slot == originalSlot);
        GC_EXPECT_FALSE(ZPointer::is_marked_young(to_zpointer(raw(slot.GetFieldValue()))));
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
    Heap::OnHeapExtended(fx.heapStart + GcHeapFixture::kUnits * ZPage::UNIT_SIZE);
    GC_EXPECT_TRUE(Heap::IsHeapAddress(fx.obj0));
    FinalizerProcessor fp;
    ReferenceProcessor& processor = fp.GetReferenceProcessor();
    GC_EXPECT_TRUE(GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0));
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::FINAL));

    fp.ProcessReferences([](BaseObject*) { return false; });
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(1));
    fp.EnqueueReferences();

    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(1));
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
    Heap::OnHeapExtended(fx.heapStart + GcHeapFixture::kUnits * ZPage::UNIT_SIZE);
    GC_EXPECT_TRUE(Heap::IsHeapAddress(fx.obj0));
    FinalizerProcessor fp;
    ReferenceProcessor& processor = fp.GetReferenceProcessor();
    fp.RegisterFinalizer(fx.obj0);
    GC_EXPECT_TRUE(GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0));
    GC_EXPECT_TRUE(processor.DiscoverReference(fx.obj0, ReferenceType::FINAL));

    fp.ProcessReferences([](BaseObject*) { return false; });
    GC_EXPECT_EQ(processor.Enqueued(ReferenceType::FINAL), static_cast<size_t>(1));
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

GC_OTHER_VM_TEST(FnlzRoots, SharedBlockHandlesSurviveGrowthAndTransfer)
{
    Mutator mutator;
    auto& processor = Heap::GetHeap().GetFinalizerProcessor();
    alignas(8) unsigned char objects[130][16] = {};
    std::vector<NativeSlot*> slots;
    std::vector<zpointer> words;
    for (auto& object : objects) {
        mutator.AddLocalFinalizer(reinterpret_cast<BaseObject*>(object));
        slots.push_back(&mutator.GetLocalFinalizers().back());
        words.push_back(slots.back()->GetFieldValue());
    }
    processor.RegisterFinalizers(mutator.GetLocalFinalizers());
    size_t observed = 0;
    processor.VisitFinalizers([&](NativeSlot& slot) {
        auto found = std::find(slots.begin(), slots.end(), &slot);
        if (found == slots.end()) { return; }
        const size_t index = static_cast<size_t>(found - slots.begin());
        ++observed;
        GC_EXPECT_EQ(raw(slot.GetFieldValue()), raw(words[index]));
    });
    std::fprintf(stderr, "ROOT_STORAGE_TARGET finalizer_registered=%zu expected=%zu\n", observed, slots.size());
    GC_EXPECT_EQ(observed, slots.size());
    GC_EXPECT_TRUE(mutator.GetLocalFinalizers().empty());
}

GC_OTHER_VM_TEST(FnlzRoots, ExportBlockGrowthKeepsSlotsAndReleaseSkipsVacancies)
{
    auto& heap = Heap::GetHeap();
    GcHeapFixture fixture;
    std::array<BaseObject*, 130> objects;
    for (size_t i = 0; i < objects.size(); ++i) {
        objects[i] = fixture.PlaceObject(fixture.heapStart + 128 + i * 16);
    }
    fixture.region0->SetRegionAllocPtr(fixture.heapStart + 128 + objects.size() * 16);
    std::vector<U64> handles;
    NativeSlot* first = nullptr;
    for (size_t index = 0; index < 130; ++index) {
        handles.push_back(heap.RegisterExportRoot(objects[index]));
        if (index == 0) {
            heap.VisitAllExportRoots([&](NativeSlot& slot) {
                if (to_object(slot.GetTargetObject()) == objects[0]) { first = &slot; }
            });
        }
    }
    size_t seen = 0;
    NativeSlot* grown = nullptr;
    heap.VisitAllExportRoots([&](NativeSlot& slot) {
        for (auto& object : objects) {
            if (to_object(slot.GetTargetObject()) == object) {
                ++seen;
                if (&object == &objects[0]) { grown = &slot; }
            }
        }
    });
    std::fprintf(stderr, "ROOT_STORAGE_TARGET export_registered=%zu first=%p grown=%p\n",
                 seen, static_cast<void*>(first), static_cast<void*>(grown));
    GC_EXPECT_EQ(seen, size_t(130));
    GC_EXPECT_TRUE(first != nullptr && first == grown);
    for (U64 handle : handles) { heap.RemoveExportObject(handle); }
    size_t releasedSeen = 0;
    heap.VisitAllExportRoots([&](NativeSlot& slot) {
        for (auto& object : objects) {
            if (to_object(slot.GetTargetObject()) == object) { ++releasedSeen; }
        }
    });
    std::fprintf(stderr, "ROOT_STORAGE_TARGET export_released_remaining=%zu\n", releasedSeen);
    GC_EXPECT_EQ(releasedSeen, size_t(0));
}
