// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zAccess.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zHeap.hpp"
#include <cstdio>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// oopStorage.cpp:774-784 does not write the slot. zBarrierSet.inline.hpp:195-199
// publishes store_good(null), which is null-any and mark-good. A second allocated
// slot keeps the block alive so the released word can still be read.
GC_TEST(ExportRootRelease, KeepsStoreGoodNull)
{
    GcHeapFixture fx;
    OopStorage& storage = Heap::GetHeap().GetExportRootStorage();
    NativeSlot* keep = storage.Allocate();
    NativeSlot* slot = storage.Allocate();
    GC_EXPECT_TRUE(keep != nullptr);
    GC_EXPECT_TRUE(slot != nullptr);
    GC_EXPECT_FALSE(ZPointer::is_mark_good(zpointer::null));
    NativeAccess<>::oop_store(slot, fx.obj0);
    NativeAccess<>::oop_store(slot, nullptr);
    const zpointer cleared = slot->GetFieldValue();
    GC_EXPECT_TRUE(is_null_any(cleared));
    GC_EXPECT_FALSE(is_null(cleared));
    GC_EXPECT_TRUE(ZPointer::is_mark_good(cleared));
    storage.Release(slot);
    const zpointer after = slot->GetFieldValue();
    GC_EXPECT_TRUE(is_null_any(after));
    GC_EXPECT_TRUE(ZPointer::is_mark_good(after));
    std::fprintf(stderr, "EXPORT_ROOT_RELEASE_MARK_GOOD_ASSERTED word=%#zx\n", raw(after));
    GC_EXPECT_EQ(raw(after), raw(cleared));
    NativeAccess<>::oop_store(keep, nullptr);
    storage.Release(keep);
}

#include "CangjieRuntime.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/shared/collectedHeap.hpp"
#include "Heap/z/zReferenceProcessor.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Mutator/MutatorManager.h"
#include "finalizer_processor_test.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace {
void ExpectReleasedColour(const char* branch, NativeSlot* slot)
{
    const zpointer word = slot->GetFieldValue();
    // This is the result published by the product branch, not a test-side clear.
    std::fprintf(stderr, "FINALIZER_RELEASE_TARGET branch=%s word=%#zx null_any=%d mark_good=%d\n",
                 branch, raw(word), is_null_any(word), ZPointer::is_mark_good(word));
    GC_EXPECT_TRUE(is_null_any(word) && ZPointer::is_mark_good(word));
}

void CheckEnqueueRelease(int obsolete)
{
    WorkerFixture worker;
    GcHeapFixture fx;
    Heap::OnHeapCreated(fx.heapStart);
    Heap::OnHeapExtended(fx.heapStart + GcHeapFixture::kUnits * ZGranuleSize);
    ZStatWorkers stats;
    ZWorkers pool(ZGenerationId::old, 1, &stats);
    FinalizerProcessor fp(&pool);
    NativeSlot* stale = nullptr;
    if (obsolete != 0) {
        fp.RegisterFinalizer(obsolete == 1 ? nullptr : fx.obj1);
        fp.VisitFinalizers([&](NativeSlot& slot) { stale = &slot; });
        if (obsolete == 1) {
            // Weak processing may leave a null of an older colour. Use the
            // legal null-any boundary input to distinguish the store from load.
            stale->StoreColoured(zpointer::null);
        } else {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(fx.obj1);
            CollectedHeap::fill_with_dummy_object(addr, addr + 16, false);
        }
    }
    fp.RegisterFinalizer(fx.obj0);
    NativeSlot* moved = nullptr;
    fp.VisitFinalizers([&](NativeSlot& slot) { if (&slot != stale) { moved = &slot; } });
    GC_EXPECT_TRUE(moved != nullptr);
    // Pin the real storage block across its release. No unallocated word is
    // read after the storage is allowed to reclaim the block.
    OopStorage::ParState<true> pin(fp.WeakRootStorage());
    GC_EXPECT_TRUE(GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0));
    GC_EXPECT_TRUE(fp.GetReferenceProcessor().discover_reference(fx.obj0, ReferenceType::FINAL));
    fp.ProcessReferences([](BaseObject*) { return false; });
    fp.EnqueueReferences();
    ExpectReleasedColour("enqueue-move", moved);
    if (stale != nullptr) { ExpectReleasedColour(obsolete == 1 ? "enqueue-null" : "enqueue-filler", stale); }
    GC_EXPECT_EQ(fp.WeakRootStorage().AllocationCount(), size_t(0));
    GC_EXPECT_EQ(fp.StrongRootStorage().AllocationCount(), size_t(1));
}

std::atomic<unsigned> finalizerCalls{0};
void ReleaseTestFinalizer(BaseObject*, TypeInfo*)
{
    finalizerCalls.fetch_add(1, std::memory_order_release);
}

void CheckWorkerRelease(int obsolete)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto& heap = Heap::GetHeap();
    auto& manager = MutatorManager::Instance();
    manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    alignas(TypeInfo) unsigned char metadata[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(metadata);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(8);
    type->SetFlag(FLAG_HAS_FINALIZER);
    // For non-generic TypeInfo the sourceGeneric/finalizerMethod union contains
    // the method address (MClass.cpp:1197-1203).
    type->SetSourceGeneric(reinterpret_cast<TypeTemplate*>(&ReleaseTestFinalizer));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(metadata), sizeof(metadata));
    // The isolated VM owns this worker for its remaining lifetime, just as it
    // owns the runtime workers. Do not start a second set of uncommitters.
    auto& fp = *new FinalizerProcessor();
    NativeSlot* queued = nullptr;
    {
        ScopedObjectAccess access;
        auto* object = reinterpret_cast<BaseObject*>(heap.object_allocator().alloc(16));
        GC_EXPECT_TRUE(object != nullptr);
        object->SetClassInfo(type);
        // Existing fixture access prepares the queue through the product
        // registration/enqueue functions. The observed clear is executed by
        // MRT_ProcessFinalizers -> Run -> ProcessFinalizables on its own thread.
        GC_EXPECT_TRUE(FinalizerProcessorTest::Queue(fp, object));
        fp.VisitGCRoots([&](NativeSlot& slot) { queued = &slot; });
        GC_EXPECT_TRUE(queued != nullptr);
        if (obsolete == 1) { queued->StoreColoured(zpointer::null); }
        if (obsolete == 2) {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(object);
            CollectedHeap::fill_with_dummy_object(addr, addr + 16, false);
        }
    }
    OopStorage::ParState<true> pin(fp.StrongRootStorage());
    finalizerCalls.store(0, std::memory_order_release);
    std::thread([&fp] { fp.Run(); }).detach();
    fp.WaitStarted();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (fp.StrongRootStorage().AllocationCount() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ExpectReleasedColour(obsolete == 0 ? "worker-finalized" : obsolete == 1 ? "worker-null" : "worker-filler", queued);
    GC_EXPECT_EQ(fp.StrongRootStorage().AllocationCount(), size_t(0));
    GC_EXPECT_EQ(finalizerCalls.load(std::memory_order_acquire), obsolete == 0 ? 1u : 0u);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    // The child VM terminates the idle worker after the test completion sentinel.
}
}

GC_OTHER_VM_TEST(FinalizerRelease, EnqueueMove) { CheckEnqueueRelease(0); }
GC_OTHER_VM_TEST(FinalizerRelease, EnqueueNull) { CheckEnqueueRelease(1); }
GC_OTHER_VM_TEST(FinalizerRelease, EnqueueFiller) { CheckEnqueueRelease(2); }
GC_RUNTIME_OTHER_VM_TEST(FinalizerRelease, WorkerFinalized) { CheckWorkerRelease(0); }
GC_RUNTIME_OTHER_VM_TEST(FinalizerRelease, WorkerNull) { CheckWorkerRelease(1); }
GC_RUNTIME_OTHER_VM_TEST(FinalizerRelease, WorkerFiller) { CheckWorkerRelease(2); }
