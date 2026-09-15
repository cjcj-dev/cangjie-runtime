// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Free-slot metadata colour handling.

#include <cstdint>
#include <cstring>

#include "Heap/Allocator/SlotList.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "gc_heap_fixture.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

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
// Product allocation no longer consumes these resource-cleanup lists.
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

    // A complete colored pointer is not a metadata-list address.
    auto coloured = reinterpret_cast<ObjectSlot*>(
        raw(ZAddress::store_good(from_object(fx.obj0))));
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
        raw(ZAddress::store_good(from_object(fx.obj1))));
    GC_EXPECT_EQ(SlotListTestAccess::PopFront(list, size), reinterpret_cast<uintptr_t>(fx.obj0));
    GC_EXPECT_TRUE(SlotListTestAccess::GetHead(list) == nullptr);
}
