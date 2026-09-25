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
