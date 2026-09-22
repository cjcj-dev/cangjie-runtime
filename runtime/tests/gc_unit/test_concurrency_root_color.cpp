#define MRT_USE_CJTHREAD_RENAME 1
#include "gc_heap_fixture.hpp"
#include "gc_generation_test.hpp"
#include "b09_runtime_fixture.hpp"
#include "mark_publication_fixture.hpp"
#include "gc_product_access_test.hpp"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zTask.hpp"
#include "ObjectModel/RefField.inline.h"
#include <cstdio>
#include "Sync/Sync.h"
#include "Cangjie.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" void* MCC_NewExclusiveCJThread(void*, void*, void*);


namespace {
// ScheduleNew owns ScheduleManagerInit. A second initialization would reject
// the default scheduler before the real producer can allocate a CJThread.
class ConcurrencyRootRuntime final : public Runtime {
public:
    ConcurrencyRootRuntime()
    {
        runtime = this;
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        manager.Init();
        concurrency.Init(ConcurrencyParam{1024, 64, 1});
    }
    ~ConcurrencyRootRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam{}; }
    void SetGCThreshold(uint64_t) override {}
private:
    MutatorManager manager;
    Concurrency concurrency;
};

void CheckSavedColor(bool updateThreadObject, bool remap = false, bool noReturn = false)
{
    ConcurrencyRootRuntime runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    ZCollectedHeapTest::SetWorkers(1);
    for (auto gen : {ZGenerationId::young, ZGenerationId::old}) {
        auto& cycle = heap.GetZGeneration(gen);
        if (cycle.Snapshot().active) {
            cycle.End();
        }
        if (cycle.Workers() == nullptr) {
            cycle.InitializeWorkers(1);
        } else {
            cycle.Workers()->set_active_workers(1);
        }
        cycle.Begin(1);
    }
    ZGlobalsPointers::initialize();
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    ZPage* page = fx.region0;
    page->reset(PageAge::eden);
    ZGenerationTest::SetTenuringThreshold(heap.young(), 1);
    BaseObject* dead = fx.PlaceObject(page->GetRegionStart());
    BaseObject* earlier = fx.PlaceObject(page->GetRegionStart() + dead->GetSize());
    BaseObject* from = fx.PlaceObject(reinterpret_cast<MAddress>(earlier) + earlier->GetSize());
    BaseObject* second = fx.PlaceObject(reinterpret_cast<MAddress>(from) + from->GetSize());
    page->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + second->GetSize());
    auto scheduler = runtime.GetConcurrencyModel().GetThreadScheduler();
    auto* thread = noReturn
        ? MCC_NewCJThreadNoReturn(reinterpret_cast<void*>(1), from, scheduler, fx.typeInfo)
        : MCC_NewCJThread(nullptr, from, scheduler);
    GC_EXPECT_TRUE(thread != nullptr);
    auto* previousThread = CJThreadGetHandle();
    ThreadLocal::SetCJThread(thread);
    auto* data = static_cast<LWTData*>(CJThreadGetArg());
    ThreadLocal::SetCJThread(previousThread);
    GC_EXPECT_TRUE(data != nullptr);
    const uintptr_t savedColor = ZPointerStoreGoodMask;
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, earlier));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, from));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, second));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Young, {page}));
    heap.young().set_phase(ZGenerationPhase::Relocate);
    ZGlobalsPointers::flip_young_relocate_start();
    auto& manager = static_cast<RegionSpace&>(heap.GetAllocator()).GetRegionManager();
    manager.CompactRegion(page);
    page->MarkForwardingDone();
    const MAddress expected = forwarding_for_page(page)->find(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(expected != 0 && expected != reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(savedColor != ZPointerLoadGoodMask);
    MAddress observed = 0;
    RootVisitor visitor = [&](RootSlot& root) {
        if (&root == &RootSlotAt(&data->obj)) {
            observed = raw(root.LoadPlain());
        }
    };
    MAddress storedExpected = 0;
    if (updateThreadObject) {
        storedExpected = forwarding_for_page(page)->find(reinterpret_cast<MAddress>(second));
        auto* previous = CJThreadGetHandle();
        ThreadLocal::SetCJThread(thread);
        MCC_SetCurrentCJThreadObject(reinterpret_cast<void*>(storedExpected));
        ThreadLocal::SetCJThread(previous);
    }
    if (remap) {
        ZRelocate::RemapYoungRoots();
    } else {
        runtime.GetConcurrencyModel().VisitGCRoots();
    }
    observed = raw(RootSlotAt(&data->obj).LoadPlain());
    std::fprintf(stderr,
        "CONCURRENCY_ROOT_COLOR saved=%#lx current=%#lx from=%p observed=%#lx expected=%#lx\n",
        savedColor, ZPointerLoadGoodMask, from, observed, expected);
    GC_EXPECT_EQ(observed, expected);
    // Keep the young forwarding table alive. Reinterpreting the healed value
    // with the first epoch maps it to the preceding live object's destination.
    if (updateThreadObject) {
        const auto stored = raw(RootSlotAt(&data->threadObject).LoadPlain());
        std::fprintf(stderr, "CONCURRENCY_STORE_TARGET observed=%#lx expected=%#lx\n", stored, storedExpected);
        GC_EXPECT_EQ(stored, storedExpected);
    }
    heap.old().End();
    heap.old().mark_start();
    observed = 0;
    heap.old().concurrent_mark();
    observed = raw(RootSlotAt(&data->obj).LoadPlain());
    std::fprintf(stderr,
        "CONCURRENCY_ROOT_SECOND_TARGET observed=%#lx expected=%#lx\n",
        observed, expected);
    GC_EXPECT_EQ(observed, expected);
}

void CheckOldRootRead(bool healBeforeRead, bool revisitAfterRead = false)
{
    ConcurrencyRootRuntime runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    ZCollectedHeapTest::SetWorkers(1);
    for (auto gen : {ZGenerationId::young, ZGenerationId::old}) {
        auto& cycle = heap.GetZGeneration(gen);
        if (cycle.Snapshot().active) {
            cycle.End();
        }
        if (cycle.Workers() == nullptr) {
            cycle.InitializeWorkers(1);
        } else {
            cycle.Workers()->set_active_workers(1);
        }
        cycle.Begin(1);
    }
    ZGlobalsPointers::initialize();
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    ZPage* page = fx.region0;
    page->reset(PageAge::old);
    ZGenerationTest::SetTenuringThreshold(heap.young(), 1);
    BaseObject* dead = fx.PlaceObject(page->GetRegionStart());
    BaseObject* earlier = fx.PlaceObject(page->GetRegionStart() + dead->GetSize());
    BaseObject* from = fx.PlaceObject(reinterpret_cast<MAddress>(earlier) + earlier->GetSize());
    BaseObject* second = fx.PlaceObject(reinterpret_cast<MAddress>(from) + from->GetSize());
    page->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + second->GetSize());
    auto scheduler = runtime.GetConcurrencyModel().GetThreadScheduler();
    auto* thread = MCC_NewCJThread(nullptr, from, scheduler);
    GC_EXPECT_TRUE(thread != nullptr);
    auto* previousThread = CJThreadGetHandle();
    ThreadLocal::SetCJThread(thread);
    MCC_SetCurrentCJThreadObject(from);
    auto* data = static_cast<LWTData*>(CJThreadGetArg());
    ThreadLocal::SetCJThread(previousThread);
    GC_EXPECT_TRUE(data != nullptr);
    const uintptr_t savedColor = ZPointerStoreGoodMask;
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, earlier));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, from));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, second));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Old, {page}));
    heap.old().set_phase(ZGenerationPhase::Relocate);
    ZGlobalsPointers::flip_old_relocate_start();
    auto& manager = static_cast<RegionSpace&>(heap.GetAllocator()).GetRegionManager();
    manager.CompactRegion(page);
    page->MarkForwardingDone();
    const MAddress expected = forwarding_for_page(page)->find(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(expected != 0 && expected != reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(savedColor != ZPointerLoadGoodMask);
    // The saved store-good color is from the previous epoch, so the group is armed.
    GC_EXPECT_TRUE(CJThreadRootsAreArmed(thread, ZPointerStoreGoodMask));
    if (healBeforeRead) {
        runtime.GetConcurrencyModel().VisitGCRoots();
        // zNMethod.cpp:392-398: the GC partial color is mark good but never
        // store good, so the group stays armed for the mutator entry.
        GC_EXPECT_TRUE(CJThreadRootsAreArmed(thread, ZPointerStoreGoodMask));
    }
    auto* previous = CJThreadGetHandle();
    ThreadLocal::SetCJThread(thread);
    auto* observed = MRT_GetCurrentCJThreadObject();
    auto* repeated = MRT_GetCurrentCJThreadObject();
    ThreadLocal::SetCJThread(previous);
    std::fprintf(stderr, "CONCURRENCY_OLD_READ heal=%d saved=%#lx current=%#lx from=%p observed=%p expected=%#lx\n",
        healBeforeRead, savedColor, ZPointerLoadGoodMask, from, observed, expected);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(observed), expected);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(repeated), expected);
    // zBarrierSetNMethod.cpp:88-97: the mutator entry fully disarms the group.
    GC_EXPECT_TRUE(!CJThreadRootsAreArmed(thread, ZPointerStoreGoodMask));
    const MAddress groupObserved = raw(RootSlotAt(&data->obj).LoadPlain());
    std::fprintf(stderr, "CONCURRENCY_OLD_GROUP observed=%#lx expected=%#lx\n", groupObserved, expected);
    GC_EXPECT_EQ(groupObserved, expected);
    if (revisitAfterRead) {
        unsigned visits = 0;
        RootVisitor visitor = [&](RootSlot&) { ++visits; };
        runtime.GetConcurrencyModel().VisitGCRoots(&visitor);
        const bool armed = CJThreadRootsAreArmed(thread, ZPointerStoreGoodMask);
        std::fprintf(stderr, "CONCURRENCY_GC_REVISIT armed=%d visits=%u\n", armed, visits);
        // zNMethod.cpp:379-398: GC cannot re-arm a group within this epoch.
        GC_EXPECT_TRUE(!armed);
        GC_EXPECT_TRUE(visits >= 3);
        GC_EXPECT_EQ(raw(RootSlotAt(&data->threadObject).LoadPlain()), expected);
    }
}

} // namespace

GC_OTHER_VM_TEST(ConcurrencyRootColor, SavedColorRemapsFromOffset) { CheckSavedColor(false); }
GC_OTHER_VM_TEST(ConcurrencyRootColor, NoReturnSavedColorRemapsFromOffset) { CheckSavedColor(false, false, true); }
GC_OTHER_VM_TEST(ConcurrencyRootColor, RemapYoungRootGroupOnce) { CheckSavedColor(false, true); }
GC_OTHER_VM_TEST(ConcurrencyRootColor, ThreadObjectStorePreservesOtherRootEpoch) { CheckSavedColor(true); }

GC_OTHER_VM_TEST(ConcurrencyRootColor, RemapDuringYoungMarkPublishesFollowWork)
{
    ConcurrencyRootRuntime runtime;
    GcHeapFixture fx;
    ZGlobalsPointers::initialize();
    fx.region0->reset(PageAge::eden);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(fx.obj0) + 64);
    auto* thread = MCC_NewCJThread(nullptr, fx.obj0,
                                  runtime.GetConcurrencyModel().GetThreadScheduler());
    GC_EXPECT_TRUE(thread != nullptr);
    MarkPublicationFixture marking;
    marking.CompleteOldMarkForAdmissionTest();
    Heap::GetHeap().young().Workers()->set_active_workers(1);
    Heap::GetHeap().old().Workers()->set_active_workers(1);

    ZRelocate::RemapYoungRoots();

    class FlushMarkRootTask final : public ZTask {
    public:
        FlushMarkRootTask() : ZTask("ConcurrencyRootColorFlushMarkRoot") {}
        void work() override { Heap::GetHeap().young().Mark().FlushStacks(); }
    } flush;
    Heap::GetHeap().old().Workers()->run(&flush);
    bool found = false;
    marking.Drain([&](BaseObject* object, bool follow) {
        found |= object == fx.obj0 && follow;
    });
    std::fprintf(stderr, "CONCURRENCY_REMAP_MARK young=%d found=%d object=%p\n",
                 fx.region0->IsYoungRegion(), found, fx.obj0);
    GC_EXPECT_TRUE(found);
}

GC_OTHER_VM_TEST(ConcurrencyRootColor, NativeArgumentsAreNotRoots)
{
    ConcurrencyRootRuntime runtime;
    ZGlobalsPointers::initialize();
    uintptr_t nativeArgument = 1;
    auto entry = +[](void*, unsigned int) -> void* { return nullptr; };
    auto* thread = CJThreadNew(runtime.GetConcurrencyModel().GetThreadScheduler(), nullptr,
                              entry, &nativeArgument, sizeof(nativeArgument));
    GC_EXPECT_TRUE(thread != nullptr);
    size_t visits = 0;
    RootVisitor visitor = [&](RootSlot&) { ++visits; };
    runtime.GetConcurrencyModel().VisitGCRoots(&visitor);
    std::fprintf(stderr, "CONCURRENCY_NATIVE_TARGET visits=%zu\n", visits);
    GC_EXPECT_EQ(visits, size_t(0));
    auto* managed = MCC_NewCJThread(reinterpret_cast<void*>(1), nullptr,
                                   runtime.GetConcurrencyModel().GetThreadScheduler());
    GC_EXPECT_TRUE(managed != nullptr);
    runtime.GetConcurrencyModel().VisitGCRoots(&visitor);
    std::fprintf(stderr, "CONCURRENCY_MANAGED_CONTROL visits=%zu\n", visits);
    GC_EXPECT_EQ(visits, size_t(3));
}

GC_OTHER_VM_TEST(ConcurrencyRootColor, ThreadObjectOverwriteKeepsOldGroupAlive)
{
    ConcurrencyRootRuntime runtime;
    GcHeapFixture fx;
    auto* thread = MCC_NewCJThread(nullptr, fx.obj0,
                                  runtime.GetConcurrencyModel().GetThreadScheduler());
    GC_EXPECT_TRUE(thread != nullptr);
    auto* previous = CJThreadGetHandle();
    ThreadLocal::SetCJThread(thread);
    MCC_SetCurrentCJThreadObject(fx.obj1);
    ThreadLocal::SetCJThread(previous);
    MarkPublicationFixture marking;
    const bool beforeObj = fx.region0->is_object_marked_strong(from_object(fx.obj0));
    const bool beforeThread = fx.region1->is_object_marked_strong(from_object(fx.obj1));
    std::fprintf(stderr, "CONCURRENCY_KEEPALIVE_BEFORE obj=%u thread=%u\n", beforeObj, beforeThread);
    GC_EXPECT_TRUE(!beforeObj && !beforeThread);
    ThreadLocal::SetCJThread(thread);
    MCC_SetCurrentCJThreadObject(nullptr);
    ThreadLocal::SetCJThread(previous);
    // zMark.inline.hpp:48-87: a mutator publishes work; a GC worker marks
    // the bitmap later. Observe the product stack entries, not an early bit.
    bool afterObj = false;
    bool afterThread = false;
    marking.Drain([&](BaseObject* object, bool follow) {
        afterObj |= follow && object == fx.obj0;
        afterThread |= follow && object == fx.obj1;
    });
    std::fprintf(stderr, "CONCURRENCY_KEEPALIVE_TARGET obj=%u thread=%u\n", afterObj, afterThread);
    GC_EXPECT_TRUE(afterObj && afterThread);
}

GC_RUNTIME_OTHER_VM_TEST(ConcurrencyRootColor, ExclusiveProducerSeparatesTypeInfo)
{
    RuntimeParam param {};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    auto task = +[](void*) -> void* {
        Mutator::GetMutator()->SetManagedContext(false);
        try {
            static GcHeapFixture fx; // Roots live until this isolated VM exits.
            auto* thread = MCC_NewExclusiveCJThread(fx.obj1, fx.obj0, fx.typeInfo);
            GC_EXPECT_TRUE(thread != nullptr);
            auto* previous = CJThreadGetHandle();
            ThreadLocal::SetCJThread(thread);
            auto* data = static_cast<LWTData*>(CJThreadGetArg());
            ThreadLocal::SetCJThread(previous);
            GC_EXPECT_TRUE(data != nullptr);
            MAddress observedObj = 0;
            MAddress observedExecute = 0;
            size_t nativeVisits = 0;
            RootVisitor visitor = [&](RootSlot& slot) {
                if (&slot == &RootSlotAt(&data->obj)) { observedObj = raw(slot.LoadPlain()); }
                if (&slot == &RootSlotAt(&data->execute)) { observedExecute = raw(slot.LoadPlain()); }
                if (&slot == &RootSlotAt(&data->fn)) { ++nativeVisits; }
            };
            Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitor);
            std::fprintf(stderr,
                "CONCURRENCY_EXCLUSIVE_TARGET obj=%#lx expected_obj=%p execute=%#lx expected_execute=%p native_visits=%zu\n",
                observedObj, fx.obj0, observedExecute, fx.obj1, nativeVisits);
            GC_EXPECT_EQ(observedObj, reinterpret_cast<MAddress>(fx.obj0));
            GC_EXPECT_EQ(observedExecute, reinterpret_cast<MAddress>(fx.obj1));
            GC_EXPECT_EQ(nativeVisits, size_t(0));
            GC_EXPECT_TRUE(data->fn == fx.typeInfo);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "CONCURRENCY_EXCLUSIVE_ASSERT %s\n", error.what());
            Mutator::GetMutator()->SetManagedContext(true);
            return reinterpret_cast<void*>(1);
        }
        Mutator::GetMutator()->SetManagedContext(true);
        return nullptr;
    };
    auto handle = RunCJTask(task, nullptr);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    ReleaseHandle(handle);
    GC_EXPECT_TRUE(result == nullptr);
}

GC_OTHER_VM_TEST(ConcurrencyRootColor, OldRootReadBeforeGCVisit) { CheckOldRootRead(false); }
GC_OTHER_VM_TEST(ConcurrencyRootColor, OldRootReadAfterGCVisit) { CheckOldRootRead(true); }

GC_OTHER_VM_TEST(ConcurrencyRootColor, DisarmedGroupRemainsDisarmedAfterGCVisit)
{
    CheckOldRootRead(true, true);
}
