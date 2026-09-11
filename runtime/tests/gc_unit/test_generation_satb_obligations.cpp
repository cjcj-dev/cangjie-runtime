// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.
// Tests the explicit ownership API. Concurrent mark/store wiring belongs to
// the successor batch; these are not end-to-end dual-mark closure tests.
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Mutator/SatbBuffer.h"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(GenerationSatb, YoungCleanupPreservesOldEntries)
{
    GcHeapFixture heap;
    auto& old = SatbBuffer::Instance(GCCycleGeneration::OLD);
    auto& young = SatbBuffer::Instance(GCCycleGeneration::YOUNG);
    old.Init();
    young.Init();
    SatbBuffer::Node* node = nullptr;
    old.EnsureGoodNode(node);
    GC_EXPECT_TRUE(node != nullptr);
    node->Push(heap.obj0, nullptr, true);
    old.FlushQueue(node);
    young.EnsureGoodNode(node);
    node->Push(heap.obj1, nullptr, true);
    young.FlushQueue(node);
    BaseObject* youngObject = nullptr;
    young.GetRetiredEntries([&](BaseObject* object, bool) { youngObject = object; });
    young.ClearBuffer();
    young.ReclaimALLPages();
    BaseObject* oldObject = nullptr;
    old.GetRetiredEntries([&](BaseObject* object, bool) { oldObject = object; });
    old.Fini();
    std::printf("OBSERVED young=%p old=%p expected_young=%p expected_old=%p\n",
                youngObject, oldObject, heap.obj1, heap.obj0);
    GC_EXPECT_EQ(youngObject, heap.obj1);
    GC_EXPECT_EQ(oldObject, heap.obj0);
}

GC_TEST(GenerationSatb, FlushReturnsNodeToItsOwner)
{
    GcHeapFixture heap;
    auto& old = SatbBuffer::Instance(GCCycleGeneration::OLD);
    auto& young = SatbBuffer::Instance(GCCycleGeneration::YOUNG);
    old.Init();
    young.Init();
    SatbBuffer::Node* node = nullptr;
    old.EnsureGoodNode(node);
    GC_EXPECT_TRUE(node != nullptr);
    node->Push(heap.obj0, nullptr, true);
    // A serial selector change must not relabel already allocated storage.
    young.FlushQueue(node);
    BaseObject* oldObject = nullptr;
    BaseObject* youngObject = nullptr;
    old.GetRetiredEntries([&](BaseObject* object, bool) { oldObject = object; });
    young.GetRetiredEntries([&](BaseObject* object, bool) { youngObject = object; });
    old.Fini();
    young.Fini();
    std::printf("OBSERVED owner_old=%p other_young=%p expected=%p\n", oldObject, youngObject, heap.obj0);
    GC_EXPECT_EQ(oldObject, heap.obj0);
    GC_EXPECT_EQ(youngObject, nullptr);
}
int main(int argc, char** argv)
{
    if (argc == 2) setenv("GC_UNIT_FILTER", argv[1], 1);
    return RunAll();
}
