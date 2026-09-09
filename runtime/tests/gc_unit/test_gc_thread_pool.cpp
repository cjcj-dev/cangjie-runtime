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
#include "Base/GcLog.h"
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
    // Product PrepareForwardableRegion snapshots the ghost extent before a
    // worker can claim the region.  gc_unit parks directly on a from-list, so
    // plant the same consumed state without invoking unrelated selection code.
    fx.region0->SetInGhostRegion(1);
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
    int lateOwner = 0;
    const auto late = queue.Add(&lateOwner, 0x73200040);
    const bool closed = !late.accepted && late.request->state() == RelocationRequestQueue::State::FAILED &&
        queue.PendingCount() == 0;
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
    int lateOwner = 0;
    const auto late = queue.Add(&lateOwner, 0x73200048);
    return !late.accepted && late.request->state() == RelocationRequestQueue::State::FAILED &&
        queue.PendingCount() == 0;
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

class CycleContextProductCollector final : public WCollector {
public:
    CycleContextProductCollector(Allocator& allocator, CollectorResources& resources)
        : WCollector(allocator, resources), resources(resources) {}

    bool SawOwnedYoungCycle() const
    {
        return sawPre && sawBody && sawPost && sequence != 0 && reason == GC_REASON_YOUNG &&
            phase == GCPhase::GC_PHASE_TRACE && startTimeSet && activeYoung && !activeOld;
    }

protected:
    void PreGarbageCollection(bool, uint64_t) override
    {
        sawPre = GetCycleContext().IsYoung() && GetGCReason() == GC_REASON_YOUNG;
        SetGCPhase(GCPhase::GC_PHASE_TRACE);
    }

    void DoGarbageCollection() override
    {
        const CycleSnapshot snapshot = resources.GetCycleSnapshot();
        sequence = GetCycleContext().sequence.load(std::memory_order_acquire);
        reason = GetGCReason();
        phase = snapshot.Phase(CycleGeneration::Young);
        startTimeSet = GetCycleContext().stats.gcStartTime != 0;
        activeYoung = snapshot.Active(CycleGeneration::Young);
        activeOld = snapshot.Active(CycleGeneration::Old);
        sawBody = true;
    }

    void PostGarbageCollection(uint64_t) override
    {
        sawPost = resources.GetCycleSnapshot().Active(CycleGeneration::Young);
    }

private:
    CollectorResources& resources;
    uint64_t sequence{ 0 };
    GCReason reason{ GC_REASON_USER };
    GCPhase phase{ GCPhase::GC_PHASE_UNDEF };
    bool activeYoung{ false };
    bool activeOld{ false };
    bool startTimeSet{ false };
    bool sawPre{ false };
    bool sawBody{ false };
    bool sawPost{ false };
};

bool RunCycleContextProductEntry()
{
    if (CJ_ScheduleManagerInit() != 0) {
        return false;
    }
    MutatorManager mutatorManager;
    YoungForwardTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    CycleContextProductCollector collector(Heap::GetHeap().GetAllocator(), resources);
    collector.RunGarbageCollection(17, GC_REASON_YOUNG);
    const CycleSnapshot after = resources.GetCycleSnapshot();
    return collector.SawOwnedYoungCycle() && !after.AnyActive() && GcLog::CurrentSeq() == 0;
}

bool RunStaleCycleTokenCannotMutateReplacement()
{
    if (CJ_ScheduleManagerInit() != 0) {
        return false;
    }
    MutatorManager mutatorManager;
    YoungForwardTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();

    const CycleToken first = resources.BeginCycle(101, GC_REASON_YOUNG);
    if (!resources.EndCycle(first)) {
        return false;
    }
    const CycleToken replacement = resources.BeginCycle(102, GC_REASON_YOUNG);
    const bool stalePublished = resources.PublishCyclePhase(first, GCPhase::GC_PHASE_TRACE);
    const CycleSnapshot afterStalePublish = resources.GetCycleSnapshot();
    const bool replacementEnded = resources.EndCycle(replacement);

    return first.sequence != replacement.sequence && !stalePublished && replacementEnded &&
        afterStalePublish.Active(CycleGeneration::Young) &&
        afterStalePublish.Phase(CycleGeneration::Young) == GCPhase::GC_PHASE_IDLE;
}

bool RunStaleCycleTokenCannotEndReplacement()
{
    if (CJ_ScheduleManagerInit() != 0) {
        return false;
    }
    MutatorManager mutatorManager;
    YoungForwardTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();

    const CycleToken first = resources.BeginCycle(201, GC_REASON_YOUNG);
    if (!resources.EndCycle(first)) {
        return false;
    }
    const CycleToken replacement = resources.BeginCycle(202, GC_REASON_YOUNG);
    const bool staleEnded = resources.EndCycle(first);
    const CycleSnapshot afterStaleEnd = resources.GetCycleSnapshot();
    const bool replacementEnded = resources.EndCycle(replacement);

    return first.sequence != replacement.sequence && !staleEnded && replacementEnded &&
        afterStaleEnd.Active(CycleGeneration::Young) &&
        afterStaleEnd.Phase(CycleGeneration::Young) == GCPhase::GC_PHASE_IDLE;
}

bool RunLegacyControlActivityOwnsMarkWindow()
{
    if (CJ_ScheduleManagerInit() != 0) {
        return false;
    }
    MutatorManager mutatorManager;
    YoungForwardTestRuntime runtime(mutatorManager);
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    CycleContextProductCollector collector(Heap::GetHeap().GetAllocator(), resources);
    const GCReason reasonBefore = resources.GetGCStats().reason;

    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_USER;
    collector.SetGCPhase(GCPhase::GC_PHASE_TRACE);
    const CycleToken token = resources.GetExecutionToken();
    const CycleSnapshot during = resources.GetCycleSnapshot();
    const bool owned = token.sequence != 0 && during.Active(CycleGeneration::Old) &&
        !during.Active(CycleGeneration::Young) && during.Phase(CycleGeneration::Old) == GCPhase::GC_PHASE_TRACE &&
        resources.GetCycleContext(token).reason == GC_REASON_USER;

    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    resources.GetGCStats().reason = reasonBefore;
    resources.SetGcStarted(false);
    const CycleSnapshot after = resources.GetCycleSnapshot();
    const bool completedOwnerVisible = resources.IsCompletedControlCycle(CycleGeneration::Old, token.sequence);
    const CycleToken replacement = resources.BeginCycle(301, GC_REASON_USER);
    const bool staleOwnerCleared = !resources.IsCompletedControlCycle(CycleGeneration::Old, token.sequence);
    const bool replacementEnded = resources.EndCycle(replacement);
    return owned && !after.AnyActive() && completedOwnerVisible && staleOwnerCleared && replacementEnded &&
        GcLog::CurrentSeq() == 0;
}

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

    int lateOwner = 0;
    const auto late = queue.Add(&lateOwner, 0x73300040);
    return !late.accepted && late.request->state() == RelocationRequestQueue::State::FAILED &&
        queue.PendingCount() == 0;
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
        queue.Wait(added.request) == to && queue.CompletionCount() == 1 && queue.PendingCount() == 0;
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
    constexpr size_t kWorkers = 3;
    constexpr MAddress kFrom = 0x6000;
    constexpr MAddress kTo = 0x7000;
    int owner = 0;
    RelocationRequestQueue queue;
    queue.BeginWorkers(kWorkers);
    const auto added = queue.Add(&owner, kFrom);
    std::atomic<size_t> completionOwners{ 0 };

    GCThreadPool pool("gc-unit-relocate", static_cast<int32_t>(kWorkers - 1),
                      GCPoolThread::GC_THREAD_PRIORITY);
    for (size_t i = 0; i < kWorkers; ++i) {
        pool.AddWork(new LambdaWork([&](size_t) {
            for (;;) {
                auto selected = queue.SelectBeforeOrdinary([]() -> void* { return nullptr; });
                if (!selected) {
                    selected = queue.SynchronizePoll();
                    if (selected.workersDone) {
                        return;
                    }
                }
                if (selected.is_request()) {
                    const size_t n = queue.CompleteOwner(&owner, [](MAddress from) {
                        return from == kFrom ? kTo : static_cast<MAddress>(0);
                    });
                    completionOwners.fetch_add(n, std::memory_order_relaxed);
                }
            }
        }));
    }
    pool.Start();
    pool.WaitFinish();
    GC_EXPECT_EQ(queue.Wait(added.request), kTo);
    GC_EXPECT_EQ(completionOwners.load(std::memory_order_relaxed), static_cast<size_t>(1));
    GC_EXPECT_EQ(queue.CompletionCount(), static_cast<uint64_t>(1));
    pool.Exit();
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(GCThreadPool, ActualForwardTaskClosesClaimedRequestExactlyOnceWhenOwnerExits)
{
    GcHeapFixture fx;
    RegionManager manager;
    RegionList emptyFromSpace("gc-unit-empty-from");
    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    const auto added = queue.Add(fx.region0, from);
    GC_EXPECT_TRUE(added.accepted);

    // Execute the same product HeapWork that ForwardFromRegions submits. The
    // owner is deliberately absent from the from list, modeling an installer
    // which lost ownership and exited before publishing a receipt.
    ForwardTask<Generation::Old> task(manager, emptyFromSpace);
    task.Execute(0);

    GC_EXPECT_TRUE(added.request->state() == RelocationRequestQueue::State::FAILED);
    GC_EXPECT_EQ(queue.Wait(added.request), static_cast<MAddress>(0));
    GC_EXPECT_EQ(queue.CompletionCount(), static_cast<uint64_t>(1));
    GC_EXPECT_EQ(queue.PendingCount(), static_cast<size_t>(0));
}

GC_TEST(GCThreadPool, ClaimLoserWaitsForOrdinaryOwnerReceiptInsteadOfKeepingFrom)
{
    GcHeapFixture fx;
    RegionManager manager;
    RegionList emptyFromSpace("gc-unit-ordinary-owner-won");
    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(2);
    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    const MAddress to = reinterpret_cast<MAddress>(fx.obj1);
    const auto added = queue.Add(fx.region0, from);
    GC_EXPECT_TRUE(added.accepted);

    std::atomic<bool> pageDone{ false };
    std::atomic<bool> waiterReturned{ false };
    std::atomic<MAddress> answer{ from };
    std::thread waiter([&]() {
        answer.store(queue.WaitUntil(
            added.request, [&]() { return pageDone.load(std::memory_order_acquire); }),
            std::memory_order_release);
        waiterReturned.store(true, std::memory_order_release);
    });
    JoinGuard waiterGuard(waiter);

    // This is the real product task and real TryDeleteRegion loser branch. The
    // owner is absent because an ordinary worker has already removed it. With
    // two registered workers the task then parks at generation synchronization,
    // giving the test a deterministic point before the ordinary owner publishes.
    ForwardTask<Generation::Old> task(manager, emptyFromSpace);
    std::thread requestWorker([&]() { task.Execute(0); });
    JoinGuard requestWorkerGuard(requestWorker);
    while (queue.SynchronizedWorkerCount() == 0) {
        std::this_thread::yield();
    }

    const bool claimedBeforePublication =
        added.request->state() == RelocationRequestQueue::State::CLAIMED;
    const bool returnedBeforePublication = waiterReturned.load(std::memory_order_acquire);
    const MAddress answerBeforePublication = answer.load(std::memory_order_acquire);

    // Ordinary-owner publication is the second signal. pageDone remains false,
    // so the only legal early return is the exact to receipt, never keep-from.
    const bool published = queue.Publish(from, to);
    for (size_t i = 0; i < 100 && !waiterReturned.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool returnedByReceipt = waiterReturned.load(std::memory_order_acquire);
    const bool pageDoneBeforeClose = pageDone.load(std::memory_order_acquire);
    const MAddress publishedAnswer = answer.load(std::memory_order_acquire);

    // Always close both signals before asserting. GC_EXPECT throws, so an
    // assertion before this cleanup would make a deliberate loser cut wait on
    // requestWorker's generation rendezvous instead of reporting one red item.
    pageDone.store(true, std::memory_order_release);
    const bool generationClosed = queue.SynchronizePoll().workersDone;
    requestWorker.join();
    waiter.join();

    GC_EXPECT_TRUE(claimedBeforePublication);
    GC_EXPECT_FALSE(returnedBeforePublication);
    GC_EXPECT_EQ(answerBeforePublication, from);
    GC_EXPECT_TRUE(published);
    GC_EXPECT_TRUE(returnedByReceipt);
    GC_EXPECT_FALSE(pageDoneBeforeClose);
    GC_EXPECT_EQ(publishedAnswer, to);
    GC_EXPECT_TRUE(generationClosed);
    GC_EXPECT_EQ(queue.CompletionCount(), static_cast<uint64_t>(1));
    GC_EXPECT_EQ(queue.PendingCount(), static_cast<size_t>(0));
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

GC_TEST(GCThreadPool, ProductCollectionPublishesAndClearsOwnedYoungCycle)
{
    ExpectIsolatedScenarioPasses<RunCycleContextProductEntry>();
}

GC_TEST(GCThreadPool, StaleCycleTokenCannotPublishIntoReplacementCycle)
{
    ExpectIsolatedScenarioPasses<RunStaleCycleTokenCannotMutateReplacement>();
}

GC_TEST(GCThreadPool, StaleCycleTokenCannotEndReplacementCycle)
{
    ExpectIsolatedScenarioPasses<RunStaleCycleTokenCannotEndReplacement>();
}

GC_TEST(GCThreadPool, LegacyControlActivityPublishesAndClearsOwnedCycle)
{
    ExpectIsolatedScenarioPasses<RunLegacyControlActivityOwnsMarkWindow>();
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(GCThreadPool, ActualForwardTaskCompletesClaimedOwnerAtRegionExit)
{
    ExpectIsolatedScenarioPasses<RunActualTaskClaimedOwnerSuccess>();
}
#endif
