// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/Verify/ZVerify.h"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
// zVerify.cpp:119-128: positive counterpart to the invalid-address cases.
GC_OTHER_VM_TEST(ZVerify, AcceptsActualObjectAddress)
{
    GcHeapFixture fixture;
    ZVerify::Object(fixture.obj0, &fixture.obj0);
    GC_EXPECT_TRUE(fixture.obj0->IsValidObject());
}
