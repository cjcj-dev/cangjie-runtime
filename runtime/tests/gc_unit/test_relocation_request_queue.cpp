// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_heap_fixture.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "b09_runtime_fixture.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/Handshake.h"
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

GC_TEST(RelocationPageQueue, EnqueueWakesSynchronizedWorker)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0());
    auto* owner = forwarding_for_page(heap.region0());
    ZRelocateQueue queue;
    queue.activate(1);
    std::atomic<bool> synchronized{false};
    std::thread synchronizer([&] { queue.synchronize(); synchronized.store(true); });
    ZForwarding* selected = nullptr;
    std::thread worker([&] {
        while ((selected = queue.synchronize_poll()) == nullptr) std::this_thread::yield();
        selected->release_page();
        selected->mark_done();
        queue.leave();
    });
    while (!synchronized.load()) std::this_thread::yield();
    queue.add_and_wait(owner);
    queue.desynchronize();
    worker.join();
    synchronizer.join();
    queue.deactivate();
    GC_EXPECT_TRUE(selected == owner);
    GC_EXPECT_TRUE(owner->is_claimed() && owner->is_done());
}

// Waiting in the leaf barrier must preserve the unsafe mutator/handshake
// context. Observe it while actually queued and after the product wait returns.
GC_OTHER_VM_TEST(RelocationPageQueue, WaitPreservesMutatorAndHandshakeContext)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0());
    auto* owner = forwarding_for_page(heap.region0());
    ZRelocateQueue queue;
    queue.activate(1);
    Mutator mutator;
    auto& handshake = mutator.GetHandshakeState();
    bool afterMutatorSafe = true;
    bool afterHandshakeSafe = true;
    std::thread requester([&] {
        ThreadLocal::SetMutator(&mutator);
        ThreadLocal::SetThreadType(ThreadType::CJ_PROCESSOR);
        mutator.SetInSaferegion(Mutator::SAFE_REGION_FALSE);
        handshake.leave_safe();
        queue.add_and_wait(owner);
        afterMutatorSafe = mutator.InSaferegion();
        afterHandshakeSafe = handshake.observed_safe();
        ThreadLocal::SetMutator(nullptr);
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    });
    ZForwarding* selected = nullptr;
    while ((selected = queue.synchronize_poll()) == nullptr) std::this_thread::yield();
    const bool waitingMutatorSafe = mutator.InSaferegion();
    const bool waitingHandshakeSafe = handshake.observed_safe();
    selected->release_page();
    selected->mark_done();
    queue.leave();
    requester.join();
    queue.deactivate();
    GC_EXPECT_FALSE(waitingMutatorSafe);
    GC_EXPECT_FALSE(waitingHandshakeSafe);
    GC_EXPECT_FALSE(afterMutatorSafe);
    GC_EXPECT_FALSE(afterHandshakeSafe);
    GC_EXPECT_TRUE(owner->is_done());
}
