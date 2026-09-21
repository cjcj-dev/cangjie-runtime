#define MRT_USE_CJTHREAD_RENAME 1
#include "gc_heap_fixture.hpp"
#include "gc_generation_test.hpp"
#include "b09_runtime_fixture.hpp"
#include "gc_product_access_test.hpp"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "ObjectModel/RefField.inline.h"
#include <cstdio>
#include "Sync/Sync.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" void MRT_VisitorCaller(void*, void*);


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

void CheckSavedColor(bool updateThreadObject, bool remap = false)
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
    auto* thread = MCC_NewCJThread(nullptr, from,
        runtime.GetConcurrencyModel().GetThreadScheduler());
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
        ZRelocate::FixMinorRootSlots();
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
    ZMark::VisitMinorRootSlots(visitor, visitor);
    std::fprintf(stderr,
        "CONCURRENCY_ROOT_SECOND_TARGET observed=%#lx expected=%#lx\n",
        observed, expected);
    GC_EXPECT_EQ(observed, expected);
}

} // namespace

GC_OTHER_VM_TEST(ConcurrencyRootColor, SavedColorRemapsFromOffset) { CheckSavedColor(false); }
GC_OTHER_VM_TEST(ConcurrencyRootColor, RemapYoungRootGroupOnce) { CheckSavedColor(false, true); }
GC_OTHER_VM_TEST(ConcurrencyRootColor, ThreadObjectStorePreservesOtherRootEpoch) { CheckSavedColor(true); }

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
