// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#include <cstdint>
#include <cstdio>

#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.hpp"
#include "b09_runtime_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// zBarrier.cpp MarkFromOldSlowPath: a non-heap address has no page.
// ZGC zBarrier.cpp:187-203 assumes every non-null oop has a page; Cangjie
// slots may hold stack objects. The product result is the returned address.
GC_RUNTIME_TEST(MarkFromOldNonHeap, KeepsNonHeapIdentity)
{
    B09RuntimeFixture runtime;
    alignas(16) char stackObject[16] = {};
    const zaddress address = static_cast<zaddress>(reinterpret_cast<uintptr_t>(stackObject));
    const zaddress result = ZBarrier::MarkFromOldSlowPath(address);
    const int kept = raw(result) == raw(address) ? 1 : 0;
    std::fprintf(stderr, "MARK_FROM_OLD_NONHEAP_TARGET executed=1 kept=%d\n", kept);
    GC_EXPECT_TRUE(kept == 1);
}
