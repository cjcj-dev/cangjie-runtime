// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_heap_fixture.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zPageType.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ZPageType, EnumValues)
{
    GC_EXPECT_EQ(static_cast<uint8_t>(ZPageType::small), 0);
    GC_EXPECT_EQ(static_cast<uint8_t>(ZPageType::medium), 1);
    GC_EXPECT_EQ(static_cast<uint8_t>(ZPageType::large), 2);
}

GC_TEST(ZPage, AllocPagePublishedInTable)
{
    GcHeapFixture fx;
    ZPage* page = fx.region0;
    GC_EXPECT_TRUE(page != nullptr);
    GC_EXPECT_TRUE(ZPageTable::heap_table().get(page->GetRegionStart()) == page);
    GC_EXPECT_TRUE(Heap::page(page->GetRegionStart()) == page);
    page->reset_seqnum();
    GC_EXPECT_TRUE(page->is_allocating());
    GC_EXPECT_TRUE(!page->is_relocatable());
}

GC_TEST(ZPage, ObjectAlignmentFollowsType)
{
    GcHeapFixture fx;
    ZPage* page = fx.region0;
    GC_EXPECT_EQ(page->object_alignment(), size_t(1) << page->object_alignment_shift());
    if (page->is_small()) {
        GC_EXPECT_EQ(page->object_alignment_shift(), ZObjectAlignmentSmallShift);
    }
}

GC_TEST(ZPage, AllocObjectRespectsAlignment)
{
    GcHeapFixture fx;
    ZPage* page = fx.region0;
    page->reset_seqnum();
    const uintptr_t addr = page->alloc_object(16);
    GC_EXPECT_TRUE(addr != 0);
    GC_EXPECT_EQ(addr % page->object_alignment(), 0u);
    GC_EXPECT_TRUE(page->undo_alloc_object(addr, 16));
}
