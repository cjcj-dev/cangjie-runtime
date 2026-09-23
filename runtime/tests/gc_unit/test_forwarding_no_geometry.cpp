// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZForwardingTable::get and ZRelocate::relocate_object: a selected forwarding
// answers from its entries; an empty entry does not manufacture a destination.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zRelocate.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
void InstallReceipt(GcHeapFixture& heap, MAddress from, MAddress to)
{
    heap.InstallPageOwner(heap.region0);
    auto publication = forwarding_for_page(heap.region0, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(publication->insert(from, to), to);
}
}

GC_TEST(ForwardingNoGeometry, ArmedMissIsNullNotGeometry)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0);
    const MAddress from = reinterpret_cast<MAddress>(heap.obj0);
    const Generation generation = heap.region0->GetOwnerGeneration();
    const MAddress result = generation_forwarding_table(generation).get(from)->find(from);
    GC_EXPECT_EQ(result, static_cast<MAddress>(0));
    GC_EXPECT_TRUE(generation_forwarding_table(generation).get(from) != nullptr);
}

#if defined(MRT_TESTABLE_INTERNALS)
namespace MapleRuntime {
struct MutatorPublishTestAccess {
    static BaseObject* RelocateInner(Heap& collector, BaseObject* from, ZPage* page)
    {
        return ZGeneration::generation(page->generation_id())->relocate().relocate_object(forwarding_for_page(page), from);
    }
    static BaseObject* ForwardImpl(Heap& collector, BaseObject* from, ZPage* page)
    {
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
        ZPage::RetainScope lease(page);
        GC_EXPECT_TRUE(lease.ok());
        return ZGeneration::generation(page->generation_id())->relocate().relocate_object(forwarding_for_page(page), from);
    }
};
}

GC_TEST(ForwardingNoGeometry, RelocateObjectFindHitSkipsCopy)
{
    GcHeapFixture heap;
    InstallReceipt(heap, reinterpret_cast<MAddress>(heap.obj0), reinterpret_cast<MAddress>(heap.obj1));
    Heap& collector = Heap::GetHeap();
    GC_EXPECT_TRUE(MutatorPublishTestAccess::RelocateInner(collector, heap.obj0, heap.region0) == heap.obj1);
    GC_EXPECT_FALSE(heap.obj0->IsForwarded());
}

GC_TEST(ForwardingNoGeometry, ForwardImplFindHitSkipsCopy)
{
    GcHeapFixture heap;
    InstallReceipt(heap, reinterpret_cast<MAddress>(heap.obj0), reinterpret_cast<MAddress>(heap.obj1));
    Heap& collector = Heap::GetHeap();
    GC_EXPECT_TRUE(MutatorPublishTestAccess::ForwardImpl(collector, heap.obj0, heap.region0) == heap.obj1);
    GC_EXPECT_FALSE(heap.obj0->IsForwarded());
}

GC_TEST(ForwardingNoGeometry, ExclusiveVtableFindHitSkipsCopy)
{
    GcHeapFixture heap;
    InstallReceipt(heap, reinterpret_cast<MAddress>(heap.obj0), reinterpret_cast<MAddress>(heap.obj1));
    Heap& collector = Heap::GetHeap();
    GC_EXPECT_TRUE(ZRelocate::ForwardObjectExclusive(heap.obj0) == heap.obj1);
    GC_EXPECT_FALSE(heap.obj0->IsForwarded());
}
#endif // MRT_TESTABLE_INTERNALS
