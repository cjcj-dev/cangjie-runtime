// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <thread>
#include "Heap/z/zRelocate.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Mutator/Handshake.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Common/Runtime.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
struct PageQueueFixture {
    GcHeapFixture heap;
    ZForwarding* owner;
    RelocationRequestQueue queue;
    PageQueueFixture()
    {
        auto* page = heap.region0;
        heap.InstallPageOwner(page);
        GC_EXPECT_TRUE(UNUSED_InstallPublication(
            page->GetRegionStart(), page->GetRegionSize(), page, page->GetOwnerGeneration()));
        GC_EXPECT_TRUE(UNUSED_PublishFromPageView(page, nullptr, 1, page->GetRegionAllocPtr(),
            page->GetRegionStart(), 1, 0, page->GetRegionLifeId()));
        owner = forwarding_for_page(page);
        GC_EXPECT_TRUE(static_cast<bool>(owner));
    }
    ~PageQueueFixture()
    {
        if (owner->ref_count().load(std::memory_order_acquire) != 0) owner->release_page();
        owner->mark_done();
        UNUSED_ClearPageOwner(heap.region0);
        owner = {};
    }
    void Publish()
    {
        const MAddress from = reinterpret_cast<MAddress>(heap.obj0);
        auto publication = forwarding_for_page(heap.region0, from);
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        GC_EXPECT_EQ(UNUSED_InsertMapping(publication, from,
                     reinterpret_cast<MAddress>(heap.obj1)), reinterpret_cast<MAddress>(heap.obj1));
    }
    void Complete()
    {
        owner->mark_done();
        (void)queue.Complete(owner);
    }
};

// Observe the product wait predicate without registering a synthetic mutator
// with the runtime's global thread list. The two states are the actual inputs
// to EnsurePhaseTransition and HandshakeState::try_process respectively.
#if defined(MRT_TESTABLE_INTERNALS)
class PageQueueRuntime final : public Runtime {
public:
    explicit PageQueueRuntime(MutatorManager& manager)
    {
        mutatorManager = &manager;
        runtime = this;
    }
    ~PageQueueRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam{}; }
    void SetGCThreshold(uint64_t) override {}
};

struct WaitContext {
    MutatorManager manager;
    PageQueueRuntime runtime{manager};
    Mutator mutator;
    Mutator* savedMutator = ThreadLocal::GetMutator();
    ThreadType savedType = ThreadLocal::GetThreadType();
    HandshakeState handshake{ThreadLocal::GetThreadLocalData()};
    bool entered = false;
    bool mutatorSafe = true;
    bool handshakeSafe = true;
    ZForwarding* observed = nullptr;
    static thread_local WaitContext* current;

    WaitContext()
    {
        Handshake::BindCurrent(&handshake);
        ThreadLocal::SetMutator(&mutator);
        ThreadLocal::SetThreadType(ThreadType::CJ_PROCESSOR);
        mutator.SetInSaferegion(Mutator::SAFE_REGION_FALSE);
        handshake.leave_safe();
        current = this;
        RelocationRequestQueue::SetWaitEnterHook(&Observe);
    }
    ~WaitContext()
    {
        RelocationRequestQueue::SetWaitEnterHook(nullptr);
        current = nullptr;
        ThreadLocal::SetMutator(savedMutator);
        Handshake::BindCurrent(nullptr);
        ThreadLocal::SetThreadType(savedType);
    }
    static void Observe(ZForwarding* forwarding)
    {
        current->entered = true;
        current->observed = forwarding;
        current->mutatorSafe = ThreadLocal::GetMutator()->InSaferegion();
        current->handshakeSafe = Handshake::Current().observed_safe();
    }
};
thread_local WaitContext* WaitContext::current = nullptr;
#endif
}

// ZRelocateQueue::add_and_wait (zRelocate.cpp:134-151), called from the
// JRT_LEAF barrier (zBarrierSetRuntime.cpp:29): waiting preserves the context
// that prevents reset until the final forwarding lookup has returned.
#if defined(MRT_TESTABLE_INTERNALS)
// A fresh VM owns this synthetic thread context; no runtime registration is needed.
GC_OTHER_VM_TEST(RelocationPageQueue, WaitPreservesMutatorAndHandshakeContext)
{
    PageQueueFixture f;
    f.queue.BeginWorkers(1);
    auto request = f.queue.Add(f.owner);
    WaitContext context;
    bool timedOut = false;
    (void)f.queue.WaitUntil(request.request, 1, &timedOut);
    GC_EXPECT_TRUE(context.entered);
    GC_EXPECT_TRUE(context.observed == f.owner);
    GC_EXPECT_TRUE(timedOut);
    GC_EXPECT_FALSE(context.mutatorSafe);
    GC_EXPECT_FALSE(context.handshakeSafe);
    GC_EXPECT_FALSE(context.mutator.InSaferegion());
    GC_EXPECT_FALSE(context.handshake.observed_safe());

    f.Publish();
    f.owner->release_page();
    f.Complete();
    (void)f.queue.Wait(request.request);
    GC_EXPECT_EQ(request.request->page_forwarding()->find(reinterpret_cast<MAddress>(f.heap.obj0)),
                 reinterpret_cast<MAddress>(f.heap.obj1));
    GC_EXPECT_FALSE(context.mutator.InSaferegion());
    GC_EXPECT_FALSE(context.handshake.observed_safe());
    GC_EXPECT_TRUE(f.queue.SynchronizePoll().workersDone);
}

#endif // MRT_TESTABLE_INTERNALS

// zRelocate.cpp:134-191: objects on one page share one forwarding/claim/done.
GC_TEST(RelocationPageQueue, TwoObjectsShareOnePageClaim)
{
    PageQueueFixture f;
    f.queue.BeginWorkers(1);
    auto first = f.queue.Add(f.heap.region0, reinterpret_cast<MAddress>(f.heap.obj0));
    auto second = f.queue.Add(f.heap.region0, reinterpret_cast<MAddress>(f.heap.obj0) + 8);
    GC_EXPECT_TRUE(first.accepted && first.inserted && second.accepted && !second.inserted);
    GC_EXPECT_TRUE(first.request == second.request);
    GC_EXPECT_EQ(f.queue.PendingCount(), 1U);
    std::atomic<unsigned> winners{ 0 };
    std::thread a([&] { if (f.queue.PruneAndClaim()) ++winners; });
    std::thread b([&] { if (f.queue.PruneAndClaim()) ++winners; });
    a.join(); b.join();
    GC_EXPECT_EQ(winners.load(), 1U);
    GC_EXPECT_TRUE(first.request->page_forwarding() == f.owner);
    f.Complete();
    GC_EXPECT_TRUE(f.queue.SynchronizePoll().workersDone);
}

GC_TEST(RelocationPageQueue, EntryPublicationDoesNotCompleteThePage)
{
    PageQueueFixture f;
    f.queue.BeginWorkers(1);
    auto request = f.queue.Add(f.owner);
    f.Publish();
    bool timedOut = false;
    (void)f.queue.WaitUntil(request.request, 1, &timedOut);
    GC_EXPECT_TRUE(timedOut);
    GC_EXPECT_FALSE(f.owner->is_done());
    GC_EXPECT_EQ(forwarding_find(f.heap.region0->GetOwnerGeneration(), reinterpret_cast<MAddress>(f.heap.obj0)),
                 reinterpret_cast<MAddress>(f.heap.obj1));
    f.Complete();
    (void)f.queue.WaitUntil(request.request, 1, &timedOut);
    GC_EXPECT_FALSE(timedOut);
    GC_EXPECT_TRUE(f.queue.SynchronizePoll().workersDone);
}

GC_TEST(RelocationPageQueue, ReleasedPageStillHasItsImmutableEntry)
{
    PageQueueFixture f;
    f.Publish();
    f.owner->release_page();
    GC_EXPECT_FALSE(f.owner->retain_page(&f.queue));
    const auto answer = LookupTo(
        reinterpret_cast<MAddress>(f.heap.obj0), f.heap.region0->GetOwnerGeneration());
    GC_EXPECT_TRUE(answer.answer == FwdLookup::ArmedHit);
    GC_EXPECT_EQ(answer.to, reinterpret_cast<MAddress>(f.heap.obj1));
    GC_EXPECT_FALSE(f.owner->is_done());
}

GC_TEST(RelocationPageQueue, DoneBeforeEnqueueNeedsNoWorker)
{
    PageQueueFixture f;
    f.Complete();
    auto request = f.queue.Add(f.owner);
    GC_EXPECT_TRUE(request.accepted && !request.inserted);
    bool timedOut = true;
    (void)f.queue.WaitUntil(request.request, 1, &timedOut);
    GC_EXPECT_FALSE(timedOut);
    GC_EXPECT_TRUE(request.request->page_forwarding() == f.owner);
}

GC_TEST(RelocationPageQueue, ClosedGenerationRejectsUnownedWork)
{
    PageQueueFixture f;
    f.queue.BeginWorkers(1);
    GC_EXPECT_TRUE(f.queue.SynchronizePoll().workersDone);
    const auto rejected = f.queue.Add(f.owner);
    GC_EXPECT_FALSE(rejected.accepted);
    GC_EXPECT_TRUE(rejected.request == nullptr);
    GC_EXPECT_FALSE(f.owner->is_done());
    // Positive control: a real already-claimed page retains its completion owner.
    GC_EXPECT_TRUE(f.owner->claim());
    const auto claimed = f.queue.Add(f.owner);
    GC_EXPECT_TRUE(claimed.accepted);
    f.Complete();
    (void)f.queue.Wait(claimed.request);
}

GC_TEST(RelocationPageQueue, EnqueueWakesSynchronizedWorker)
{
    PageQueueFixture f;
    f.queue.BeginWorkers(2);
    RelocationRequestQueue::Selection selected;
    std::thread worker([&] { selected = f.queue.SynchronizePoll(); });
    while (f.queue.SynchronizedWorkerCount() != 1) std::this_thread::yield();
    const auto request = f.queue.Add(f.owner);
    worker.join();
    GC_EXPECT_TRUE(selected.request == request.request);
    GC_EXPECT_TRUE(f.owner->claimed().load(std::memory_order_acquire));
    f.Complete();
    std::thread first([&] { GC_EXPECT_TRUE(f.queue.SynchronizePoll().workersDone); });
    GC_EXPECT_TRUE(f.queue.SynchronizePoll().workersDone);
    first.join();
}
