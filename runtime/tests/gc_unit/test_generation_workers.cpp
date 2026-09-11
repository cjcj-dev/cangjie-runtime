// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <set>
#include <linux/futex.h>
#include <sys/syscall.h>
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

namespace {
// These Linux process-isolated tests observe the real coordinator sleeping in
// the kernel. A deadline is only a harness error, never evidence of waiting.
// No product method, pthread operation, or condition variable is interposed.
[[noreturn]] void JoinHarnessError(const char* stage)
{
    std::fprintf(stderr, "JOIN_HARNESS_ERROR stage=%s\n", stage);
    std::_Exit(2);
}

void JoinSchedule()
{
    // In the isolated child, all subsequently created threads inherit one CPU
    // and one FIFO priority. Yield then lets an already runnable product caller
    // finish or block before we inspect it; a wakeup pending on another CPU
    // cannot be mistaken for a settled wait. This needs CAP_SYS_NICE, and is a
    // harness error when unavailable, never a skipped/passing product test.
    cpu_set_t allowed, single;
    CPU_ZERO(&allowed);
    CPU_ZERO(&single);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) JoinHarnessError("get-affinity");
    int cpu = 0;
    while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &allowed)) ++cpu;
    if (cpu == CPU_SETSIZE) JoinHarnessError("empty-affinity");
    CPU_SET(cpu, &single);
    sched_param parameter{};
    parameter.sched_priority = 1;
    if (sched_setaffinity(0, sizeof(single), &single) != 0 ||
        sched_setscheduler(0, SCHED_FIFO, &parameter) != 0) JoinHarnessError("fifo-schedule");
    std::fprintf(stderr, "JOIN_SCHEDULE cpu=%d policy=FIFO priority=1\n", cpu);
}

void JoinRequire(bool value, const char* invariant)
{
    std::fprintf(stderr, "JOIN_ASSERT %s value=%d\n", invariant, value);
    // A broken completion protocol may leave threads using this fixture.
    // Fail the isolated process without destructing an in-flight fixture.
    if (!value) std::_Exit(1);
}

template<class Predicate>
void JoinAwait(const char* stage, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        std::this_thread::yield();
        if (predicate()) return;
        if (std::chrono::steady_clock::now() >= deadline) JoinHarnessError(stage);
    }
}

struct FutexWait {
    unsigned long address = 0;
    unsigned long operation = 0;
    explicit operator bool() const { return address != 0; }
};

FutexWait SleepingFutex(pid_t tid)
{
    if (tid == 0) return {};
    char path[96];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", tid);
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) return {}; // The observed thread may already have exited.
    long number = -1;
    unsigned long address = 0, operation = 0;
    int fields = std::fscanf(file, "%ld %lx %lx", &number, &address, &operation);
    std::fclose(file);
    if (fields != 3 || number != SYS_futex) return {};
    const auto command = operation & FUTEX_CMD_MASK;
    if (command != FUTEX_WAIT && command != FUTEX_WAIT_BITSET) return {};
    return {address, operation};
}

struct CallState {
    std::atomic<pid_t> tid{0};
    std::atomic<bool> returned{false};
    void Enter() { tid = static_cast<pid_t>(syscall(SYS_gettid)); }
    FutexWait Observe(const char* stage)
    {
        FutexWait wait;
        JoinAwait(stage, [&] { wait = SleepingFutex(tid); return returned.load() || bool(wait); });
        std::fprintf(stderr, "JOIN_OBSERVED stage=%s returned=%d futex=%lx op=%lu\n",
                     stage, returned.load(), wait.address, wait.operation);
        return wait;
    }
};

struct ExitGate {
    std::atomic<unsigned> entered{0}, finished{0};
    Latch release;
    pthread_key_t key;
    ExitGate()
    {
        if (pthread_key_create(&key, [](void* data) {
            auto& gate = *static_cast<ExitGate*>(data);
            ++gate.entered;
            if (!gate.release.Wait(1)) JoinHarnessError("thread-exit-release");
            ++gate.finished;
        }) != 0) JoinHarnessError("pthread-key-create");
    }
    ~ExitGate() { pthread_key_delete(key); }
    void Install()
    {
        if (pthread_setspecific(key, this) != 0) JoinHarnessError("pthread-setspecific");
    }
};
}

GC_OTHER_VM_TEST(GenerationWorkers, BorrowedCompletionOrder)
{
    JoinSchedule();
    GCWorkers workers(Generation::OLD, 3);
    Latch entered, releaseOthers, releaseLast;
    std::atomic<unsigned> completed{0}, destroyed{0};
    CallState run;
    unsigned completedAtReturn = 0;
    struct Borrowed : GCWorkerTask {
        Latch &entered, &others, &last;
        std::atomic<unsigned> &completed, &destroyed;
        Borrowed(Latch& e, Latch& o, Latch& l, std::atomic<unsigned>& c, std::atomic<unsigned>& d)
            : entered(e), others(o), last(l), completed(c), destroyed(d) {}
        ~Borrowed() override { ++destroyed; }
        void Work(uint32_t id) override
        {
            entered.Add();
            if (!(id == 0 ? last : others).Wait(1)) JoinHarnessError("borrowed-release");
            ++completed;
        }
    };
    {
        Borrowed task(entered, releaseOthers, releaseLast, completed, destroyed);
        std::thread coordinator([&] {
            run.Enter();
            workers.Run(task);
            completedAtReturn = completed.load();
            run.returned = true;
        });
        if (!entered.Wait(3)) JoinHarnessError("borrowed-entered");
        auto initialWait = run.Observe("all-work-held");
        JoinRequire(!run.returned, "borrowed ownership retained while all Work calls are held");
        releaseOthers.Add();
        JoinAwait("last-worker-accounted", [&] {
            return run.returned || (completed == 2 && workers.GetSnapshot().remainingWorkers == 1);
        });
        // Read the product snapshot before the syscall: its lock acquisition
        // orders this observation after the other workers' completion signals.
        // Only accept the same condition-variable futex (glibc has two words),
        // not transient contention on a different mutex after early completion.
        JoinAwait("last-work-held", [&] {
            (void)workers.GetSnapshot();
            auto wait = SleepingFutex(run.tid);
            return run.returned || (wait && (wait.address == initialWait.address ||
                wait.address + 4 == initialWait.address || wait.address == initialWait.address + 4));
        });
        std::fprintf(stderr, "JOIN_OBSERVED last-work-held completed=%u remaining=%u returned=%d\n",
                     completed.load(), workers.GetSnapshot().remainingWorkers, run.returned.load());
        JoinRequire(!run.returned, "borrowed ownership retained until final Work completes");
        releaseLast.Add();
        coordinator.join();
        JoinRequire(completedAtReturn == 3, "Run result includes every participant completion");
        JoinRequire(destroyed == 0, "borrowed task still owned by caller");
    }
    JoinRequire(destroyed == 1, "caller destroys borrowed task exactly once");
}

GC_OTHER_VM_TEST(GenerationWorkers, StopCompletionOrder)
{
    JoinSchedule();
    ExitGate exit;
    GCWorkers workers(Generation::YOUNG, 2);
    Latch entered, release;
    std::atomic<unsigned> completed{0};
    Task task([&](uint32_t) {
        exit.Install();
        entered.Add();
        if (!release.Wait(1)) JoinHarnessError("stop-work-release");
        ++completed;
    });
    std::thread coordinator([&] { workers.Run(task); });
    if (!entered.Wait(2)) JoinHarnessError("stop-work-entered");
    CallState stop;
    unsigned exitsAtReturn = 0;
    std::thread stopper([&] {
        stop.Enter();
        workers.Stop();
        exitsAtReturn = exit.finished.load();
        stop.returned = true;
    });
    JoinAwait("closing-request", [&] { return workers.GetSnapshot().closing; });
    stop.Observe("stop-work-held");
    auto during = workers.GetSnapshot();
    JoinRequire(!stop.returned && !during.stopped && during.remainingWorkers == 2,
                "Stop retains ownership of accepted task");
    release.Add();
    coordinator.join();
    JoinAwait("thread-exit-entered", [&] { return exit.entered == 2; });
    stop.Observe("thread-exit-held");
    std::fprintf(stderr, "JOIN_OBSERVED exiting=%u exited=%u stopped=%d returned=%d\n",
                 exit.entered.load(), exit.finished.load(), workers.GetSnapshot().stopped, stop.returned.load());
    JoinRequire(!stop.returned && !workers.GetSnapshot().stopped,
                "Stop completion follows actual thread exit");
    exit.release.Add();
    stopper.join();
    JoinRequire(exitsAtReturn == 2 && completed == 2 && workers.GetSnapshot().stopped,
                "Stop result includes both worker exits and task results");
    workers.Stop();
    JoinRequire(exit.finished == 2, "repeated Stop preserves completed exit set");
}

GC_OTHER_VM_TEST(GenerationWorkers, StopRestartCompletionOrder)
{
    JoinSchedule();
    ExitGate exit;
    GCWorkers workers(Generation::OLD, 2);
    workers.SetActiveWorkers(1);
    workers.SetActive();
    struct Restart : GCRestartableWorkerTask {
        GCWorkers& workers;
        ExitGate& exit;
        Latch callback, release;
        std::atomic<unsigned> phase{0}, completed{0};
        Restart(GCWorkers& w, ExitGate& e) : workers(w), exit(e) {}
        void Work(uint32_t) override
        {
            exit.Install();
            if (phase == 0) workers.RequestResize(2); else ++completed;
        }
        void ResizeWorkers(uint32_t) override
        {
            callback.Add();
            if (!release.Wait(1)) JoinHarnessError("restart-callback-release");
            ++phase;
        }
    } task(workers, exit);
    std::thread coordinator([&] { workers.Run(task); });
    if (!task.callback.Wait(1)) JoinHarnessError("restart-callback-entered");
    CallState stop;
    std::thread stopper([&] { stop.Enter(); workers.Stop(); stop.returned = true; });
    JoinAwait("restart-closing", [&] { return workers.GetSnapshot().closing; });
    // glibc pthread_join waits on the thread ID with a shared futex. A private
    // futex here is the coordinator mutex: the callback holds it, no test lock
    // runs on the Stop thread, and the closing-state mutex is already released.
    FutexWait wait;
    JoinAwait("restart-stop-order", [&] {
        wait = SleepingFutex(stop.tid);
        return stop.returned || exit.entered != 0 || (wait && (wait.operation & FUTEX_PRIVATE_FLAG));
    });
    std::fprintf(stderr, "JOIN_OBSERVED callback-held exiting=%u returned=%d futex=%lx op=%lu\n",
                 exit.entered.load(), stop.returned.load(), wait.address, wait.operation);
    JoinRequire(exit.entered == 0 && !stop.returned,
                "Stop keeps workers alive until accepted restart completes");
    task.release.Add();
    coordinator.join();
    JoinRequire(task.completed == 2, "accepted restarted batch completes during closing");
    JoinAwait("restart-worker-exits", [&] { return exit.entered == 2; });
    exit.release.Add();
    stopper.join();
    JoinRequire(exit.finished == 2 && workers.GetSnapshot().stopped, "restart stop closes both worker lifetimes");
}

GC_OTHER_VM_TEST(GenerationWorkers, OrdinaryRunStopControl)
{
    // Keep the set's storage until this isolated process exits, including in a
    // control arm whose deliberately omitted join leaves a worker still exiting.
    auto& workers = *new GCWorkers(Generation::YOUNG, 1);
    std::atomic<unsigned> result{0};
    Task task([&](uint32_t) { ++result; });
    workers.Run(task);
    workers.Stop();
    JoinRequire(result == 1, "ordinary task result survives completion control");
}

int main(int argc, char** argv)
{
    constexpr const char* prefix = "--gtest_filter=";
    if (argc == 2 && std::strncmp(argv[1], prefix, std::strlen(prefix)) == 0) {
        setenv("GC_UNIT_FILTER", argv[1] + std::strlen(prefix), 1);
    } else if (argc != 1) {
        return 2;
    }
    Dl_info info{};
    void* symbol = dlsym(RTLD_DEFAULT, "_ZN12MapleRuntime9GCWorkers4StopEv");
    if (symbol == nullptr || dladdr(symbol, &info) == 0) return 2;
    std::printf("PRODUCT_LOADED=%s\n", info.dli_fname);
    return RunAll();
}
