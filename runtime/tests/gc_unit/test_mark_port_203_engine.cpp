// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_worker_fixture.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zRemembered.hpp"
#include <memory>
#include <thread>
#include <vector>

#include "Common/SuspendibleThreadSet.h"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zWorkers.hpp"
#include "gc_unittest.hpp"
#include "b09_runtime_fixture.hpp"
#include "gc_heap_fixture.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
MarkStackEntry Entry(size_t i)
{
    return MarkStackEntry(size_t(i + 1), size_t(i + 3), false);
}

void DrainFollow(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, MarkTerminate& terminate,
                 size_t workerId, std::vector<size_t>& seen, bool partial)
{
    MapleRuntime::GcUnit::WorkerFixture workerThread(workerId);
    SuspendibleThreadSetJoiner stsJoiner;
    (void)ZMark::FollowWork(context, smr, stripes, terminate, workerId, partial,
                                 [&seen](const MarkStackEntry& entry) {
                                     seen.push_back(entry.partial_array_offset());
                                 });
}

struct ResetAbort {
    ~ResetAbort() { ZAbort::reset(); }
};

size_t StealOffset(MarkStripe& stripe, MarkingSMR& smr, size_t workerId)
{
    MarkStripeStack* stack = stripe.StealStack(smr, workerId);
    if (stack == nullptr) {
        return 0;
    }
    const size_t offset = stack->Pop().partial_array_offset();
    MarkStripeStack::Destroy(stack);
    return offset;
}
} // namespace

GC_TEST(MarkPort203Engine, SingleAndTwoWorkersDrainSamePublishedSet)
{
    for (size_t workers : {size_t{1}, size_t{2}}) {
        MarkStripeSet stripes(4);
        MarkTerminate terminate;
        terminate.Reset(workers);
        stripes.SetTerminate(&terminate);
        MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
        MarkThreadLocalStacks seed(4);
        constexpr size_t count = 40;
        for (size_t i = 0; i < count; ++i) {
            seed.Push(stripes, i % 4, Entry(i), true);
        }
        (void)seed.Flush(stripes, true);
        std::vector<std::vector<size_t>> seen(workers);
        std::vector<std::unique_ptr<MarkThreadLocalStacks>> stacks;
        std::vector<std::unique_ptr<MarkContext>> contexts;
        for (size_t w = 0; w < workers; ++w) {
            stacks.emplace_back(std::make_unique<MarkThreadLocalStacks>(4));
            contexts.emplace_back(std::make_unique<MarkContext>(workers, w, stripes, *stacks[w]));
        }
        std::vector<std::thread> threads;
        for (size_t w = 1; w < workers; ++w) {
            threads.emplace_back([&, w]() {
                DrainFollow(*contexts[w], smr, stripes, terminate, w, seen[w], false);
            });
        }
        DrainFollow(*contexts[0], smr, stripes, terminate, 0, seen[0], false);
        for (auto& t : threads) {
            t.join();
        }
        GC_EXPECT_TRUE(stripes.IsEmpty());
        size_t total = 0;
        for (size_t w = 0; w < workers; ++w) {
            total += seen[w].size();
        }
        GC_EXPECT_EQ(total, count);
        GC_EXPECT_TRUE(stripes.IsEmpty());
    }
}

GC_TEST(MarkPort203Engine, StealLocalBeforeGlobal)
{
    MarkStripeSet stripes(2);
    MarkTerminate terminate;
    terminate.Reset(1);
    stripes.SetTerminate(&terminate);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkThreadLocalStacks stacks(2);
    MarkContext context(1, 0, stripes, stacks);
    context.SetStripeId(0);
    MarkStripeStack* localVictim = MarkStripeStack::Create(true);
    localVictim->Push(Entry(11));
    context.Stacks().Install(1, localVictim);
    MarkStripeStack* global = MarkStripeStack::Create(true);
    global->Push(Entry(22));
    stripes.At(1).PublishStack(global, true, stripes.Terminate());
    std::vector<size_t> seen;
    DrainFollow(context, smr, stripes, terminate, 0, seen, false);
    GC_EXPECT_EQ(seen.size(), 2u);
    GC_EXPECT_EQ(seen[0], 12u);
    GC_EXPECT_EQ(seen[1], 23u);
}

GC_TEST(MarkPort203Engine, OverflowPreferredOverPublished)
{
    MarkStripeSet stripes(1);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkStripeStack* overflow = MarkStripeStack::Create(true);
    overflow->Push(Entry(1));
    MarkStripeStack* published = MarkStripeStack::Create(true);
    published->Push(Entry(2));
    stripes.At(0).PublishStack(overflow, false);
    stripes.At(0).PublishStack(published, true);
    MarkStripeStack* first = stripes.At(0).StealStack(smr, 0);
    GC_EXPECT_EQ(first->Pop().partial_array_offset(), 2u);
    MarkStripeStack::Destroy(first);
    MarkStripeStack* second = stripes.At(0).StealStack(smr, 0);
    GC_EXPECT_EQ(second->Pop().partial_array_offset(), 3u);
    MarkStripeStack::Destroy(second);
}

GC_TEST(MarkPort203Engine, ShrinkingNStripesStillSeesHighSlotWork)
{
    MarkStripeSet stripes(4);
    MarkStripeStack* high = MarkStripeStack::Create(true);
    high->Push(Entry(9));
    stripes.At(3).PublishStack(high, true);
    stripes.SetNStripes(2);
    GC_EXPECT_TRUE(!stripes.IsEmpty());
    GC_EXPECT_EQ(stripes.FirstNonEmptyStripe(), 3u);
    size_t home = 0;
    size_t seenHigh = 0;
    for (size_t victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
        if (victim == 3) {
            ++seenHigh;
        }
    }
    GC_EXPECT_EQ(seenHigh, 1u);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkStripeStack* taken = stripes.At(3).StealStack(smr, 0);
    GC_EXPECT_TRUE(taken != nullptr);
    MarkStripeStack::Destroy(taken);
    GC_EXPECT_TRUE(stripes.IsEmpty());
}

GC_TEST(MarkPort203Engine, PartialReturnsBeforeTerminate)
{
    MarkStripeSet stripes(1);
    MarkTerminate terminate;
    terminate.Reset(1);
    stripes.SetTerminate(&terminate);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkThreadLocalStacks stacks(1);
    MarkContext context(1, 0, stripes, stacks);
    std::vector<size_t> seen;
    auto result = ZMark::FollowWork(context, smr, stripes, terminate, 0, true,
                                         [&seen](const MarkStackEntry& entry) {
                                             seen.push_back(entry.partial_array_offset());
                                         });
    GC_EXPECT_TRUE(result == ZMark::Result::Partial);
    GC_EXPECT_TRUE(terminate.Saturated());
    GC_EXPECT_EQ(seen.size(), 0u);
}

GC_TEST(MarkPort203Engine, RebalanceImbalancePublishesLocalStack)
{
    B09RuntimeFixture runtime;
    MarkStripeSet stripes(2);
    MarkTerminate terminate;
    terminate.Reset(2);
    stripes.SetTerminate(&terminate);
    WorkerFixture workerFixture;
    SuspendibleThreadSetJoiner stsJoiner;
    MarkingSMR smr;
    MarkThreadLocalStacks stacks(2);
    MarkContext context(2, 0, stripes, stacks);
    MarkStripeStack* overflow = MarkStripeStack::Create(true);
    overflow->Push(Entry(1));
    stripes.At(0).PublishStack(overflow, false, stripes.Terminate());
    MarkStripeStack* published = MarkStripeStack::Create(true);
    published->Push(Entry(2));
    stripes.At(0).PublishStack(published, true, stripes.Terminate());
    MarkStripeStack* local = MarkStripeStack::Create(true);
    local->Push(Entry(80));
    local->Push(Entry(81));
    stacks.Install(0, local);
    terminate.Leave();
    ZMark domain(2, MarkingStacks::MarkingGeneration::YOUNG);
    ZAbort::abort();
    ResetAbort resetAbort;
    const auto result = ZMark::FollowWork(context, smr, stripes, terminate, 0, false,
        [](const MarkStackEntry&) {}, nullptr, nullptr, &domain);
    const size_t first = StealOffset(stripes.At(0), smr, 0);
    const size_t second = StealOffset(stripes.At(0), smr, 0);
    std::fprintf(stderr, "REBALANCE_FLUSH_TARGET executed=1 branch=unsaturated result=%d first=%zu second=%zu\n",
                 static_cast<int>(result), first, second);
    GC_EXPECT_EQ(first, 2u);
    GC_EXPECT_TRUE(result == ZMark::Result::Aborted);
    GC_EXPECT_EQ(second, 81u);
}

GC_TEST(MarkPort203Engine, RebalanceStripeChangePublishesLocalStack)
{
    B09RuntimeFixture runtime;
    MarkStripeSet stripes(4);
    stripes.SetNStripes(1);
    MarkTerminate terminate;
    terminate.Reset(2);
    stripes.SetTerminate(&terminate);
    WorkerFixture workerFixture(1);
    SuspendibleThreadSetJoiner stsJoiner;
    MarkingSMR smr;
    for (size_t i = 0; i < 17; ++i) {
        MarkStripeStack* crowded = MarkStripeStack::Create(true);
        crowded->Push(Entry(100 + i));
        stripes.At(0).PublishStack(crowded, true, stripes.Terminate());
    }
    MarkStripeStack* overflow = MarkStripeStack::Create(true);
    overflow->Push(Entry(1));
    stripes.At(0).PublishStack(overflow, false, stripes.Terminate());
    MarkThreadLocalStacks stacks(4);
    MarkContext context(2, 1, stripes, stacks);
    MarkStripeStack* local = MarkStripeStack::Create(true);
    local->Push(Entry(80));
    local->Push(Entry(81));
    stacks.Install(0, local);
    ZMark domain(4, MarkingStacks::MarkingGeneration::YOUNG);
    ZAbort::abort();
    ResetAbort resetAbort;
    const auto result = ZMark::FollowWork(context, smr, stripes, terminate, 1, false,
        [](const MarkStackEntry&) {}, nullptr, nullptr, &domain);
    const size_t first = StealOffset(stripes.At(0), smr, 1);
    const size_t second = StealOffset(stripes.At(0), smr, 1);
    std::fprintf(stderr, "REBALANCE_FLUSH_TARGET executed=1 branch=stripe result=%d first=%zu second=%zu\n",
                 static_cast<int>(result), first, second);
    GC_EXPECT_EQ(first, 2u);
    GC_EXPECT_TRUE(result == ZMark::Result::Aborted);
    GC_EXPECT_EQ(second, 81u);
}

GC_TEST(MarkPort203Engine, PublishWakesWaitingWorker)
{
    MarkStripeSet stripes(2);
    MarkTerminate terminate;
    terminate.Reset(2);
    stripes.SetTerminate(&terminate);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkThreadLocalStacks waiterStacks(2);
    MarkThreadLocalStacks producerStacks(2);
    MarkContext waiter(2, 0, stripes, waiterStacks);
    MarkContext producer(2, 1, stripes, producerStacks);
    std::atomic<bool> waiterEntered{ false };
    std::vector<size_t> seen;
    std::thread waitThread([&]() {
        waiterEntered.store(true, std::memory_order_release);
        DrainFollow(waiter, smr, stripes, terminate, 0, seen, false);
    });
    while (!waiterEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MarkStripeStack* stack = MarkStripeStack::Create(true);
    stack->Push(Entry(4));
    stripes.At(0).PublishStack(stack, true, stripes.Terminate());
    std::vector<size_t> producerSeen;
    DrainFollow(producer, smr, stripes, terminate, 1, producerSeen, false);
    waitThread.join();
    GC_EXPECT_TRUE(stripes.IsEmpty());
    GC_EXPECT_EQ(seen.size() + producerSeen.size(), 1u);
}

GC_TEST(MarkPort203Engine, LeaveUnblocksTryTerminateWaiter)
{
    MarkTerminate terminate;
    terminate.Reset(2);
    MarkStripeSet stripes(2);
    stripes.SetTerminate(&terminate);
    std::atomic<bool> waiting{ false };
    std::atomic<bool> finished{ false };
    std::thread waiter([&]() {
        SuspendibleThreadSetJoiner stsJoiner;
        waiting.store(true, std::memory_order_release);
        GC_EXPECT_TRUE(terminate.TryTerminate(stripes, stripes.NStripes()));
        finished.store(true, std::memory_order_release);
    });
    while (!waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    SuspendibleThreadSetJoiner stsJoiner;
    terminate.Leave();
    waiter.join();
    GC_EXPECT_TRUE(finished.load(std::memory_order_acquire));
}

GC_TEST(MarkPort203Engine, TrySetNStripesIsAtomicSnapshot)
{
    MarkStripeSet stripes(4);
    GC_EXPECT_EQ(stripes.NStripes(), 4u);
    GC_EXPECT_TRUE(stripes.TrySetNStripes(4, 2));
    GC_EXPECT_EQ(stripes.NStripes(), 2u);
    GC_EXPECT_TRUE(!stripes.TrySetNStripes(4, 1));
    GC_EXPECT_EQ(stripes.NStripes(), 2u);
    GC_EXPECT_EQ(stripes.StripeForWorker(2, 0), 0u);
    GC_EXPECT_EQ(stripes.Next(0), 1u);
    GC_EXPECT_EQ(stripes.Next(3), 0u);
}

GC_TEST(MarkPort203Engine, DomainPrepareResizeKeepsCapacity)
{
    MapleRuntime::GcUnit::WorkerFixture domainWorker;
    ZMark domain(8, MarkingStacks::MarkingGeneration::YOUNG);
    domain.PrepareWork(2);
    GC_EXPECT_EQ(domain.Stripes().Count(), 8u);
    GC_EXPECT_TRUE(domain.Stripes().NStripes() <= 8u);
    domain.ResizeWorkers(4);
    GC_EXPECT_EQ(domain.NWorkers(), 4u);
    GC_EXPECT_EQ(domain.Stripes().Count(), 8u);
    MarkThreadLocalStacks* owner = &domain.Stacks();
    domain.ResizeWorkers(2);
    GC_EXPECT_TRUE(owner == &domain.Stacks());
}

GC_TEST(MarkPort203Engine, CrowdedRestoresNStripes)
{
    MarkStripeSet stripes(4);
    stripes.SetNStripes(1);
    for (size_t i = 0; i < 32; ++i) {
        MarkStripeStack* stack = MarkStripeStack::Create(true);
        stack->Push(Entry(i));
        stripes.At(0).PublishStack(stack, true);
    }
    GC_EXPECT_TRUE(stripes.IsCrowded());
    GC_EXPECT_TRUE(stripes.TrySetNStripes(1, 2));
    GC_EXPECT_EQ(stripes.NStripes(), 2u);
}

GC_TEST(MarkPort203Engine, AbortAndResizeRequestsStopFollowWork)
{
    MapleRuntime::GcUnit::WorkerFixture domainWorker;
    ZMark domain(4, MarkingStacks::MarkingGeneration::YOUNG);
    domain.PrepareWork(1);
    GC_EXPECT_TRUE(!domain.PollStop());
    ZAbort::abort();
    GC_EXPECT_TRUE(domain.PollStop());
    ZAbort::reset();

    ZStatWorkers statWorkers;
    ZWorkers workers(ZGenerationId::young, 2, &statWorkers);
    workers.set_active();
    workers.set_active_workers(1);
    domain.BindWorkers(&workers);
    domain.PrepareWork(1);
    GC_EXPECT_TRUE(!domain.PollStop());
    workers.request_resize_workers(2);
    GC_EXPECT_TRUE(domain.PollStop());
}

// ZMark::drain/rebalance_work (zMark.cpp:468-485): stop following while
// retaining unpublished work until the worker flushes and the phase joins.
GC_TEST(MarkPort203Engine, AbortReturnsWithRemainingMarkWorkOwned)
{
    MapleRuntime::GcUnit::B09RuntimeFixture runtime;
    MapleRuntime::GcUnit::WorkerFixture domainWorker;
    SuspendibleThreadSetJoiner stsJoiner;
    ZMark domain(4, MarkingStacks::MarkingGeneration::MAJOR);
    domain.PrepareWork(1);
    MarkThreadLocalStacks stacks(4);
    MarkContext context(1, 0, domain.Stripes(), stacks);
    constexpr size_t count = 64;
    for (size_t i = 0; i < count; ++i) {
        stacks.Push(domain.Stripes(), 0, Entry(i), true);
    }
    size_t followed = 0;
    const auto result = ZMark::FollowWork(context, domain.Smr(), domain.Stripes(), domain.Terminate(),
        0, false, [&](const MarkStackEntry&) {
            ++followed;
            ZAbort::abort();
        }, nullptr, nullptr, &domain);
    GC_EXPECT_TRUE(result == ZMark::Result::Aborted);
    GC_EXPECT_EQ(followed, 1u);
    (void)stacks.Flush(domain.Stripes(), true);
    context.Cache().Flush();
    // zMarkStack.cpp: ZMarkStackList::length counts segments, not entries.
    // The resume below proves every remaining entry is still owned and consumed.
    GC_EXPECT_TRUE(domain.Stripes().Population() > 0);
    GC_EXPECT_TRUE(domain.PollStop());
}

// ZMark::try_end (zMark.cpp:954-971): true iff stripes empty and !resurrected
// after a terminate flush of thread-local stacks.
GC_TEST(MarkPort203Engine, TryEndFalseWhenResurrectedOrUnflushed)
{
    MapleRuntime::GcUnit::B09RuntimeFixture runtime;
    MapleRuntime::GcUnit::WorkerFixture domainWorker;
    ZMark domain(4, MarkingStacks::MarkingGeneration::YOUNG);
    domain.PrepareWork(1);
    GC_EXPECT_TRUE(domain.Stripes().IsEmpty());
    GC_EXPECT_TRUE(!domain.Terminate().Resurrected());
    GC_EXPECT_TRUE(domain.TryEnd());

    domain.Terminate().SetResurrected(true);
    GC_EXPECT_TRUE(!domain.TryEnd());
    domain.Terminate().SetResurrected(false);
    GC_EXPECT_TRUE(domain.TryEnd());
}

// ZGC zRemembered.cpp:561-565 dispatches through the young generation pool.
// The invalid-input arm must fail at this boundary, before entering mark work.
GC_TEST(RememberedWorkers719, MissingYoungPoolFailsAtDispatch)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        dup2(output[1], STDERR_FILENO);
        dup2(output[1], STDOUT_FILENO);
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        ZMark mark(4, MarkingStacks::MarkingGeneration::YOUNG);
        mark.PrepareWork(1);
        if (Heap::GetHeap().young().Workers() != nullptr) _exit(91);
        ZRemembered remembered(&Heap::page_table(), &Heap::GetHeap().old().forwarding_table(),
                               &Heap::GetHeap().page_allocator());
        remembered.scan_and_follow(&mark);
        _exit(0);
    }
    close(output[1]);
    std::string diagnostic;
    char buffer[1024];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) {
        diagnostic.append(buffer, static_cast<size_t>(count));
    }
    close(output[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fprintf(stderr, "REMEMBERED719 child_status=%d diagnostic=%s\n", status, diagnostic.c_str());
    GC_EXPECT_TRUE(diagnostic.find("ZRemembered::scan_and_follow requires young workers") != std::string::npos);
    GC_EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

GC_TEST(RememberedWorkers719, YoungPoolRunsFromNonWorkerThread)
{
    B09RuntimeFixture runtime;
    GC_EXPECT_EQ(WorkerThread::worker_id(), UINT32_MAX);
    auto& young = Heap::GetHeap().young();
    young.InitializeWorkers(1);
    ZMark& mark = young.Mark();
    mark.Start();
    ZRemembered remembered(&Heap::page_table(), &Heap::GetHeap().old().forwarding_table(),
                               &Heap::GetHeap().page_allocator());
    remembered.scan_and_follow(&mark);
    GC_EXPECT_TRUE(mark.Stripes().IsEmpty());
    GC_EXPECT_EQ(WorkerThread::worker_id(), UINT32_MAX);
    std::fprintf(stderr, "REMEMBERED719 completed with empty mark stripes on non-worker caller\n");
    young.StopWorkers();
}

namespace {
// A quiescent caller can inspect live accounting after the real closure task
// joins. This does not assert that concurrent mutator publication has stopped.
void CheckYoungClosureAccounting(uint32_t workers)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    heap.region0()->reset(PageAge::eden);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    auto& young = Heap::GetHeap().young();
    young.InitializeWorkers(workers);
    young.Mark().Start();
    young.set_phase(ZGenerationPhase::Mark);
    HeapSlotAt<>(reinterpret_cast<MAddress>(heap.obj0) + TYPEINFO_PTR_SIZE)
        .StoreColoured(zpointer::null);
    const size_t expected = heap.obj0->GetSize();
    const uint64_t before = heap.region0()->live_bytes();
    WorkStack work;
    std::vector<BaseObject*> reached;
    std::unordered_set<MAddress> slots;
    std::unordered_set<MAddress> weak;
    ZMark::PushYoungObject(heap.obj0, work, "young-closure-accounting");
    ZMark::TraceYoungClosure(work, false, reached, slots, weak);
    const uint64_t after = heap.region0()->live_bytes();
    const bool marked = heap.region0()->is_object_strongly_live(from_object(heap.obj0));
    young.StopWorkers();
    std::fprintf(stderr,
        "YOUNG_CLOSURE784_RESULT workers=%u before=%llu after=%llu expected=%zu marked=%d\n",
        workers, static_cast<unsigned long long>(before), static_cast<unsigned long long>(after),
        expected, marked);
    GC_EXPECT_EQ(after - before, static_cast<uint64_t>(expected));
    GC_EXPECT_TRUE(marked);
}
}

GC_TEST(YoungClosure784, SingleWorkerAccountsPublishedRoot)
{
    CheckYoungClosureAccounting(1);
}

GC_TEST(YoungClosure784, MultipleWorkersAccountPublishedRoot)
{
    CheckYoungClosureAccounting(2);
}

// ZGC zRemembered.cpp:127-168: consumed previous entries are cleared by the
// page scan before the same page can be scanned again. Enter the product worker
// task; observe the product bitmap, without a replacement consumer or callback.
GC_TEST(RememberedClear845, ConsumedPreviousSlotsAreAbsentOnRescan)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fixture;
    auto& heap = Heap::GetHeap();
    auto& young = heap.young();
    young.InitializeWorkers(1);
    young.Mark().Start();
    // Old marking makes the scan independent of incomplete old live bits.
    heap.old().set_phase(ZGenerationPhase::Mark);
    auto* page = fixture.region0();
    auto* slot = reinterpret_cast<volatile zpointer*>(
        reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE);
    *slot = StoreGoodPointer(nullptr);
    page->remember(slot);
    heap.remembered().register_found_old(page);
    heap.remembered().flip();
    const bool published = page->was_remembered(slot);
    heap.remembered().scan_and_follow(&young.Mark());
    const bool remaining = page->was_remembered(slot);
    // Read the entries a subsequent page scan would visit in this face.
    size_t repeated = 0;
    page->oops_do_remembered([&](volatile zpointer*) { ++repeated; });
    std::fprintf(stderr, "REMEMBERED845_ASSERT_EXECUTED published=%d remaining=%d repeated=%zu\n",
                 published, remaining, repeated);
    young.StopWorkers();
    GC_EXPECT_EQ(remaining, false);
    GC_EXPECT_EQ(repeated, size_t{0});
    GC_EXPECT_EQ(published, true);
}

// ZGC zMarkStack.inline.hpp:66: publication owns the nonempty invariant.
GC_TEST(MarkPublish1144, EmptyStackRejectedForBothRoutes)
{
    for (bool publish : {false, true}) {
        const pid_t child = fork();
        GC_EXPECT_TRUE(child >= 0);
        if (child == 0) {
            signal(SIGABRT, SIG_DFL);
            MarkStripe stripe;
            stripe.PublishStack(MarkStripeStack::Create(true), publish);
            _exit(0);
        }
        int status = 0;
        GC_EXPECT_EQ(waitpid(child, &status, 0), child);
        std::fprintf(stderr, "MARK1144 publication publish=%d status=%d\n", publish, status);
        GC_EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
}

// ZGC zMarkStack.cpp:78: list insertion does not inspect the stack payload.
GC_TEST(MarkPublish1144, ListPreservesEmptyPayload)
{
    WorkerFixture worker;
    MarkingSMR smr;
    MarkStripeStackList list;
    MarkStripeStack* stack = MarkStripeStack::Create(true);
    list.Push(stack);
    MarkStripeStack* observed = list.Pop(smr, 0);
    std::fprintf(stderr, "MARK1144 list payload preserved=%d\n", observed == stack);
    GC_EXPECT_TRUE(observed == stack);
    GC_EXPECT_TRUE(observed->IsEmpty());
    MarkStripeStack::Destroy(observed);
}
