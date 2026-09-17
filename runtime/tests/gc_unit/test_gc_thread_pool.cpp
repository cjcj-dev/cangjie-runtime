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
#include "Heap/z/zStat.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zRelocate.hpp"
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
    static void ParkFrom(RegionManager& manager, ZPage* region)
    {
        manager.fromRegionList.PrependRegion(region);
    }

#if defined(MRT_TESTABLE_INTERNALS)
    static void BindCollector(CollectorResources& resources, CopyCollector& collector)
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

// zPage.inline.hpp object_iterate walks a dense allocation interval.
void PlaceOwnerObjects(GcHeapFixture& fx)
{
    fx.obj0 = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.obj1 = fx.PlaceObject(fx.region1->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(fx.obj0) + fx.obj0->GetSize());
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(fx.obj1) + fx.obj1->GetSize());
}

void PrepareOwnerRegion(GcHeapFixture& fx)
{
    PlaceOwnerObjects(fx);
    // Relocation may compact in place and transfer remembered slots.
    ZPage* region = fx.region0;
    region->SetRegionListOwner(nullptr);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, fx.obj0));
    // zRelocationSet.cpp:79-134 freezes the selected set before preparation.
    RegionList selected("runtime-workers-selected");
    selected.PrependRegion(region);
        GC_EXPECT_TRUE(BeginForwardingArena(Generation::Old, selected));
        (void)selected.TakeHeadRegion();
        region->MarkForwardingDone();
}

bool InstallOwnerReceipt(GcHeapFixture& fx, MAddress& from, MAddress& to)
{
    PlaceOwnerObjects(fx);
    // CompleteRelocationRequests consumes the current relocation set through
    // ForwardingTable::FindTo.  Build that exact active product state instead
    // of planting a retired table (which FindTo deliberately stopped scanning
    // when relocation-set reset was aligned with ZGC).
    ZPage* region = fx.region0;
    region->SetRegionListOwner(nullptr);
    from = reinterpret_cast<MAddress>(fx.obj0);
    to = reinterpret_cast<MAddress>(fx.obj1);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, fx.obj0));
    // zRelocationSet.cpp:79-134 freezes the selected set before preparation.
    RegionList selected("runtime-workers-selected");
    selected.PrependRegion(region);
        GC_EXPECT_TRUE(BeginForwardingArena(Generation::Old, selected));
        (void)selected.TakeHeadRegion();
        ForwardingEntries* entries = generation_forwarding_table(region->GetOwnerGeneration()).get(region->GetRegionStart());
    if (entries == nullptr || entries->insert(from, to) != to) {
        return false;
    }
    // The mapping is ready, but page completion belongs to the worker.
    // ZRelocateTask marks done after processing the claimed forwarding.
    region->SetInGhostRegion(1);
    return true;
}

bool RunParallelProductEntryClosesGeneration()
{
    GcHeapFixture fx;
    RegionManager manager;
    PrepareOwnerRegion(fx);

    ZRelocateQueue& queue = manager.GetZRelocateQueue();
    RelocationReceiptTestAccess::ParkFrom(manager, fx.region0);
    ZStatWorkers statWorkers;
    ZWorkers workers(GCCycleGeneration::OLD, 3, &statWorkers);
    workers.set_active();
    manager.ForwardFromRegions<Generation::Old>(workers);
    workers.set_inactive();
    const bool closed = !queue.IsActive() && queue.PendingCount() == 0;
    return closed;
}

bool RunSerialProductEntryClosesGeneration()
{
    GcHeapFixture fx;
    RegionManager manager;
    PrepareOwnerRegion(fx);

    ZRelocateQueue& queue = manager.GetZRelocateQueue();
    RelocationReceiptTestAccess::ParkFrom(manager, fx.region0);
    // ZRelocate uses the generation worker entry even with one participant.
    ZStatWorkers statWorkers;
    ZWorkers workers(GCCycleGeneration::OLD, 1, &statWorkers);
    workers.set_active();
    manager.ForwardFromRegions<Generation::Old>(workers);
    workers.set_inactive();
    return !queue.IsActive() && queue.PendingCount() == 0;
}

#if defined(MRT_TESTABLE_INTERNALS)
class YoungForwardRuntimeCollector : public WCollector {
public:
    YoungForwardRuntimeCollector(Allocator& allocator, CollectorResources& resources)
        : WCollector(allocator, resources) {}

    void ForwardYoungFromRuntimeEntry()
    {
        GetGenerationCycle(GCCycleGeneration::YOUNG).SelectReason(GC_REASON_YOUNG);
        ForwardFromSpace(GCCycleGeneration::YOUNG);
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

    ZRelocateQueue& queue = manager.GetZRelocateQueue();

    YoungForwardRuntimeCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
#if defined(MRT_TESTABLE_INTERNALS)
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), collector);
#endif
    collector.GetGenerationCycle(GCCycleGeneration::YOUNG).InitializeWorkers(1);
    ZStat::Initialize();
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

    ZRelocateQueue& queue = manager.GetZRelocateQueue();
    queue.BeginWorkers(1);
    const auto added = queue.Add(fx.region0, from);
    if (!added.accepted) {
        return false;
    }
    fromSpace.PrependRegion(fx.region0);
    ForwardTask<Generation::Old> task(manager, fromSpace);
    task.work();
    return added.request->state() == ZRelocateQueue::State::COMPLETED &&
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

// ZGC has no dedicated gtest for the relocation worker entry. These
// task-result checks exercise WorkerTaskDispatcher's complete-before-return
// contract through the generation worker set that ZRelocate uses.
GC_TEST(RelocateWorkers, FixedParticipantsCompleteEachBorrowedTask)
{
    for (uint32_t count : { 1u, 3u }) {
        ZStatWorkers statWorkers;
        ZWorkers workers(GCCycleGeneration::OLD, count, &statWorkers);
        class Task : public ZTask {
        public:
            explicit Task(uint32_t count) : ZTask("ZWorkersUnitVisits"), visits(count, 0), handles(count) {}
            void work() override
            {
                const uint32_t id = WorkerThread::worker_id();
                ++visits[id];
                handles[id] = pthread_self();
            }
            std::vector<uint32_t> visits;
            std::vector<pthread_t> handles;
        } task(count);
        workers.run(&task);
        workers.run(&task);
        GC_EXPECT_EQ(workers.active_workers(), count);
        std::vector<pthread_t> owned;
        workers.threads_do([&](WorkerThread* thread) { owned.push_back(thread->os_thread()); });
        GC_EXPECT_EQ(owned.size(), count);
        for (uint32_t id = 0; id < count; ++id) {
            GC_EXPECT_EQ(task.visits[id], 2u);
            GC_EXPECT_FALSE(pthread_equal(task.handles[id], pthread_self()));
            bool owns = false;
            for (pthread_t thread : owned) owns = owns || pthread_equal(task.handles[id], thread);
            GC_EXPECT_TRUE(owns);
        }
    }
}

GC_TEST(RelocateWorkers, YoungAndOldOwnDistinctThreads)
{
    ZStatWorkers youngStats, oldStats;
    ZWorkers young(GCCycleGeneration::YOUNG, 1, &youngStats);
    ZWorkers old(GCCycleGeneration::OLD, 1, &oldStats);
    std::vector<pthread_t> youngThreads;
    young.threads_do([&](WorkerThread* thread) { youngThreads.push_back(thread->os_thread()); });
    GC_EXPECT_EQ(youngThreads.size(), 1u);
    unsigned oldThreads = 0;
    old.threads_do([&](WorkerThread* thread) {
        ++oldThreads;
        for (pthread_t other : youngThreads) GC_EXPECT_FALSE(pthread_equal(thread->os_thread(), other));
    });
    GC_EXPECT_EQ(oldThreads, 1u);
}

GC_TEST(RelocateWorkers, RelocationRequestHasOneCompletionOwnerBeforeRunReturns)
{
    GcHeapFixture fx;
    MAddress from = 0, to = 0;
    GC_EXPECT_TRUE(InstallOwnerReceipt(fx, from, to));
    ZRelocateQueue queue;
    constexpr size_t kWorkers = 3;
    queue.BeginWorkers(kWorkers);
    const auto added = queue.Add(fx.region0, from);
    GC_EXPECT_TRUE(added.accepted);
    GC_EXPECT_TRUE(queue.IsActive());
    std::atomic<size_t> completionOwners{ 0 };
    ZStatWorkers statWorkers;
    ZWorkers workers(GCCycleGeneration::OLD, kWorkers, &statWorkers);
    class RequestTask : public ZTask {
    public:
        RequestTask(ZRelocateQueue& queue, std::atomic<size_t>& owners)
            : ZTask("ZWorkersUnitRequest"), queue(queue), completionOwners(owners) {}
        void work() override
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
        ZRelocateQueue& queue;
        std::atomic<size_t>& completionOwners;
    } task(queue, completionOwners);
    workers.run(&task);
    (void)queue.Wait(added.request);
    GC_EXPECT_EQ(added.request->page_forwarding()->find(from), to);
    GC_EXPECT_EQ(completionOwners.load(), 1U);
    GC_EXPECT_EQ(queue.CompletionCount(), 1U);
    GC_EXPECT_FALSE(queue.IsActive());
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(RelocateWorkers, ActualForwardTaskPreservesExternalClaimant)
{
    GcHeapFixture fx;
    MAddress from = 0, to = 0;
    GC_EXPECT_TRUE(InstallOwnerReceipt(fx, from, to));
    auto owner = forwarding_for_page(fx.region0);
    GC_EXPECT_TRUE(owner->claim());
    RegionManager manager;
    RegionList empty("gc-unit-claimed-page");
    auto& queue = manager.GetZRelocateQueue();
    queue.BeginWorkers(1);
    const auto request = queue.Add(owner);
    ForwardTask<Generation::Old> task(manager, empty);
    task.work();
    GC_EXPECT_FALSE(owner->is_done());
    GC_EXPECT_TRUE(request.request->state() == ZRelocateQueue::State::CLAIMED);
    owner->release_page();
    owner->mark_done();
    GC_EXPECT_EQ(queue.Complete(owner), 1U);
    GC_EXPECT_TRUE(request.request->state() == ZRelocateQueue::State::COMPLETED);
}

GC_TEST(RelocateWorkers, ClaimLoserWaitsForPageCompletionAndFindsEntry)
{
    GcHeapFixture fx;
    MAddress from = 0, to = 0;
    GC_EXPECT_TRUE(InstallOwnerReceipt(fx, from, to));
    auto owner = forwarding_for_page(fx.region0);
    GC_EXPECT_TRUE(owner->claim());
    RegionManager manager;
    RegionList empty("gc-unit-external-owner");
    auto& queue = manager.GetZRelocateQueue();
    queue.BeginWorkers(2);
    const auto request = queue.Add(owner);
    std::atomic<MAddress> answer{ 0 };
    std::thread waiter([&] {
        (void)queue.Wait(request.request);
        answer.store(owner->find(from), std::memory_order_release);
    });
    ForwardTask<Generation::Old> task(manager, empty);
    std::thread worker([&] { task.work(); });
    while (queue.SynchronizedWorkerCount() != 1) std::this_thread::yield();
    const bool pending = !owner->is_done() && answer.load(std::memory_order_acquire) == 0;
    owner->release_page();
    owner->mark_done();
    (void)queue.Complete(owner);
    const bool closed = queue.SynchronizePoll().workersDone;
    worker.join(); waiter.join();
    GC_EXPECT_TRUE(pending);
    GC_EXPECT_TRUE(closed);
    GC_EXPECT_EQ(answer.load(), to);
}
#endif

GC_TEST(RelocateWorkers, ProductParallelEntryRegistersWorkersAndClosesGeneration)
{
    ExpectIsolatedScenarioPasses<RunParallelProductEntryClosesGeneration>();
}

GC_TEST(RelocateWorkers, ProductSerialEntryRegistersWorkerAndClosesGeneration)
{
    ExpectIsolatedScenarioPasses<RunSerialProductEntryClosesGeneration>();
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(RelocateWorkers, ProductYoungRuntimeEntryClosesRelocationRequestGeneration)
{
    ExpectIsolatedScenarioPasses<RunYoungRuntimeProductEntry>();
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(RelocateWorkers, ActualForwardTaskCompletesClaimedOwnerAtRegionExit)
{
    ExpectIsolatedScenarioPasses<RunActualTaskClaimedOwnerSuccess>();
}
#endif
