// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZForwardingTable::get and ZRelocate::relocate_object: a selected forwarding
// answers from its entries; an empty entry does not manufacture a destination.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/WCollector.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
void InstallReceipt(GcHeapFixture& heap, MAddress from, MAddress to)
{
    heap.InstallPageOwner(heap.region0);
    auto publication = forwarding_for_page(heap.region0, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(UNUSED_InsertMapping(publication, from, to), to);
}
}

GC_TEST(ForwardingNoGeometry, ArmedMissIsNullNotGeometry)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0);
    const MAddress from = reinterpret_cast<MAddress>(heap.obj0);
    const Generation generation = heap.region0->GetOwnerGeneration();
    const auto result = LookupTo(from, generation);
    GC_EXPECT_TRUE(result.answer == FwdLookup::ArmedMiss);
    GC_EXPECT_EQ(result.to, static_cast<MAddress>(0));
    GC_EXPECT_TRUE(generation_forwarding_table(generation).get(from) != nullptr);
}

#if defined(MRT_TESTABLE_INTERNALS)
namespace MapleRuntime {
struct MutatorPublishTestAccess {
    static BaseObject* RelocateInner(WCollector& collector, BaseObject* from, ZPage* page)
    {
        return collector.RelocateObjectInner(from, page);
    }
    static BaseObject* ForwardImpl(WCollector& collector, BaseObject* from, ZPage* page)
    {
        collector.GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Relocate);
        ZPage::RetainScope lease(page);
        GC_EXPECT_TRUE(lease.ok());
        return collector.RelocateObjectInner(from, page);
    }
};
}

GC_TEST(ForwardingNoGeometry, RelocateInnerFindHitSkipsCopy)
{
    GcHeapFixture heap;
    InstallReceipt(heap, reinterpret_cast<MAddress>(heap.obj0), reinterpret_cast<MAddress>(heap.obj1));
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    GC_EXPECT_TRUE(MutatorPublishTestAccess::RelocateInner(collector, heap.obj0, heap.region0) == heap.obj1);
    GC_EXPECT_FALSE(heap.obj0->IsForwarded());
}

GC_TEST(ForwardingNoGeometry, ForwardImplFindHitSkipsCopy)
{
    GcHeapFixture heap;
    InstallReceipt(heap, reinterpret_cast<MAddress>(heap.obj0), reinterpret_cast<MAddress>(heap.obj1));
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    GC_EXPECT_TRUE(MutatorPublishTestAccess::ForwardImpl(collector, heap.obj0, heap.region0) == heap.obj1);
    GC_EXPECT_FALSE(heap.obj0->IsForwarded());
}

GC_TEST(ForwardingNoGeometry, ExclusiveVtableFindHitSkipsCopy)
{
    GcHeapFixture heap;
    InstallReceipt(heap, reinterpret_cast<MAddress>(heap.obj0), reinterpret_cast<MAddress>(heap.obj1));
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    GC_EXPECT_TRUE(collector.ForwardObjectExclusive(heap.obj0) == heap.obj1);
    GC_EXPECT_FALSE(heap.obj0->IsForwarded());
}
#endif // MRT_TESTABLE_INTERNALS
