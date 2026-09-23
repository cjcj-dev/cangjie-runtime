// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC has no gtest for zWorkers/workerThread. These exercise the product
// WorkerTaskDispatcher/WorkerThreads/ZWorkers/ZStatWorkers wiring directly:
//   1. one run(task) executes work() once per active worker, with distinct
//      thread-local worker ids below active_workers(), and returns only after
//      every worker has completed (gc/shared/workerThread.cpp:41-83);
//   2. the dispatcher carries no GC state: worker id and gc id are the only
//      values a task can observe (workerThread.cpp:68-73);
//   3. ZWorkers::run drives ZStatWorkers::at_start/at_end so the accumulated
//      parallel time is active_workers x run duration (zWorkers.cpp:92-106,
//      zStat.cpp:1338-1356).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "Heap/z/zGCIdPrinter.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.inline.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
class Task : public ZTask {
public:
    explicit Task(std::function<void()> fn) : ZTask("ZWorkersUnitTask"), fn(std::move(fn)) {}
    void work() override { fn(); }
private:
    std::function<void()> fn;
};

// A latch makes in-flight observations deterministic; timeout is only a
// harness escape and is never accepted as evidence of a product invariant.
class Latch {
public:
    void Add()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++count;
        cv.notify_all();
    }
    bool Wait(unsigned target)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(10), [&] { return count >= target; });
    }
private:
    std::mutex mutex;
    std::condition_variable cv;
    unsigned count = 0;
};

struct Record {
    std::mutex mutex;
    std::vector<uint32_t> ids;
    std::set<pthread_t> threads;
    pthread_t coordinator = pthread_self();
    void Add()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ids.push_back(WorkerThread::worker_id());
        threads.insert(pthread_self());
    }
    // Invariant 1: n executions with ids exactly {0..n-1}, none on the
    // coordinator. workerThread.cpp:63-70 hands ids out per start-semaphore
    // token, so a worker that finishes early may take a second token: the
    // number of distinct threads is between 1 and n, not necessarily n.
    void Check(unsigned n)
    {
        std::sort(ids.begin(), ids.end());
        GC_EXPECT_EQ(ids.size(), n);
        GC_EXPECT_TRUE(!threads.empty() && threads.size() <= n);
        GC_EXPECT_EQ(threads.count(coordinator), 0u);
        for (unsigned i = 0; i < n; ++i) GC_EXPECT_EQ(ids[i], i);
    }
};

struct Fixture {
    ZStatWorkers stats;
    ZWorkers workers;
    Fixture(ZGenerationId id, uint32_t max) : workers(id, max, &stats) {}
};
} // namespace

GC_TEST(ZWorkers, RunGivesEachActiveWorkerOneDistinctIdBelowActive)
{
    Fixture fx(ZGenerationId::young, 4);
    // zWorkers.cpp:60-64: all workers are created and active at construction.
    GC_EXPECT_EQ(fx.workers.active_workers(), 4u);
    fx.workers.set_active_workers(3);
    GC_EXPECT_EQ(fx.workers.active_workers(), 3u);
    Record result;
    Latch entered, release;
    // Holding every execution until all three have entered forces three
    // distinct worker threads to carry the three ids at the same time.
    Task task([&] { result.Add(); entered.Add(); (void)release.Wait(1); });
    std::thread coordinator([&] { fx.workers.run(&task); });
    JoinGuard guard(coordinator);
    const bool ready = entered.Wait(3);
    release.Add();
    coordinator.join();
    GC_EXPECT_TRUE(ready);
    result.Check(3);
    GC_EXPECT_EQ(result.threads.size(), 3u);
    // The coordinating thread is never a worker (workerThread.cpp:211).
    GC_EXPECT_EQ(WorkerThread::worker_id(), UINT32_MAX);
    std::puts("OBSERVED three distinct thread-local worker ids 0..2 on three simultaneously held worker threads");
}

GC_TEST(ZWorkers, RunAllUsesMaxWorkersAndRestoresActive)
{
    Fixture fx(ZGenerationId::young, 4);
    fx.workers.set_active_workers(1);
    for (unsigned count : {1u, 4u, 1u}) {
        Record result;
        Task task([&] { result.Add(); });
        if (count == 4) fx.workers.run_all(&task); else fx.workers.run(&task);
        result.Check(count);
        // zWorkers.cpp:126-137: run_all restores the previous active count.
        GC_EXPECT_EQ(fx.workers.active_workers(), 1u);
    }
    std::puts("OBSERVED IDs for single/full/restored batches");
}

GC_TEST(ZWorkers, IndependentGenerationSets)
{
    Fixture young(ZGenerationId::young, 3), old(ZGenerationId::old, 2);
    young.workers.set_active_workers(1);
    young.workers.set_active();
    old.workers.set_active();
    Record yr, orr;
    Latch entered, release;
    Task ot([&] { orr.Add(); entered.Add(); (void)release.Wait(1); });
    std::thread coordinator([&] { old.workers.run(&ot); });
    JoinGuard guard(coordinator);
    bool ready = entered.Wait(2);
    Task yt([&] { yr.Add(); });
    young.workers.run(&yt);
    const bool youngActive = young.workers.is_active();
    const bool oldActive = old.workers.is_active();
    release.Add();
    coordinator.join();
    GC_EXPECT_TRUE(ready);
    yr.Check(1);
    orr.Check(2);
    GC_EXPECT_TRUE(youngActive && oldActive);
    for (auto t : yr.threads) GC_EXPECT_EQ(orr.threads.count(t), 0u);
    std::puts("OBSERVED independent generation worker sets with disjoint threads");
}

// workerThread.cpp:41-61: the coordinator returns only after the last worker
// has decremented _not_finished to zero and signalled the end semaphore.
GC_OTHER_VM_TEST(ZWorkers, CoordinatorReturnsAfterEveryWorkerCompleted)
{
    // Isolate a broken completion protocol: a failing pool cannot safely run
    // its destructor's next dispatch. The child process owns that lifetime.
    auto fx = std::make_unique<Fixture>(ZGenerationId::old, 3);
    Latch entered, releaseFirst, releaseOthers, finished;
    std::atomic<unsigned> completed{0};
    std::atomic<bool> returned{false};
    unsigned completedAtReturn = 0;
    Task task([&] {
        entered.Add();
        if (!(WorkerThread::worker_id() == 0 ? releaseFirst : releaseOthers).Wait(1)) return;
        ++completed;
        finished.Add();
    });
    std::thread coordinator([&] {
        fx->workers.run(&task);
        completedAtReturn = completed.load();
        returned = true;
    });
    JoinGuard guard(coordinator);
    const bool allEntered = entered.Wait(3);
    releaseFirst.Add();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (completed.load() < 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool stillWaiting = !returned.load();
    releaseOthers.Add();
    const bool allFinished = finished.Wait(3);
    coordinator.join();
    // Record the actual product result before cleanup or any assertion. Keep
    // the original target predicates; the setup predicate is independent.
    std::fprintf(stderr, "WORKER_COMPLETION_TARGET entered=%u finished=%u stillWaiting=%u completedAtReturn=%u\n",
                 unsigned(allEntered), unsigned(allFinished), unsigned(stillWaiting), completedAtReturn);
    if (!stillWaiting || completedAtReturn != 3) {
        (void)fx.release(); // process exit reclaims a pool whose protocol failed
    }
    GC_EXPECT_TRUE(stillWaiting);
    GC_EXPECT_EQ(completedAtReturn, 3u);
    GC_EXPECT_TRUE(allEntered && allFinished);
    std::puts("OBSERVED coordinator blocked until the third worker completed");
}

// zWorkers.cpp:108-124: a restartable task is rerun with the requested
// worker count after resize_workers; workers observe the request lock-free.
GC_TEST(ZWorkers, RestartableTaskResizesOnRequest)
{
    for (auto counts : {std::make_pair(3u, 1u), std::make_pair(1u, 3u)}) {
        Fixture fx(ZGenerationId::young, 3);
        fx.workers.set_active_workers(counts.first);
        fx.workers.set_active();
        struct Restart : ZRestartableTask {
            ZWorkers& workers;
            Latch entered, release;
            Record before, after;
            std::atomic<unsigned> phase{0}, polled{0};
            unsigned applied = 0;
            explicit Restart(ZWorkers& w) : ZRestartableTask("ZWorkersUnitRestart"), workers(w) {}
            void work() override
            {
                if (phase == 0) {
                    before.Add();
                    entered.Add();
                    (void)release.Wait(1);
                    if (workers.should_worker_resize()) ++polled;
                } else {
                    after.Add();
                }
            }
            void resize_workers(uint32_t n) override { applied = n; ++phase; }
        } task(fx.workers);
        std::thread coordinator([&] { fx.workers.run(&task); });
        JoinGuard guard(coordinator);
        bool ready = task.entered.Wait(counts.first);
        fx.workers.request_resize_workers(counts.second);
        const bool pending = fx.workers.should_worker_resize();
        task.release.Add();
        coordinator.join();
        GC_EXPECT_TRUE(ready);
        GC_EXPECT_TRUE(pending);
        GC_EXPECT_EQ(task.polled.load(), counts.first);
        GC_EXPECT_EQ(task.applied, counts.second);
        task.before.Check(counts.first);
        task.after.Check(counts.second);
        GC_EXPECT_EQ(fx.workers.active_workers(), counts.second);
        GC_EXPECT_FALSE(fx.workers.should_worker_resize());
        std::printf("OBSERVED resize %u->%u callback=%u poll=%u\n", counts.first, counts.second,
                    task.applied, task.polled.load());
    }
}

GC_TEST(ZWorkers, RestartableTaskWithoutRequestRunsOnce)
{
    Fixture fx(ZGenerationId::old, 2);
    struct Restart : ZRestartableTask {
        Record result;
        unsigned callbacks = 0;
        Restart() : ZRestartableTask("ZWorkersUnitRestart") {}
        void work() override { result.Add(); }
        void resize_workers(uint32_t) override { ++callbacks; }
    } task;
    fx.workers.run(&task);
    task.result.Check(2);
    GC_EXPECT_EQ(task.callbacks, 0u);
    // zTask.cpp:48: the default resize_workers is empty and never required.
    struct Plain : ZRestartableTask {
        Plain() : ZRestartableTask("ZWorkersUnitPlain") {}
        void work() override {}
    } plain;
    fx.workers.run(&plain);
    std::puts("OBSERVED restartable task without request completes one batch");
}

// zWorkers.cpp:67-69,147-166: a pending request survives ordinary tasks and
// is cleared when the next cycle activates the workers.
GC_TEST(ZWorkers, PendingRequestSurvivesOrdinaryRunUntilNextCycle)
{
    Fixture fx(ZGenerationId::old, 3);
    fx.workers.set_active_workers(1);
    fx.workers.set_active();
    fx.workers.request_resize_workers(2);
    fx.workers.request_resize_workers(1); // already the active count: ignored
    Record result;
    Task task([&] { result.Add(); });
    fx.workers.run(&task);
    result.Check(1);
    GC_EXPECT_TRUE(fx.workers.should_worker_resize());
    GC_EXPECT_TRUE(fx.workers.is_active());
    fx.workers.set_inactive();
    GC_EXPECT_FALSE(fx.workers.is_active());
    fx.workers.set_active();
    GC_EXPECT_FALSE(fx.workers.should_worker_resize());
    std::puts("OBSERVED ordinary task retains request; next cycle clears it");
}

// workerThread.hpp:44-49: a WorkerTask captures the constructing thread's
// collection id (GCId::current_or_undefined) at construction, not at
// dispatch. GCIdMark's slot is an inline thread_local, so this check stays
// inside one image: the worker-side GCIdMark (workerThread.cpp:72) is only
// observable from product code.
GC_TEST(ZWorkers, WorkerTaskCapturesGcIdAtConstruction)
{
    struct Plain final : WorkerTask {
        Plain() : WorkerTask("ZWorkersUnitPlain") {}
        void work(uint32_t) override {}
    };
    Plain outside;
    GC_EXPECT_EQ(outside.gc_id(), GCIdMark::Current());
    uint64_t expected = 0;
    {
        GCIdMark mark;
        expected = GCIdMark::Current();
        Plain inside;
        GC_EXPECT_EQ(inside.gc_id(), expected);
        GC_EXPECT_NE(inside.gc_id(), outside.gc_id());
    }
    GC_EXPECT_NE(expected, 0u);
    std::puts("OBSERVED WorkerTask gc_id captured at construction");
}

// Invariant 3 through the product wiring: ZWorkers::run brackets the task
// with ZStatWorkers::at_start(active_workers)/at_end (zWorkers.cpp:92-106).
GC_TEST(ZWorkers, RunAccumulatesParallelTimeInStatWorkers)
{
    Fixture fx(ZGenerationId::old, 3);
    GC_EXPECT_EQ(fx.stats.stats()._accumulated_duration, 0.0);
    Latch entered, release;
    Task task([&] {
        entered.Add();
        (void)release.Wait(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    std::thread coordinator([&] { fx.workers.run(&task); });
    JoinGuard guard(coordinator);
    const bool allEntered = entered.Wait(3);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto inFlight = fx.stats.stats();
    release.Add();
    coordinator.join();
    std::fprintf(stderr, "WORKER_STATS_TARGET entered=%u inFlightDuration=%.9f inFlightTime=%.9f\n",
                 unsigned(allEntered), inFlight._accumulated_duration, inFlight._accumulated_time);
    const auto first = fx.stats.stats();
    std::fprintf(stderr, "WORKER_WEIGHTED_TIME_TARGET duration=%.9f time=%.9f workers=3\n",
                 first._accumulated_duration, first._accumulated_time);
    // The weighted-time invariant is the causal target, so a missing start
    // cannot be hidden by an earlier in-flight/setup assertion.
    GC_EXPECT_TRUE(std::fabs(first._accumulated_time - 3.0 * first._accumulated_duration) < 0.000001);
    GC_EXPECT_TRUE(first._accumulated_duration >= 0.010);
    GC_EXPECT_TRUE(inFlight._accumulated_duration > 0.0);
    GC_EXPECT_TRUE(std::fabs(inFlight._accumulated_time - 3.0 * inFlight._accumulated_duration) < 0.000001);
    GC_EXPECT_TRUE(allEntered);
    fx.workers.set_active_workers(1);
    fx.workers.run(&task);
    const auto second = fx.stats.stats();
    const double addedDuration = second._accumulated_duration - first._accumulated_duration;
    GC_EXPECT_TRUE(addedDuration >= 0.010);
    GC_EXPECT_TRUE(std::fabs((second._accumulated_time - first._accumulated_time) - addedDuration) < 0.000001);
    std::printf("OBSERVED worker stats duration=%.6f time=%.6f after 3x and 1x runs\n",
                second._accumulated_duration, second._accumulated_time);
}

// zGeneration.cpp:129: each generation constructs its workers over its own
// ZStatWorkers, and the driver reads that stat unit for the cycle.
GC_TEST(ZWorkers, ZGenerationOwnsWorkersAndStatWorkers)
{
    class Probe : public ZGeneration {
    public:
        using ZGeneration::ZGeneration;
        bool should_record_stats() override { return false; }
    };
    Probe young(ZGenerationId::young);
    Probe old(ZGenerationId::old);
    young.InitializeWorkers(2);
    old.InitializeWorkers(1);
    GC_EXPECT_TRUE(young.StatWorkers() != old.StatWorkers());
    Task task([] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); });
    young.Workers()->run(&task);
    GC_EXPECT_TRUE(young.StatWorkers()->stats()._accumulated_duration > 0.0);
    GC_EXPECT_EQ(old.StatWorkers()->stats()._accumulated_duration, 0.0);
    young.StopWorkers();
    old.StopWorkers();
    GC_EXPECT_TRUE(young.Workers() == nullptr);
}
