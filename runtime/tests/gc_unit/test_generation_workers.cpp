// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <set>
#include "Heap/GcThreadPool.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
using Generation = GCWorkers::Generation;

namespace {
class Task : public GCWorkerTask {
public:
    explicit Task(std::function<void(uint32_t)> fn) : fn(std::move(fn)) {}
    void Work(uint32_t id) override { fn(id); }
private:
    std::function<void(uint32_t)> fn;
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
    void Add(uint32_t id)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ids.push_back(id);
        threads.insert(pthread_self());
    }
    void Check(unsigned n)
    {
        std::sort(ids.begin(), ids.end());
        GC_EXPECT_EQ(ids.size(), n);
        GC_EXPECT_EQ(threads.size(), n);
        for (unsigned i = 0; i < n; ++i) GC_EXPECT_EQ(ids[i], i);
    }
};
}

GC_TEST(GenerationWorkers, IndependentSets)
{
    GCWorkers young(Generation::YOUNG, 3), old(Generation::OLD, 2);
    young.SetActiveWorkers(1);
    young.SetActive();
    old.SetActive();
    Record yr, orr;
    Latch entered, release;
    Task ot([&](uint32_t id) { orr.Add(id); entered.Add(); (void)release.Wait(1); });
    std::thread coordinator([&] { old.Run(ot); });
    bool ready = entered.Wait(2);
    Task yt([&](uint32_t id) { yr.Add(id); });
    young.Run(yt);
    auto ys = young.GetSnapshot();
    auto os = old.GetSnapshot();
    release.Add();
    coordinator.join();
    GC_EXPECT_TRUE(ready);
    yr.Check(1);
    orr.Check(2);
    GC_EXPECT_EQ(ys.generation, Generation::YOUNG);
    GC_EXPECT_EQ(os.generation, Generation::OLD);
    GC_EXPECT_EQ(ys.remainingWorkers, 0u);
    GC_EXPECT_EQ(os.remainingWorkers, 2u);
    GC_EXPECT_EQ(os.runningWorkers, 2u);
    GC_EXPECT_EQ(ys.completedBatches, 1u);
    GC_EXPECT_EQ(os.completedBatches, 0u);
    for (auto t : yr.threads) GC_EXPECT_EQ(orr.threads.count(t), 0u);
    std::puts("OBSERVED independent generations and completed/remaining batches");
}

GC_TEST(GenerationWorkers, CountsAndRunAll)
{
    GCWorkers workers(Generation::YOUNG, 4);
    workers.SetActiveWorkers(1);
    for (unsigned count : {1u, 4u, 1u}) {
        Record result;
        Task task([&](uint32_t id) { result.Add(id); });
        if (count == 4) workers.RunAll(task); else workers.Run(task);
        result.Check(count);
        GC_EXPECT_EQ(workers.ActiveWorkers(), 1u);
        GC_EXPECT_EQ(workers.GetSnapshot().runningWorkers, 0u);
    }
    GC_EXPECT_EQ(workers.GetSnapshot().completedBatches, 3u);
    std::puts("OBSERVED IDs for single/full/restored batches");
}

GC_TEST(GenerationWorkers, BorrowedTaskJoin)
{
    GCWorkers workers(Generation::OLD, 3);
    std::atomic<unsigned> completed{0}, destroyed{0};
    struct Borrowed : GCWorkerTask {
        std::atomic<unsigned>& completed;
        std::atomic<unsigned>& destroyed;
        Borrowed(std::atomic<unsigned>& c, std::atomic<unsigned>& d) : completed(c), destroyed(d) {}
        ~Borrowed() override { ++destroyed; }
        void Work(uint32_t) override { ++completed; }
    };
    {
        Borrowed task(completed, destroyed);
        workers.Run(task);
        GC_EXPECT_EQ(completed.load(), 3u);
        GC_EXPECT_EQ(destroyed.load(), 0u);
    }
    GC_EXPECT_EQ(destroyed.load(), 1u);
    std::puts("OBSERVED borrowed task completed before caller destruction");
}

GC_TEST(GenerationWorkers, ResizeRestart)
{
    for (auto counts : {std::make_pair(3u, 1u), std::make_pair(1u, 3u)}) {
        GCWorkers workers(Generation::YOUNG, 3);
        workers.SetActiveWorkers(counts.first);
        workers.SetActive();
        struct Restart : GCRestartableWorkerTask {
            GCWorkers& workers;
            Latch entered, release;
            Record before, after;
            std::atomic<unsigned> phase{0}, polled{0};
            unsigned applied = 0;
            explicit Restart(GCWorkers& w) : workers(w) {}
            void Work(uint32_t id) override
            {
                if (phase == 0) {
                    before.Add(id);
                    entered.Add();
                    (void)release.Wait(1);
                    if (workers.ShouldWorkerResize()) ++polled;
                } else {
                    after.Add(id);
                }
            }
            void ResizeWorkers(uint32_t n) override { applied = n; ++phase; }
        } task(workers);
        std::thread coordinator([&] { workers.Run(task); });
        bool ready = task.entered.Wait(counts.first);
        workers.RequestResize(counts.second);
        auto pending = workers.GetSnapshot();
        task.release.Add();
        coordinator.join();
        GC_EXPECT_TRUE(ready);
        GC_EXPECT_EQ(pending.activeWorkers, counts.first);
        GC_EXPECT_EQ(pending.requestedWorkers, counts.second);
        GC_EXPECT_EQ(task.polled.load(), counts.first);
        GC_EXPECT_EQ(task.applied, counts.second);
        task.before.Check(counts.first);
        task.after.Check(counts.second);
        GC_EXPECT_EQ(workers.ActiveWorkers(), counts.second);
        GC_EXPECT_EQ(workers.GetSnapshot().completedBatches, 2u);
        std::printf("OBSERVED resize %u->%u callback=%u poll=%u\n", counts.first, counts.second,
                    task.applied, task.polled.load());
    }
}

GC_TEST(GenerationWorkers, RestartWithoutRequest)
{
    GCWorkers workers(Generation::OLD, 2);
    struct Restart : GCRestartableWorkerTask {
        Record result;
        unsigned callbacks = 0;
        void Work(uint32_t id) override { result.Add(id); }
        void ResizeWorkers(uint32_t) override { ++callbacks; }
    } task;
    workers.Run(task);
    task.result.Check(2);
    GC_EXPECT_EQ(task.callbacks, 0u);
    GC_EXPECT_EQ(workers.GetSnapshot().completedBatches, 1u);
    std::puts("OBSERVED restartable task without request completes one batch");
}

GC_TEST(GenerationWorkers, PendingAndCycle)
{
    GCWorkers workers(Generation::OLD, 3);
    workers.SetActiveWorkers(1);
    workers.SetActive();
    workers.RequestResize(2);
    workers.RequestResize(1); // ZWorkers preserves an already pending request.
    Record result;
    Task task([&](uint32_t id) { result.Add(id); });
    workers.Run(task);
    result.Check(1);
    GC_EXPECT_EQ(workers.GetSnapshot().requestedWorkers, 2u);
    GC_EXPECT_TRUE(workers.IsActive());
    workers.SetInactive();
    GC_EXPECT_FALSE(workers.IsActive());
    workers.SetActive();
    GC_EXPECT_FALSE(workers.ShouldWorkerResize());
    std::puts("OBSERVED ordinary task retains request; next cycle clears it");
}

GC_TEST(GenerationWorkers, StopJoin)
{
    GCWorkers workers(Generation::YOUNG, 2);
    std::set<pthread_t> enumerated;
    workers.ThreadsDo([&](pthread_t t) { enumerated.insert(t); });
    Latch entered, release;
    Record result;
    Task task([&](uint32_t id) { entered.Add(); (void)release.Wait(1); result.Add(id); });
    std::thread coordinator([&] { workers.Run(task); });
    bool ready = entered.Wait(2);
    std::thread stopper([&] { workers.Stop(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!workers.GetSnapshot().closing && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    auto during = workers.GetSnapshot();
    std::thread secondStopper([&] { workers.Stop(); });
    release.Add();
    coordinator.join();
    stopper.join();
    secondStopper.join();
    workers.Stop();
    unsigned after = 0;
    workers.ThreadsDo([&](pthread_t) { ++after; });
    GC_EXPECT_TRUE(ready);
    GC_EXPECT_TRUE(during.closing);
    GC_EXPECT_FALSE(during.stopped);
    GC_EXPECT_EQ(during.remainingWorkers, 2u);
    GC_EXPECT_TRUE(workers.GetSnapshot().stopped);
    GC_EXPECT_EQ(after, 0u);
    result.Check(2);
    GC_EXPECT_TRUE(result.threads == enumerated);
    GCWorkers idle(Generation::OLD, 1);
    idle.Stop();
    idle.Stop();
    GC_EXPECT_TRUE(idle.GetSnapshot().stopped);
    std::puts("OBSERVED idle and concurrent stop, task completion, joined thread set");
}

GC_TEST(GenerationWorkers, BatchStats)
{
    GCWorkers workers(Generation::OLD, 3);
    workers.SetActiveWorkers(1);
    Task task([](uint32_t) {});
    workers.Run(task);
    auto single = workers.GetSnapshot();
    workers.RunAll(task);
    auto all = workers.GetSnapshot();
    GC_EXPECT_TRUE(single.elapsedNanos > 0);
    GC_EXPECT_EQ(single.workerNanos, single.elapsedNanos);
    GC_EXPECT_TRUE(all.elapsedNanos > single.elapsedNanos);
    GC_EXPECT_EQ(all.workerNanos - single.workerNanos, 3 * (all.elapsedNanos - single.elapsedNanos));
    GC_EXPECT_EQ(all.batch, 2u);
    GC_EXPECT_EQ(all.completedBatches, 2u);
    std::printf("OBSERVED batch stats single=%llu all=%llu weighted=%llu\n",
        (unsigned long long)single.elapsedNanos, (unsigned long long)all.elapsedNanos,
        (unsigned long long)all.workerNanos);
}

int main()
{
    Dl_info info{};
    void* symbol = dlsym(RTLD_DEFAULT, "_ZN12MapleRuntime9GCWorkers4StopEv");
    if (symbol == nullptr || dladdr(symbol, &info) == 0) return 2;
    std::printf("PRODUCT_LOADED=%s\n", info.dli_fname);
    return RunAll();
}
