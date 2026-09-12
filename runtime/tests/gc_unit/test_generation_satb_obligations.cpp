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
#include "Heap/Collector/MarkEngine.h"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(GenerationSatb, YoungCleanupPreservesOldEntries)
{
    GcHeapFixture heap;
    // Old publications now use the actual M3 mark domain, not an old SATB
    // instance. This component test retains the original cleanup invariant.
    MarkDomain old(64, VerifyMarkingStacks::MarkingGeneration::MAJOR);
    old.PrepareWork(1);
    MarkThreadLocalStacks publication(old.Stripes().Count());
    publication.Push(old.Stripes(), 0, MarkStackEntry::FollowOnly(heap.obj0), true);
    publication.Flush(old.Stripes(), true);
    auto& young = SatbBuffer::Young();
    young.Init();
    SatbBuffer::Node* node = nullptr;
    young.EnsureGoodNode(node);
    node->Push(heap.obj1, nullptr, true);
    young.FlushQueue(node);
    BaseObject* youngObject = nullptr;
    young.GetRetiredEntries([&](BaseObject* object, bool) { youngObject = object; });
    young.ClearBuffer();
    young.ReclaimALLPages();
    MarkStackEntry entry;
    const bool found = old.Stacks(0).Pop(old.Smr(), 0, old.Stripes(), 0, entry);
    GC_EXPECT_TRUE(found);
    GC_EXPECT_TRUE(found && entry.object() == heap.obj0 && entry.follow() && !entry.mark());
    GC_EXPECT_TRUE(youngObject == heap.obj1);

}

GC_TEST(GenerationSatb, FlushReturnsNodeToItsOwner)
{
    GcHeapFixture heap;
    SatbBuffer old;
    SatbBuffer young;
    old.Init();
    young.Init();
    SatbBuffer::Node* node = nullptr;
    old.EnsureGoodNode(node);
    GC_EXPECT_TRUE(node != nullptr);
    node->Push(heap.obj0, nullptr, true);
    // Passing a different queue must not relabel already allocated storage.
    young.FlushQueue(node);
    BaseObject* oldObject = nullptr;
    BaseObject* youngObject = nullptr;
    old.GetRetiredEntries([&](BaseObject* object, bool) { oldObject = object; });
    young.GetRetiredEntries([&](BaseObject* object, bool) { youngObject = object; });
    old.Fini();
    young.Fini();
    std::printf("OBSERVED owner_old=%p other_young=%p expected=%p\n", oldObject, youngObject, heap.obj0);
    GC_EXPECT_TRUE(oldObject == heap.obj0);
    GC_EXPECT_TRUE(youngObject == nullptr);
}
int main(int argc, char** argv)
{
    if (argc == 2) setenv("GC_UNIT_FILTER", argv[1], 1);
    return RunAll();
}
