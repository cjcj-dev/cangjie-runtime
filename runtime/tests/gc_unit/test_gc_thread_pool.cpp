// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"
#include "Heap/GcThreadPool.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Collector/RelocationRequestQueue.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/WCollector/WCollector.h"
#include "Common/Runtime.h"
#include "Mutator/MutatorManager.h"
#include "gc_unittest.hpp"

extern "C" int CJ_ScheduleManagerInit();

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct RelocationReceiptTestAccess {
    static void ParkFrom(RegionManager& manager, RegionInfo* region)
    {
        manager.fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
    }

#if defined(MRT_TESTABLE_INTERNALS)
    static void BindCollector(CollectorResources& resources, TracingCollector& collector)
    {
        resources.collectorProxy.currentCollector = &collector;
    }
#endif
};

} // namespace MapleRuntime

namespace {

int WaitChildExit(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void EnterIsolatedChild()
{
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        (void)dup2(devnull, STDERR_FILENO);
        (void)dup2(devnull, STDOUT_FILENO);
        (void)close(devnull);
    }
    (void)signal(SIGABRT, SIG_DFL);
}

void PrepareOwnerRegion(GcHeapFixture& fx)
{
    RegionInfo* region = fx.region0;
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    (void)bitmap->MarkBits(region->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0)),
                           fx.obj0->GetSize(), region->GetRegionSize());
    region->AddLiveByteCount(fx.obj0->GetSize());
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    region->MarkForwardingDone();
}

bool InstallOwnerReceipt(GcHeapFixture& fx, MAddress& from, MAddress& to)
{
    // CompleteRelocationRequests consumes the current relocation set through
    // ForwardingTable::FindTo.  Build that exact active product state instead
    // of planting a retired table (which FindTo deliberately stopped scanning
    // when relocation-set reset was aligned with ZGC).
    RegionInfo* region = fx.region0;
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    from = reinterpret_cast<MAddress>(fx.obj0);
    to = reinterpret_cast<MAddress>(fx.obj1);
    (void)bitmap->MarkBits(region->GetAddressOffset(from), fx.obj0->GetSize(), region->GetRegionSize());
    region->AddLiveByteCount(fx.obj0->GetSize());
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    ForwardingEntries* entries = ForwardingTable::GetEntries(region->GetRegionStart());
    if (entries == nullptr || entries->insert(from, to) != to) {
        return false;
    }
    // This station owns queue completion, not object copying.  A marked old
    // page with a completed in-place route takes ForwardRegion's region
    // exit, after which the real task must consume this active receipt through
    // CompleteRelocationRequests.
    region->SetInGhostRegion(1);
    region->MarkForwardingDone();
    return true;
}

bool RunParallelProductEntryClosesGeneration()
{
    GcHeapFixture fx;
    RegionManager manager;
    PrepareOwnerRegion(fx);

    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    RelocationReceiptTestAccess::ParkFrom(manager, fx.region0);
    GCWorkers workers(GCWorkers::Generation::OLD, 3);
    workers.SetActive();
    manager.ForwardFromRegions<Generation::Old>(workers);
    workers.SetInactive();
    const bool closed = !queue.IsActive() && queue.PendingCount() == 0;
    return closed;
}

bool RunSerialProductEntryClosesGeneration()
{
    GcHeapFixture fx;
    RegionManager manager;
    PrepareOwnerRegion(fx);

    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    RelocationReceiptTestAccess::ParkFrom(manager, fx.region0);
    manager.ForwardFromRegions<Generation::Old>();
    return !queue.IsActive() && queue.PendingCount() == 0;
}

#if defined(MRT_TESTABLE_INTERNALS)
class YoungForwardRuntimeCollector : public WCollector {
public:
    YoungForwardRuntimeCollector(Allocator& allocator, CollectorResources& resources)
        : WCollector(allocator, resources) {}

    void ForwardYoungFromRuntimeEntry()
    {
        SetGCReason(GC_REASON_YOUNG);
        ForwardFromSpace();
    }
};

class YoungForwardTestRuntime : public Runtime {
public:
    explicit YoungForwardTestRuntime(MutatorManager& manager)
    {
        mutatorManager = &manager;
        runtime = this;
    }

    ~YoungForwardTestRuntime() override { runtime = nullptr; }

    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}
};

bool RunYoungRuntimeProductEntry()
{
    // gc_unit does not start the language scheduler.  The product phase
    // transition still visits its real global mutator list, so initialize just
    // that list and its lock in this isolated child (which exits via _exit).
    if (CJ_ScheduleManagerInit() != 0) {
        return false;
    }
    MutatorManager mutatorManager;
    YoungForwardTestRuntime runtime(mutatorManager);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& manager = space.GetRegionManager();

    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();

    YoungForwardRuntimeCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
#if defined(MRT_TESTABLE_INTERNALS)
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), collector);
#endif
    collector.ForwardYoungFromRuntimeEntry();

    return !queue.IsActive() && queue.PendingCount() == 0;
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
bool RunActualTaskClaimedOwnerSuccess()
{
    GcHeapFixture fx;
    RegionManager manager;
    RegionList fromSpace("gc-unit-request-owner-from");
    MAddress from = 0;
    MAddress to = 0;
    if (!InstallOwnerReceipt(fx, from, to)) {
        return false;
    }

    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    const auto added = queue.Add(fx.region0, from);
    if (!added.accepted) {
        return false;
    }
    fromSpace.PrependRegion(fx.region0, RegionInfo::RegionType::FROM_REGION);
    ForwardTask<Generation::Old> task(manager, fromSpace);
    task.Work(0);
    return added.request->state() == RelocationRequestQueue::State::COMPLETED &&
        added.request->page_forwarding()->find(from) == to && queue.CompletionCount() == 1 && queue.PendingCount() == 0;
}
#endif

template<bool (*Scenario)()>
void ExpectIsolatedScenarioPasses()
{
    const pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        EnterIsolatedChild();
        _exit(Scenario() ? 0 : 1);
    }
    GC_EXPECT_EQ(WaitChildExit(pid), 0);
}

} // namespace

// ZRuntimeWorkers has no dedicated upstream gtest. These task-result checks
// exercise WorkerTaskDispatcher's complete-before-return contract directly.
GC_TEST(RuntimeWorkers, FixedParticipantsCompleteEachBorrowedTask)
{
    for (uint32_t count : { 1u, 3u }) {
        RuntimeWorkers workers(count);
        class Task : public GCWorkerTask {
        public:
            explicit Task(uint32_t count) : visits(count, 0), handles(count) {}
            void Work(uint32_t id) override
            {
                ++visits[id];
                handles[id] = pthread_self();
            }
            std::vector<uint32_t> visits;
            std::vector<pthread_t> handles;
        } task(count);
        workers.Run(task);
        workers.Run(task);
        GC_EXPECT_EQ(workers.ActiveWorkers(), count);
        std::vector<pthread_t> owned;
        workers.ThreadsDo([&](pthread_t thread) { owned.push_back(thread); });
        GC_EXPECT_EQ(owned.size(), count);
        for (uint32_t id = 0; id < count; ++id) {
            GC_EXPECT_EQ(task.visits[id], 2u);
            GC_EXPECT_TRUE(pthread_equal(task.handles[id], owned[id]));
            GC_EXPECT_FALSE(pthread_equal(task.handles[id], pthread_self()));
        }
    }
}

GC_TEST(RuntimeWorkers, RuntimeYoungAndOldOwnDistinctThreads)
{
    RuntimeWorkers runtime(2);
    GCWorkers young(GCWorkers::Generation::YOUNG, 1);
    GCWorkers old(GCWorkers::Generation::OLD, 1);
    std::vector<pthread_t> runtimeThreads;
    runtime.ThreadsDo([&](pthread_t thread) { runtimeThreads.push_back(thread); });
    young.ThreadsDo([&](pthread_t thread) {
        for (pthread_t other : runtimeThreads) GC_EXPECT_FALSE(pthread_equal(thread, other));
    });
    old.ThreadsDo([&](pthread_t thread) {
        for (pthread_t other : runtimeThreads) GC_EXPECT_FALSE(pthread_equal(thread, other));
    });
    GC_EXPECT_EQ(young.GetSnapshot().generation, GCWorkers::Generation::YOUNG);
    GC_EXPECT_EQ(old.GetSnapshot().generation, GCWorkers::Generation::OLD);
}

GC_TEST(RuntimeWorkers, RelocationRequestHasOneCompletionOwnerBeforeRunReturns)
{
    GcHeapFixture fx;
    MAddress from = 0, to = 0;
    GC_EXPECT_TRUE(InstallOwnerReceipt(fx, from, to));
    RelocationRequestQueue queue;
    constexpr size_t kWorkers = 3;
    queue.BeginWorkers(kWorkers);
    const auto added = queue.Add(fx.region0, from);
    GC_EXPECT_TRUE(added.accepted);
    GC_EXPECT_TRUE(queue.IsActive());
    std::atomic<size_t> completionOwners{ 0 };
    RuntimeWorkers workers(kWorkers);
    class RequestTask : public GCWorkerTask {
    public:
        RequestTask(RelocationRequestQueue& queue, std::atomic<size_t>& owners)
            : queue(queue), completionOwners(owners) {}
        void Work(uint32_t) override
        {
            for (;;) {
                auto selected = queue.SelectBeforeOrdinary([]() -> void* { return nullptr; });
                if (!selected) selected = queue.SynchronizePoll();
                if (selected.workersDone) return;
                if (selected.is_request()) {
                    auto* forwarding = selected.request->page_forwarding();
                    forwarding->release_page();
                    forwarding->mark_done();
                    completionOwners.fetch_add(1, std::memory_order_relaxed);
                    // Exercise completion after a peer has pruned the record.
                    (void)queue.PruneAndClaim();
                    (void)queue.Complete(forwarding);
                }
            }
        }
    private:
        RelocationRequestQueue& queue;
        std::atomic<size_t>& completionOwners;
    } task(queue, completionOwners);
    workers.Run(task);
    (void)queue.Wait(added.request);
    GC_EXPECT_EQ(added.request->page_forwarding()->find(from), to);
    GC_EXPECT_EQ(completionOwners.load(), 1U);
    GC_EXPECT_EQ(queue.CompletionCount(), 1U);
    GC_EXPECT_FALSE(queue.IsActive());
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(RuntimeWorkers, ActualForwardTaskPreservesExternalClaimant)
{
    GcHeapFixture fx;
    MAddress from = 0, to = 0;
    GC_EXPECT_TRUE(InstallOwnerReceipt(fx, from, to));
    auto owner = ForwardingTable::RetainPageOwner(fx.region0);
    GC_EXPECT_TRUE(owner->claim());
    RegionManager manager;
    RegionList empty("gc-unit-claimed-page");
    auto& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    const auto request = queue.Add(owner);
    ForwardTask<Generation::Old> task(manager, empty);
    task.Work(0);
    GC_EXPECT_FALSE(owner->is_done());
    GC_EXPECT_TRUE(request.request->state() == RelocationRequestQueue::State::CLAIMED);
    owner->release_page();
    owner->mark_done();
    GC_EXPECT_EQ(queue.Complete(owner.get()), 1U);
    GC_EXPECT_TRUE(request.request->state() == RelocationRequestQueue::State::COMPLETED);
}

GC_TEST(RuntimeWorkers, ClaimLoserWaitsForPageCompletionAndFindsEntry)
{
    GcHeapFixture fx;
    MAddress from = 0, to = 0;
    GC_EXPECT_TRUE(InstallOwnerReceipt(fx, from, to));
    auto owner = ForwardingTable::RetainPageOwner(fx.region0);
    GC_EXPECT_TRUE(owner->claim());
    RegionManager manager;
    RegionList empty("gc-unit-external-owner");
    auto& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(2);
    const auto request = queue.Add(owner);
    std::atomic<MAddress> answer{ 0 };
    std::thread waiter([&] {
        (void)queue.Wait(request.request);
        answer.store(owner->find(from), std::memory_order_release);
    });
    ForwardTask<Generation::Old> task(manager, empty);
    std::thread worker([&] { task.Work(0); });
    while (queue.SynchronizedWorkerCount() != 1) std::this_thread::yield();
    const bool pending = !owner->is_done() && answer.load(std::memory_order_acquire) == 0;
    owner->release_page();
    owner->mark_done();
    (void)queue.Complete(owner.get());
    const bool closed = queue.SynchronizePoll().workersDone;
    worker.join(); waiter.join();
    GC_EXPECT_TRUE(pending);
    GC_EXPECT_TRUE(closed);
    GC_EXPECT_EQ(answer.load(), to);
}
#endif

GC_TEST(RuntimeWorkers, ProductParallelEntryRegistersWorkersAndClosesGeneration)
{
    ExpectIsolatedScenarioPasses<RunParallelProductEntryClosesGeneration>();
}

GC_TEST(RuntimeWorkers, ProductSerialEntryRegistersWorkerAndClosesGeneration)
{
    ExpectIsolatedScenarioPasses<RunSerialProductEntryClosesGeneration>();
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(RuntimeWorkers, ProductYoungRuntimeEntryClosesRelocationRequestGeneration)
{
    ExpectIsolatedScenarioPasses<RunYoungRuntimeProductEntry>();
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(RuntimeWorkers, ActualForwardTaskCompletesClaimedOwnerAtRegionExit)
{
    ExpectIsolatedScenarioPasses<RunActualTaskClaimedOwnerSuccess>();
}
#endif
