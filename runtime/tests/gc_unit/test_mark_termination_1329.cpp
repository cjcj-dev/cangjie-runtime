// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <sstream>
#include <functional>
#include <memory>
#include <thread>
#include <condition_variable>
#include <vector>
#include <array>
#include "Base/TimeUtils.h"
#include "Heap/z/zDriverPort.hpp"
#include "Heap/z/zMetronome.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zThread.hpp"
// Read the product statistics object; no replacement implementation or export.
#define private public
#include "Heap/z/zStat.hpp"
#undef private
#include "b09_runtime_fixture.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zRootsIterator.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Mutator/ThreadLocal.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
void StartOld(Heap& heap)
{
    InitializeGenerationWorkers(heap.old(), 1);
    InitializeGenerationWorkers(heap.young(), 1);
    ScopedStopTheWorld pause("1329 old mark start", false);
    heap.old().mark_start();
}
}

// zMark.cpp:974-991: a late barrier publication rejects end, then the next
// successful phase publishes the continuation count into ZStatMark.
GC_OTHER_VM_TEST(MarkTermination1329, OldContinueReachesStatistics)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    Heap& heap = Heap::GetHeap();
    StartOld(heap);
    heap.old().concurrent_mark();
    ZBarrier::MarkSlowPath(from_object(fx.obj0));
    bool first;
    {
        ScopedStopTheWorld pause("1329 first mark end", false);
        first = heap.old().mark_end();
    }
    heap.old().concurrent_mark_continue();
    bool second;
    {
        ScopedStopTheWorld pause("1329 continued mark end", false);
        second = heap.old().mark_end();
    }
    const size_t count = heap.old().StatMark()->_ncontinue;
    std::fprintf(stderr, "MARK1329_CONTINUE_TARGET executed=1 first=%d second=%d ncontinue=%zu\n",
                 first, second, count);
    GC_EXPECT_TRUE(!first && second && count >= 1);
}

// zMark.cpp:417-425: live bytes follow the page's object alignment, even
// when the object's own size is only aligned to the language allocation unit.
GC_OTHER_VM_TEST(MarkTermination1329, MediumLiveBytesUsePageAlignment)
{
    B09RuntimeFixture runtime;
    ZHeuristics::set_max_heap_size(256 * 1024 * 1024);
    ZHeuristics::set_medium_page_size();
    GcHeapFixture fx(ZPageType::medium);
    fx.typeInfo->SetInstanceSize(ZObjectSizeLimitSmall + 8);
    Heap& heap = Heap::GetHeap();
    fx.obj0 = fx.PlaceObject(fx.region0()->GetRegionStart());
    const size_t size = fx.obj0->GetSize();
    const size_t alignment = fx.region0()->object_alignment();
    const size_t expected = AlignUp(size, alignment);
    fx.region0()->SetRegionAllocPtr(fx.region0()->GetRegionStart() + expected);
    const U64 root = heap.cross_vm().export_roots().RegisterExportRoot(fx.obj0);
    StartOld(heap);
    heap.old().concurrent_mark();
    const size_t actual = fx.region0()->live_bytes();
    std::fprintf(stderr, "MARK1329_LIVE_TARGET executed=1 size=%zu alignment=%zu actual=%zu expected=%zu\n",
                 size, alignment, actual, expected);
    GC_EXPECT_TRUE(size != expected && actual == expected);
    heap.cross_vm().export_roots().RemoveExportRoot(root);
}

// The old pool's persistent worker receives a young mark through the product
// barrier. Only the ensuing old marking task is responsible for publishing it.
GC_OTHER_VM_TEST(MarkTermination1329, OldTaskPublishesYoungPrivateStack)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    Heap& heap = Heap::GetHeap();
    fx.region0()->reset(PageAge::eden);
    StartOld(heap);
    {
        ScopedStopTheWorld pause("1329 young mark start", false);
        heap.young().mark_start();
    }
    class SeedYoung final : public ZTask {
        BaseObject* object;
    public:
        explicit SeedYoung(BaseObject* obj) : ZTask("1329 young barrier input"), object(obj) {}
        void work() override { ZBarrier::MarkFromYoungSlowPath(from_object(object)); }
    } seed(fx.obj0);
    heap.old().Workers()->run(&seed);
    size_t before = 0;
    heap.old().Workers()->threads_do([&](WorkerThread* thread) {
        before += thread->gc_data()->markStacks[0].Population();
    });
    const size_t publicBefore = heap.young().Mark().Stripes().Population();
    heap.old().mark_follow();
    size_t after = 0;
    heap.old().Workers()->threads_do([&](WorkerThread* thread) {
        after += thread->gc_data()->markStacks[0].Population();
    });
    const size_t publicAfter = heap.young().Mark().Stripes().Population();
    std::fprintf(stderr, "MARK1329_FLUSH_TARGET executed=1 private_before=%zu private_after=%zu public_before=%zu public_after=%zu\n",
                 before, after, publicBefore, publicAfter);
    GC_EXPECT_TRUE(before == 1 && publicBefore == 0 && after == 0 && publicAfter == 1);
    heap.young().Mark().MarkFollow();
}
