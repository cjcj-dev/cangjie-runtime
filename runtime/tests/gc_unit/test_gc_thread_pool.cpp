// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>

#include "gc_heap_fixture.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Common/Runtime.h"
#include "Mutator/MutatorManager.h"
#include "gc_unittest.hpp"

extern "C" int CJ_ScheduleManagerInit();

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

class RelocationReceiptTest {
public:
    static void ParkFrom(RegionManager&, ZPage* region)
    {
        region->SetRegionRole(ZPageRole::From);
    }

#if defined(MRT_TESTABLE_INTERNALS)
    static void BindCollector(Heap& collector)
    {
        CHECK(&collector == &Heap::GetHeap());
    }
    static void ForwardYoungFromRuntimeEntry(Heap& collector)
    {
        Heap::GetHeap().GetZGeneration(ZGenerationId::young).SelectReason(GC_REASON_YOUNG);
        auto& young = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
        ZRelocate::StartRelocationTasks(young.id());
        young.relocate().relocate(&young.relocation_set());
    }
#endif
};

} // namespace MapleRuntime

namespace {

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
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, fx.obj0));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region1, fx.obj1));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Old, {region, fx.region1}));
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
    from = reinterpret_cast<MAddress>(fx.obj0);
    to = reinterpret_cast<MAddress>(fx.obj1);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, fx.obj0));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region1, fx.obj1));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Old, {region, fx.region1}));
        ForwardingEntries* entries = generation_forwarding_table(region->GetOwnerGeneration()).get(region->GetRegionStart());
    if (entries == nullptr || entries->insert(from, to) != to) {
        return false;
    }
    // Only the source is pending in this claimant fixture. The second sparse
    // page was needed for selection; publish its completed identity before
    // exercising a worker with an externally claimed source.
    auto* companion = forwarding_for_page(fx.region1);
    GC_EXPECT_TRUE(companion != nullptr && companion->claim());
    GC_EXPECT_EQ(companion->insert(to, to), to);
    companion->release_page();
    companion->mark_done();
    // The mapping is ready, but page completion belongs to the worker.
    // ZRelocateTask marks done after processing the claimed forwarding.
    region->SetInGhostRegion(1);
    return true;
}

bool RunParallelProductEntryClosesGeneration()
{
    GcHeapFixture fx;
    auto& manager = Heap::GetHeap().page_allocator();
    PrepareOwnerRegion(fx);

    ZRelocateQueue& queue = generation_relocate_queue(Generation::Old);
    RelocationReceiptTest::ParkFrom(manager, fx.region0);
    auto& old = Heap::GetHeap().old();
    if (old.Workers() == nullptr) old.InitializeWorkers(3);
    old.Workers()->set_active_workers(3);
    old.Workers()->set_active();
    ZRelocate::StartRelocationTasks(old.id());
    old.relocate().relocate(&old.relocation_set());
    old.Workers()->set_inactive();
    const bool closed = !queue.IsActive() && queue.PendingCount() == 0;
    return closed;
}

bool RunSerialProductEntryClosesGeneration()
{
    GcHeapFixture fx;
    auto& manager = Heap::GetHeap().page_allocator();
    PrepareOwnerRegion(fx);

    ZRelocateQueue& queue = generation_relocate_queue(Generation::Old);
    RelocationReceiptTest::ParkFrom(manager, fx.region0);
    // ZRelocate uses the generation worker entry even with one participant.
    auto& old = Heap::GetHeap().old();
    if (old.Workers() == nullptr) old.InitializeWorkers(1);
    old.Workers()->set_active_workers(1);
    old.Workers()->set_active();
    ZRelocate::StartRelocationTasks(old.id());
    old.relocate().relocate(&old.relocation_set());
    old.Workers()->set_inactive();
    return !queue.IsActive() && queue.PendingCount() == 0;
}

#if defined(MRT_TESTABLE_INTERNALS)
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

    ZRelocateQueue& queue = generation_relocate_queue(Generation::Young);

    Heap& collector = Heap::GetHeap();
#if defined(MRT_TESTABLE_INTERNALS)
    RelocationReceiptTest::BindCollector(collector);
#endif
    Heap::GetHeap().GetZGeneration(ZGenerationId::young).InitializeWorkers(1);
    ZStat::Initialize();
    RelocationReceiptTest::ForwardYoungFromRuntimeEntry(collector);

    return !queue.IsActive() && queue.PendingCount() == 0;
}
#endif

} // namespace

// ZGC has no dedicated gtest for the relocation worker entry. These
// task-result checks exercise WorkerTaskDispatcher's complete-before-return
// contract through the generation worker set that ZRelocate uses.
GC_TEST(RelocateWorkers, FixedParticipantsCompleteEachBorrowedTask)
{
    for (uint32_t count : { 1u, 3u }) {
        ZStatWorkers statWorkers;
        ZWorkers workers(ZGenerationId::old, count, &statWorkers);
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
    ZWorkers young(ZGenerationId::young, 1, &youngStats);
    ZWorkers old(ZGenerationId::old, 1, &oldStats);
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
    ZWorkers workers(ZGenerationId::old, kWorkers, &statWorkers);
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
                    auto* forwarding = selected.forwarding;
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
    (void)queue.Wait(added.forwarding);
    GC_EXPECT_EQ(added.forwarding->find(from), to);
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
    auto& queue = generation_relocate_queue(Generation::Old);
    queue.BeginWorkers(1);
    const auto request = queue.Add(owner);
    // ForwardTask polls the owning generation's workers, as the runtime entry
    // does. This component fixture must provide that existing dependency.
    auto& old = Heap::GetHeap().old();
    if (old.Workers() == nullptr) old.InitializeWorkers(1);
    ForwardTask<Generation::Old> task(manager, &Heap::GetHeap().GetZGeneration(Generation::Old).relocation_set());
    WorkerFixture workerIdentity;
    task.work();
    GC_EXPECT_FALSE(owner->is_done());
    GC_EXPECT_TRUE(request.state() == ZRelocateQueue::State::CLAIMED);
    owner->release_page();
    owner->mark_done();
    GC_EXPECT_EQ(queue.Complete(owner), 1U);
    GC_EXPECT_TRUE(request.state() == ZRelocateQueue::State::COMPLETED);
}

GC_TEST(RelocateWorkers, ClaimLoserWaitsForPageCompletionAndFindsEntry)
{
    GcHeapFixture fx;
    MAddress from = 0, to = 0;
    GC_EXPECT_TRUE(InstallOwnerReceipt(fx, from, to));
    auto owner = forwarding_for_page(fx.region0);
    GC_EXPECT_TRUE(owner->claim());
    RegionManager manager;
    auto& queue = generation_relocate_queue(Generation::Old);
    queue.BeginWorkers(2);
    const auto request = queue.Add(owner);
    auto& old = Heap::GetHeap().old();
    if (old.Workers() == nullptr) old.InitializeWorkers(1);
    std::atomic<MAddress> answer{ 0 };
    std::thread waiter([&] {
        (void)queue.Wait(request.forwarding);
        answer.store(owner->find(from), std::memory_order_release);
    });
    bool pending = false;
    {
        ForwardTask<Generation::Old> task(manager, &Heap::GetHeap().GetZGeneration(Generation::Old).relocation_set());
        std::thread worker([&] { WorkerFixture workerIdentity; task.work(); });
        // ZGC zRelocate.cpp:1211: an ordinary worker leaves when its iterator
        // is exhausted; task destruction deactivates after all work joins.
        worker.join();
        pending = !owner->is_done() && answer.load(std::memory_order_acquire) == 0;
        owner->release_page();
        owner->mark_done();
        (void)queue.Complete(owner);
        queue.leave(); // the externally claimed page's participant completes
        waiter.join();
    }
    const bool closed = !queue.IsActive() && queue.PendingCount() == 0;
    GC_EXPECT_TRUE(pending);
    GC_EXPECT_TRUE(closed);
    GC_EXPECT_EQ(answer.load(), to);
}
#endif

GC_OTHER_VM_TEST(RelocateWorkers, ProductParallelEntryRegistersWorkersAndClosesGeneration)
{
    GC_EXPECT_TRUE(RunParallelProductEntryClosesGeneration());
}

GC_OTHER_VM_TEST(RelocateWorkers, ProductSerialEntryRegistersWorkerAndClosesGeneration)
{
    GC_EXPECT_TRUE(RunSerialProductEntryClosesGeneration());
}

namespace {
// Park the real relocation workers using the queue's normal GC synchronization
// protocol, then request resize while that product task is already running.
// No test callback or timing race decides when the request is issued.
void ResizeRunningRelocation(ZGeneration& generation, uint32_t initial = 1, uint32_t requested = 3)
{
    auto* queue = generation.relocate().queue();
    queue->synchronize();
    ZRelocate::StartRelocationTasks(generation.id());
    std::thread relocating([&] { generation.relocate().relocate(&generation.relocation_set()); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (queue->SynchronizedWorkerCount() != initial && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool running = queue->SynchronizedWorkerCount() == initial;
    generation.Workers()->request_resize_workers(requested);
    queue->desynchronize();
    relocating.join();
    GC_EXPECT_TRUE(running);
}
}

// ZGC zRelocate.cpp:1193-1224 and zWorkers.cpp:108-124. A request made
// during the real relocation task must restart it with the new worker budget.
GC_OTHER_VM_TEST(RelocateWorkers, ProductEntryRestartsWithRequestedWorkers)
{
    GcHeapFixture fx;
    PrepareOwnerRegion(fx);
    auto& old = Heap::GetHeap().old();
    auto& manager = Heap::GetHeap().page_allocator();
    RelocationReceiptTest::ParkFrom(manager, fx.region0);
    if (old.Workers() == nullptr) old.InitializeWorkers(3);
    old.Workers()->set_active_workers(1);
    old.Workers()->set_active();
    ResizeRunningRelocation(old);
    const auto active = old.Workers()->active_workers();
    old.Workers()->set_inactive();
    GC_EXPECT_EQ(active, 3u);
    GC_EXPECT_TRUE(forwarding_for_page(fx.region0)->is_done());
    GC_EXPECT_FALSE(old.relocate().queue()->is_active());
}

// The young branch must also select ZWorkers::run(ZRestartableTask*),
// including an empty installed set at the end of a relocation cycle.
GC_OTHER_VM_TEST(RelocateWorkers, YoungProductEntryRestartsWithRequestedWorkers)
{
    GcHeapFixture fx;
    auto& young = Heap::GetHeap().young();
    if (young.Workers() == nullptr) young.InitializeWorkers(3);
    young.Workers()->set_active_workers(1);
    young.Workers()->set_active();
    ResizeRunningRelocation(young);
    const auto active = young.Workers()->active_workers();
    young.Workers()->set_inactive();
    GC_EXPECT_EQ(active, 3u);
    GC_EXPECT_FALSE(young.relocate().queue()->is_active());
}

namespace {
// Retained source pages stop real workers in detach_page (or the in-place
// claim wait). Three pages hold the initial batch; the fourth holds its
// successor. This leaves ordinary forwarding work pending at the observation
// point, so ZWorkers' end-of-task resize cannot satisfy the assertion.
// ZGC zRelocate.cpp:1206-1214, zForwarding.cpp:86-157.
void CheckResizeBeforeRemainingForwarding(Generation id)
{
    GcHeapFixture fx;
    ZPage* pages[4] = {fx.region0, fx.region1, nullptr, nullptr};
    const PageAge age = id == Generation::Young ? PageAge::eden : PageAge::old;
    for (size_t i = 0; i < 4; ++i) {
        if (pages[i] == nullptr) {
            pages[i] = ZPage::InitRegion(ZPage::GranuleIndex(fx.heapStart) + i,
                                       ZGranuleSize, ZPageType::small);
            PublishAllocatedPage(pages[i]);
        }
        pages[i]->reset(age);
        auto* object = fx.PlaceObject(pages[i]->GetRegionStart());
        // This isolated relocation fixture starts after the promotion barrier.
        // Supply its input contract (ZGC zRelocate.cpp:742-749); the assertions
        // below observe worker resizing, not promotion-barrier production.
        ZIterator::basic_oop_iterate(object, [](RefField<>& field) {
            field.StoreColoured(ZAddress::store_good(zaddress::null));
        });
        pages[i]->SetRegionAllocPtr(reinterpret_cast<MAddress>(object) + object->GetSize());
        GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(pages[i], object));
    }
    GC_EXPECT_TRUE(BeginForwardingArena(id, {pages[0], pages[1], pages[2], pages[3]}));
    auto& generation = Heap::GetHeap().GetZGeneration(id);
    auto* queue = generation.relocate().queue();
    ZForwarding* owners[4] = {};
    ZRelocationSetIterator iterator(&generation.relocation_set());
    for (auto& owner : owners) {
        GC_EXPECT_TRUE(iterator.next(&owner));
        GC_EXPECT_TRUE(owner->retain_page(queue));
    }
    if (generation.Workers() == nullptr) generation.InitializeWorkers(3);
    auto* workers = generation.Workers();
    workers->set_active_workers(3);
    workers->set_active();
    ZRelocate::StartRelocationTasks(generation.id());
    std::thread relocating([&] { generation.relocate().relocate(&generation.relocation_set()); });
    const auto waitUntil = [](const auto& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return predicate();
    };
    const bool initialBatch = waitUntil([&] {
        return owners[0]->is_claimed() && owners[1]->is_claimed() && owners[2]->is_claimed();
    });
    const bool remainingUnclaimed = !owners[3]->is_claimed();
    workers->request_resize_workers(1);
    for (size_t i = 0; i < 3; ++i) owners[i]->release_page();
    const bool remainingClaimed = waitUntil([&] { return owners[3]->is_claimed(); });
    uint32_t activeWhilePending;
    {
        // Same lock as the product resize writer, avoiding an unsynchronized
        // read of WorkerThreads' active count while a batch is restarting.
        std::lock_guard<std::mutex> lock(*workers->resizing_lock());
        activeWhilePending = workers->active_workers();
    }
    const bool remainingPending = !owners[3]->is_done();
    owners[3]->release_page();
    relocating.join();
    const auto finalActive = workers->active_workers();
    workers->set_inactive();
    bool completed = true;
    for (auto* owner : owners) completed = completed && owner->is_done();
    std::fprintf(stderr,
        "RELOCATE_RESIZE_PENDING generation=%s initial_batch=%d remaining_unclaimed=%d "
        "remaining_claimed=%d remaining_pending=%d active_while_pending=%u final_active=%u\n",
        id == Generation::Young ? "young" : "old", initialBatch, remainingUnclaimed,
        remainingClaimed, remainingPending, activeWhilePending, finalActive);
    GC_EXPECT_TRUE(initialBatch);
    GC_EXPECT_TRUE(remainingUnclaimed);
    GC_EXPECT_TRUE(remainingClaimed);
    GC_EXPECT_TRUE(remainingPending);
    std::fprintf(stderr, "ASSERT_RELOCATE_RESIZE_BEFORE_COMPLETION executed=1 active=%u expected=1\n",
                 activeWhilePending);
    GC_EXPECT_EQ(activeWhilePending, 1u);
    GC_EXPECT_EQ(finalActive, 1u);
    GC_EXPECT_TRUE(completed);
    GC_EXPECT_FALSE(queue->is_active());
}
}

GC_OTHER_VM_TEST(RelocateWorkers, OldProductEntryReducesWorkersDuringRelocation)
{
    CheckResizeBeforeRemainingForwarding(Generation::Old);
}

GC_OTHER_VM_TEST(RelocateWorkers, YoungProductEntryReducesWorkersDuringRelocation)
{
    CheckResizeBeforeRemainingForwarding(Generation::Young);
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(RelocateWorkers, ProductYoungRuntimeEntryClosesRelocationRequestGeneration)
{
    GC_EXPECT_TRUE(RunYoungRuntimeProductEntry());
}
#endif

// The collected heap owns the runtime pool (ZGC zCollectedHeap.cpp:310-311),
// independently of either generation's GC worker set.
GC_TEST(RuntimeWorkers, CollectedHeapOwnsActiveRuntimePool)
{
    WorkerThreads* workers = ZCollectedHeap::heap()->safepoint_workers();
    GC_EXPECT_TRUE(workers != nullptr);
    const uint32_t count = workers->max_workers();
    GC_EXPECT_TRUE(count > 0);
    GC_EXPECT_EQ(workers->active_workers(), count);
    GC_EXPECT_EQ(workers->created_workers(), count);
    class RuntimeResultTask final : public WorkerTask {
    public:
        explicit RuntimeResultTask(uint32_t count)
            : WorkerTask("RuntimeResultTask"), size(count), results(new std::atomic<uint32_t>[count])
        {
            for (uint32_t i = 0; i < size; ++i) results[i].store(0);
        }
        void work(uint32_t id) override
        {
            if (id >= size || WorkerThread::worker_id() != id) {
                invalid.fetch_add(1);
                return;
            }
            results[id].fetch_add(1);
        }
        const uint32_t size;
        std::unique_ptr<std::atomic<uint32_t>[]> results;
        std::atomic<uint32_t> invalid{0};
    } task(count);
    workers->run_task(&task);
    std::fprintf(stderr, "RUNTIME_WORKERS_RESULT workers=%u invalid=%u\n", count, task.invalid.load());
    GC_EXPECT_EQ(task.invalid.load(), 0u);
    for (uint32_t i = 0; i < count; ++i) {
        GC_EXPECT_EQ(task.results[i].load(), 1u);
    }
}

// Runtime threads are created by the real collected-heap constructor. Exercise
// its normal shutdown in an exec-isolated process so other fixtures keep theirs.
GC_OTHER_VM_TEST(RuntimeWorkers, HeapStopJoinsRuntimePool)
{
    WorkerThreads* workers = ZCollectedHeap::heap()->safepoint_workers();
    GC_EXPECT_TRUE(workers != nullptr);
    const uint32_t createdBefore = workers->created_workers();
    const uint32_t activeBefore = workers->active_workers();
    GC_EXPECT_TRUE(createdBefore > 0);
    GC_EXPECT_TRUE(activeBefore > 0);

    Heap::GetHeap().StopGCWork();
    std::fprintf(stderr, "RUNTIME_WORKERS_STOP_RESULT before_created=%u before_active=%u created=%u active=%u\n",
                 createdBefore, activeBefore, workers->created_workers(), workers->active_workers());
    GC_EXPECT_EQ(workers->created_workers(), 0u);
    GC_EXPECT_EQ(workers->active_workers(), 0u);

    // The main entry also stops an existing heap before static destruction.
    Heap::GetHeap().StopGCWork();
    GC_EXPECT_EQ(workers->created_workers(), 0u);
    GC_EXPECT_EQ(workers->active_workers(), 0u);
}

// The external teardown-order runner observes this real main/heap at the
// shutdown and completion boundaries; the body never stops the pool itself.
GC_OTHER_VM_TEST(RuntimeWorkers, ActivePoolBeforeHarnessShutdown)
{
    WorkerThreads* workers = ZCollectedHeap::heap()->safepoint_workers();
    GC_EXPECT_TRUE(workers != nullptr);
    GC_EXPECT_TRUE(workers->created_workers() > 0);
    GC_EXPECT_TRUE(workers->active_workers() > 0);
    std::fprintf(stderr, "RUNTIME_WORKERS_LIVE created=%u active=%u\n",
                 workers->created_workers(), workers->active_workers());
}
