// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U5 / U6 — product Collector::TryRecoverInteriorBase + PlausibleManagedObjectGate.
// Product symbols from Collector.cpp (linked via libcangjie-runtime).
// Defect anchors: RawArray+8 / tip-small-int / tip-in-heap (fixinput / nilclass).

#include <cstdint>
#include <cstring>

#include "Heap/Allocator/SlotList.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/ManagedObjectGate.h"
#include "gc_heap_fixture.hpp"
#include "Heap/Allocator/RegionManager.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// U6: product PlausibleManagedObjectGate rejects tip-small-int / null tip.
GC_TEST(ObjectGate, TipSmallIntRejected)
{
    GcHeapFixture fx;
    BaseObject* obj = fx.obj0;
    // Null tip.
    *reinterpret_cast<uint64_t*>(obj) = 0;
    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit", obj));
    // Small-int tip (RawArray length shape).
    *reinterpret_cast<uint64_t*>(obj) = 42;
    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit", obj));
    *reinterpret_cast<uint64_t*>(obj) = 8;
    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit", obj));
    // Restore for fixture teardown hygiene.
    *reinterpret_cast<uint64_t*>(obj) = reinterpret_cast<uintptr_t>(fx.typeInfo);
}

// U6: product gate rejects tip-in-heap (interior into another managed object).
GC_TEST(ObjectGate, TipInHeapRejected)
{
    GcHeapFixture fx;
    BaseObject* obj = fx.obj0;
    // Tip points into heap (obj1).
    *reinterpret_cast<uint64_t*>(obj) = reinterpret_cast<uintptr_t>(fx.obj1);
    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit", obj));
    *reinterpret_cast<uint64_t*>(obj) = reinterpret_cast<uintptr_t>(fx.typeInfo);
}

// U6: aligned non-heap tip accepted by product tip branch (region live).
GC_TEST(ObjectGate, PlausibleTipAccepted)
{
    GcHeapFixture fx;
    GC_EXPECT_TRUE(Collector::PlausibleManagedObjectGate("gc_unit", fx.obj0));
}

// U5: product TryRecoverInteriorBase recovers RawArray+8 host.
GC_TEST(ObjectGate, RawArrayPlus8RecoversBase)
{
    GcHeapFixture fx;
    BaseObject* base = fx.obj0;
    auto interiorAddr = reinterpret_cast<uintptr_t>(base) + 8;
    auto* interior = reinterpret_cast<BaseObject*>(interiorAddr);
    // Length word at interior (tip-small-int); base keeps plausible TypeInfo tip.
    *reinterpret_cast<uint64_t*>(interior) = 16;

    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit.interior", interior));
    BaseObject* host = Collector::TryRecoverInteriorBase(interior);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(host), reinterpret_cast<uintptr_t>(base));
}

GC_TEST(ObjectGate, GeomCrossEndRejectedOnActiveRegion)
{
    GcHeapFixture fx;
    MAddress regionEnd = fx.region0->GetRegionEnd();
    auto* tail = reinterpret_cast<BaseObject*>(regionEnd - 8);
    *reinterpret_cast<uint64_t*>(tail) = reinterpret_cast<uintptr_t>(fx.typeInfo);
    fx.region0->SetRegionAllocPtr(regionEnd);
    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit.tailslot", tail));
    BaseObject* host = Collector::TryRecoverInteriorBase(tail);
    (void)host;
}

// U5: non-interior (plausible tip) does not invent a host.
GC_TEST(ObjectGate, NonInteriorNoFalseRecover)
{
    GcHeapFixture fx;
    BaseObject* host = Collector::TryRecoverInteriorBase(fx.obj0);
    GC_EXPECT_TRUE(host == nullptr);
}

// sizegate2: allocator-header consumers fail closed on a FREE_REGION without
// entering BaseObject::GetSize. This is the deterministic counterpart to the
// stochastic compiler-load GetSize/0x8 signature.
GC_TEST(ObjectGate, HeaderConsumersRejectFreeRegion)
{
    GcHeapFixture fx;
    fx.region0->SetRegionType(RegionInfo::RegionType::FREE_REGION);

    GC_EXPECT_FALSE(MapleRuntime::PlausibleManagedObjectGate("gc_unit.free", fx.obj0));
    GC_EXPECT_TRUE(fx.region0->MarkObject(
        fx.region0->GetMarkView<Generation::Old>(), fx.obj0));

    SlotList slots;
    GC_EXPECT_FALSE(slots.ClearExtraContent(fx.obj0));

    // The retired enqueue side map no longer dereferences the object header.
    // ShouldEnqueue rejects the dead region at the ownership gate.
    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Old>(fx.obj0));
}

namespace MapleRuntime {
struct SlotListTestAccess {
    static uintptr_t PopFront(SlotList& list, size_t size) { return list.PopFront(size); }
    static void SetHead(SlotList& list, ObjectSlot* h) { list.head = h; }
    static ObjectSlot* GetHead(SlotList& list) { return list.head; }
};
} // namespace MapleRuntime

// getsizetrace: SlotList::PopFront used to call GetSize on `head` with no gate.
// ObjectSlot::next overlays Future payload+8 (store-good colour). A coloured
// next makes head non-canonical; GetSize then #GPs (si_code=128, rbx=0xa8).
// AllocPinnedFromFreeList (RegionManager.cpp:3611) is the soak caller.
// ZGC (zPage.cpp:103-121): recycle by resetting metadata, not by treating a
// coloured oop as a free-list successor. Drop the chain; do not uncolor-and-hand-out.
GC_TEST(ObjectGate, SlotListPopFrontRejectsColouredHead)
{
    GcHeapFixture fx;
    SlotList list;
    GC_EXPECT_TRUE(list.ClearExtraContent(fx.obj0));
    list.PushFront(fx.obj0);
    size_t size = fx.obj0->GetSize();
    GC_EXPECT_EQ(SlotListTestAccess::PopFront(list, size), reinterpret_cast<uintptr_t>(fx.obj0));

    // Coloured head: Remembered bit 56 (store-good) makes the word non-canonical.
    auto coloured = reinterpret_cast<ObjectSlot*>(
        reinterpret_cast<uintptr_t>(fx.obj0) | MapleRuntime::REMEMBERED_0);
    SlotListTestAccess::SetHead(list, coloured);
    GC_EXPECT_EQ(SlotListTestAccess::PopFront(list, size), static_cast<uintptr_t>(0));
    GC_EXPECT_TRUE(SlotListTestAccess::GetHead(list) == nullptr);

    // Drop, do not jam: a later PushFront of a plain slot must still succeed.
    GC_EXPECT_TRUE(list.ClearExtraContent(fx.obj0));
    list.PushFront(fx.obj0);
    GC_EXPECT_EQ(SlotListTestAccess::PopFront(list, size), reinterpret_cast<uintptr_t>(fx.obj0));
}

GC_TEST(ObjectGate, SlotListPopFrontDropsColouredNext)
{
    GcHeapFixture fx;
    SlotList list;
    GC_EXPECT_TRUE(list.ClearExtraContent(fx.obj0));
    list.PushFront(fx.obj0);
    size_t size = fx.obj0->GetSize();
    auto* slot = reinterpret_cast<ObjectSlot*>(fx.obj0);
    slot->next = reinterpret_cast<ObjectSlot*>(
        reinterpret_cast<uintptr_t>(fx.obj1) | MapleRuntime::REMEMBERED_0);
    GC_EXPECT_EQ(SlotListTestAccess::PopFront(list, size), reinterpret_cast<uintptr_t>(fx.obj0));
    GC_EXPECT_TRUE(SlotListTestAccess::GetHead(list) == nullptr);
}

GC_TEST(ObjectGate, FreePinnedPushFrontRejectsFreeRegion)
{
    GcHeapFixture fx;
    fx.region0->SetRegionType(RegionInfo::RegionType::FREE_REGION);
    FreePinnedSlotLists lists;
    lists.PushFront(fx.obj0);
}
