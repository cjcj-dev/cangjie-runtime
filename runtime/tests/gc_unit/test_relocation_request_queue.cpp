// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_heap_fixture.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zRelocate.hpp"
#include <atomic>
#include <thread>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// ZGC zRelocate.cpp:134-191. add_and_wait may enqueue the same forwarding
// more than once; claim() selects exactly one worker, and leave() wakes waiters.
GC_TEST(RelocationPageQueue, TwoObjectsShareOnePageClaim)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0());
    auto* owner = forwarding_for_page(heap.region0());
    ZRelocateQueue queue;
    queue.activate(1);
    std::atomic<unsigned> returned{0};
    std::thread first([&] { queue.add_and_wait(owner); ++returned; });
    std::thread second([&] { queue.add_and_wait(owner); ++returned; });
    ZForwarding* selected = nullptr;
    while ((selected = queue.synchronize_poll()) == nullptr) std::this_thread::yield();
    const bool singleClaim = selected == owner && !owner->claim();
    owner->release_page();
    owner->mark_done();
    queue.leave();
    first.join();
    second.join();
    queue.deactivate();
    GC_EXPECT_TRUE(singleClaim);
    GC_EXPECT_EQ(returned.load(), 2U);
    GC_EXPECT_TRUE(owner->is_done());
}

GC_TEST(RelocationPageQueue, ReleasedPageStillHasItsImmutableEntry)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0());
    auto* owner = forwarding_for_page(heap.region0());
    const auto from = reinterpret_cast<MAddress>(heap.obj0);
    const auto to = reinterpret_cast<MAddress>(heap.obj1);
    GC_EXPECT_EQ(owner->insert(from, to), to);
    owner->release_page();
    ZRelocateQueue queue;
    GC_EXPECT_FALSE(owner->retain_page(&queue));
    GC_EXPECT_TRUE(Heap::GetHeap().old().remap_object(heap.obj0) == heap.obj1);
    GC_EXPECT_FALSE(owner->is_done());
}

GC_TEST(RelocationPageQueue, DoneBeforeEnqueueNeedsNoWorker)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0());
    auto* owner = forwarding_for_page(heap.region0());
    owner->release_page();
    owner->mark_done();
    ZRelocateQueue queue;
    queue.add_and_wait(owner);
    GC_EXPECT_TRUE(owner->is_done());
    GC_EXPECT_FALSE(queue.is_active());
}

GC_TEST(RelocationPageQueue, EntryPublicationDoesNotCompleteThePage)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0());
    auto* owner = forwarding_for_page(heap.region0());
    const auto from = reinterpret_cast<MAddress>(heap.obj0);
    const auto to = reinterpret_cast<MAddress>(heap.obj1);
    ZRelocateQueue queue;
    queue.activate(1);
    std::atomic<bool> returned{false};
    std::thread requester([&] { queue.add_and_wait(owner); returned.store(true); });
    while (queue.synchronize_poll() == nullptr) std::this_thread::yield();
    const auto receipt = owner->insert(from, to);
    const bool prematurelyDone = owner->is_done();
    const bool prematurelyReturned = returned.load();
    owner->release_page();
    owner->mark_done();
    queue.leave();
    requester.join();
    queue.deactivate();
    GC_EXPECT_EQ(receipt, to);
    GC_EXPECT_FALSE(prematurelyDone);
    GC_EXPECT_FALSE(prematurelyReturned);
    GC_EXPECT_EQ(owner->find(from), to);
}
