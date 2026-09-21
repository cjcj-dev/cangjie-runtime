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

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" void MRT_VisitorCaller(void*, void*);

GC_OTHER_VM_TEST(ConcurrencyRootColor, SavedColorRemapsFromOffset)
{
    B09RuntimeFixture runtime;
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
    LWTData data {};
    StorePlain(RootSlotAt(&data.obj), from_object(from));
    const uintptr_t savedColor = ZPointerLoadGoodMask;
    data.color = savedColor;
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
        if (&root == &RootSlotAt(&data.obj)) {
            observed = raw(root.LoadPlain());
        }
    };
    MRT_VisitorCaller(&data, &visitor);
    std::fprintf(stderr,
        "CONCURRENCY_ROOT_COLOR saved=%#lx current=%#lx from=%p observed=%#lx expected=%#lx\n",
        savedColor, ZPointerLoadGoodMask, from, observed, expected);
    GC_EXPECT_EQ(observed, expected);
    GC_EXPECT_EQ(data.color, ZPointerLoadGoodMask);
}
