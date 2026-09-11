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
    region->SetRouteState(RegionInfo::RouteState::COMPACTED);
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
    region->SetRouteState(RegionInfo::RouteState::COMPACTED);
    return true;
}

bool RunParallelProductEntryClosesGeneration()
{
    GcHeapFixture fx;
    RegionManager manager;
    PrepareOwnerRegion(fx);

    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    RelocationReceiptTestAccess::ParkFrom(manager, fx.region0);
    GCThreadPool pool("gc-unit-product-parallel", 2, GCPoolThread::GC_THREAD_PRIORITY);
    manager.ForwardFromRegions<Generation::Old>(&pool);
    const bool closed = !queue.IsActive() && queue.PendingCount() == 0;
    pool.Exit();
    return closed;
}

bool RunSerialProductEntryClosesGeneration()
{
    GcHeapFixture fx;
    RegionManager manager;
    PrepareOwnerRegion(fx);

    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    RelocationReceiptTestAccess::ParkFrom(manager, fx.region0);
    manager.ForwardFromRegions<Generation::Old>(nullptr);
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
    task.Execute(0);
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

GC_TEST(GCThreadPool, RelocationRequestHasOneCompletionOwnerBeforeWaitFinishReturns)
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
    GCThreadPool pool("gc-unit-relocate", 2, GCPoolThread::GC_THREAD_PRIORITY);
    for (size_t i = 0; i < kWorkers; ++i) {
        pool.AddWork(new LambdaWork([&](size_t) {
            for (;;) {
                auto selected = queue.SelectBeforeOrdinary([]() -> void* { return nullptr; });
                if (!selected) selected = queue.SynchronizePoll();
                if (selected.workersDone) return;
                if (selected.is_request()) {
                    auto* forwarding = selected.request->page_forwarding();
                    forwarding->release_page();
                    forwarding->mark_done();
                    completionOwners.fetch_add(1, std::memory_order_relaxed);
                    // A peer may prune the completed page before its claimant
                    // notifies the queue. Force that ordering: Complete's return
                    // counts remaining queue records, not completion owners.
                    (void)queue.PruneAndClaim();
                    (void)queue.Complete(forwarding);
                }
            }
        }));
    }
    pool.Start();
    pool.WaitFinish();
    (void)queue.Wait(added.request);
    pool.Exit();
    GC_EXPECT_EQ(added.request->page_forwarding()->find(from), to);
    GC_EXPECT_EQ(completionOwners.load(), 1U);
    GC_EXPECT_EQ(queue.CompletionCount(), 1U);
    GC_EXPECT_FALSE(queue.IsActive());
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(GCThreadPool, ActualForwardTaskPreservesExternalClaimant)
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
    task.Execute(0);
    GC_EXPECT_FALSE(owner->is_done());
    GC_EXPECT_TRUE(request.request->state() == RelocationRequestQueue::State::CLAIMED);
    owner->release_page();
    owner->mark_done();
    GC_EXPECT_EQ(queue.Complete(owner.get()), 1U);
    GC_EXPECT_TRUE(request.request->state() == RelocationRequestQueue::State::COMPLETED);
}

GC_TEST(GCThreadPool, ClaimLoserWaitsForPageCompletionAndFindsEntry)
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
    std::thread worker([&] { task.Execute(0); });
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

GC_TEST(GCThreadPool, ProductParallelEntryRegistersWorkersAndClosesGeneration)
{
    ExpectIsolatedScenarioPasses<RunParallelProductEntryClosesGeneration>();
}

GC_TEST(GCThreadPool, ProductSerialEntryRegistersWorkerAndClosesGeneration)
{
    ExpectIsolatedScenarioPasses<RunSerialProductEntryClosesGeneration>();
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(GCThreadPool, ProductYoungRuntimeEntryClosesRelocationRequestGeneration)
{
    ExpectIsolatedScenarioPasses<RunYoungRuntimeProductEntry>();
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(GCThreadPool, ActualForwardTaskCompletesClaimedOwnerAtRegionExit)
{
    ExpectIsolatedScenarioPasses<RunActualTaskClaimedOwnerSuccess>();
}
#endif
