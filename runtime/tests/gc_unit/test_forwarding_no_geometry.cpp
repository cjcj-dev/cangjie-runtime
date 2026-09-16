// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZForwardingTable::get and ZRelocate::relocate_object: a selected forwarding
// answers from its entries; an empty entry does not manufacture a destination.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/WCollector/WCollector.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
void InstallReceipt(GcHeapFixture& heap, MAddress from, MAddress to)
{
    Heap::GetHeap().GetRememberedSet().Initialize(heap.heapStart, GcHeapFixture::kUnits * ZPage::UNIT_SIZE);
    heap.InstallPageOwner(heap.region0);
    auto publication = ForwardingTable::EnsurePublicationBeforeCopy(heap.region0, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, from, to), to);
}
}

GC_TEST(ForwardingNoGeometry, ArmedMissIsNullNotGeometry)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0);
    const MAddress from = reinterpret_cast<MAddress>(heap.obj0);
    const Generation generation = heap.region0->GetOwnerGeneration();
    const auto result = ForwardingTable::LookupTo(from, generation);
    GC_EXPECT_TRUE(result.answer == ForwardingTable::ToAnswer::ArmedMiss);
    GC_EXPECT_EQ(result.to, static_cast<MAddress>(0));
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(from, generation) != nullptr);
}

GC_TEST(ForwardingNoGeometry, InstalledReceiptSurvivesPageReleaseUntilSetReset)
{
    GcHeapFixture heap;
    const MAddress from = reinterpret_cast<MAddress>(heap.obj0);
    const MAddress to = reinterpret_cast<MAddress>(heap.obj1);
    const Generation generation = heap.region0->GetOwnerGeneration();
    InstallReceipt(heap, from, to);
    auto owner = ForwardingTable::RetainPageOwner(heap.region0);
    GC_EXPECT_TRUE(static_cast<bool>(owner));
    owner->release_page();
    owner->mark_done();
    const auto result = ForwardingTable::LookupTo(from, generation);
    GC_EXPECT_TRUE(result.answer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_EQ(result.to, to);
    ForwardingTable::ResetRelocationSet(generation);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(from, generation) == nullptr);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from, generation).answer == ForwardingTable::ToAnswer::Unarmed);
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
        collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_FORWARD);
        ZPage::RetainScope lease(page);
        GC_EXPECT_TRUE(lease.ok());
        return collector.ForwardObjectImpl(from, page, lease);
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
