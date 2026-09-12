// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/GcThreadPool.h"

#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
#include <sys/resource.h>
#endif
#include <sched.h>
#include <chrono>

#include "Base/Log.h"
#include "Base/Panic.h"
#include "Base/SysCall.h"
#include "Mutator/MutatorManager.h"
#include "securec.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
#if defined(__linux__) || defined(hongmeng)
void RuntimeWorkers::SetThreadPriority(pid_t tid, int32_t priority)
{
    errno = 0;
    int ret = ::setpriority(static_cast<int>(PRIO_PROCESS), static_cast<uint32_t>(tid), priority);
    // strerror_r does not work as expected: outputs nothing for __amd64__
    CHECK_E(UNLIKELY(ret != 0 && errno != 0), "::setpriority(tid %d, priority %d) failed: %s", tid, priority,
            ::strerror(errno));
}
#endif

GCWorkers::GCWorkers(Generation gen, uint32_t count, int32_t prior)
    : generation(gen), capacity(count), priority(prior), activeWorkers(count)
{
    CheckCount(count);
    threads.resize(capacity);
    pthread_attr_t attr;
    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "GCWorkers");
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, 512 * 1024), "GCWorkers");
    for (uint32_t id = 0; id < capacity; ++id) {
        Worker& worker = threads[id];
        worker.owner = this;
        worker.id = id;
        CHECK_PTHREAD_CALL(pthread_create, (&worker.thread, &attr, WorkerEntry, &worker), "GCWorkers");
#ifdef __WIN64
        CHECK_PTHREAD_CALL(pthread_setname_np,
            (worker.thread, generation == Generation::YOUNG ? "GCWorkerYoung" : "GCWorkerOld"), "GCWorkers");
#endif
    }
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "GCWorkers");
}

GCWorkers::~GCWorkers() { Stop(); }

void GCWorkers::CheckCount(uint32_t workers) const
{
    CHECK_DETAIL(workers > 0 && workers <= capacity, "GCWorkers count outside [1, capacity]");
}

void GCWorkers::CheckOpen() const
{
    CHECK_DETAIL(!closing, "GCWorkers is closing");
}

void* GCWorkers::WorkerEntry(void* argument)
{
    Worker& worker = *static_cast<Worker*>(argument);
    GCWorkers& owner = *worker.owner;
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
    const char* name = owner.generation == Generation::YOUNG ? "GCWorkerYoung" : "GCWorkerOld";
#ifdef __APPLE__
    CHECK_PTHREAD_CALL(pthread_setname_np, (name), "GCWorkers");
#elif defined(__linux__) || defined(hongmeng)
    CHECK_PTHREAD_CALL(prctl, (PR_SET_NAME, name), "GCWorkers");
    RuntimeWorkers::SetThreadPriority(MapleRuntime::GetTid(), owner.priority);
#else
    (void)name;
#endif
    owner.WorkerLoop(worker.id);
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanDetachNativeThread();
#endif
    return nullptr;
}

void GCWorkers::WorkerLoop(uint32_t id)
{
    uint64_t observedBatch = 0;
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        dispatched.wait(lock, [&] { return shutdown || batch != observedBatch; });
        if (shutdown) {
            return;
        }
        observedBatch = batch;
        if (id >= runningWorkers) {
            continue;
        }
        GCWorkerTask* task = currentTask;
        lock.unlock();
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanAttachNativeThread();
        TsanPosCtrlMaybeRace(id);
#endif
        task->Work(id);
        lock.lock();
        --remainingWorkers;
        if (remainingWorkers == 0) {
            completed.notify_all();
        }
    }
}

// coordinatorMutex protects the complete run, including resize callbacks.
// mutex is released while waiting, so worker polls and external requests work.
void GCWorkers::RunBatch(GCWorkerTask& task)
{
    std::unique_lock<std::mutex> lock(mutex);
    currentTask = &task;
    runningWorkers = activeWorkers;
    remainingWorkers = runningWorkers;
    ++batch;
    const auto start = std::chrono::steady_clock::now();
    dispatched.notify_all();
    completed.wait(lock, [&] { return remainingWorkers == 0; });
    const uint64_t duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    elapsedNanos += duration;
    workerNanos += duration * runningWorkers;
    ++completedBatches;
    runningWorkers = 0;
    currentTask = nullptr;
}

void GCWorkers::Run(GCWorkerTask& task)
{
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    {
        std::lock_guard<std::mutex> lock(mutex);
        CheckOpen();
    }
    RunBatch(task);
}

void GCWorkers::Run(GCRestartableWorkerTask& task)
{
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    {
        std::lock_guard<std::mutex> lock(mutex);
        CheckOpen();
    }
    for (;;) {
        RunBatch(task);
        uint32_t applied;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (requestedWorkers == 0) {
                return;
            }
            activeWorkers = requestedWorkers;
            requestedWorkers = 0;
            applied = activeWorkers;
        }
        task.ResizeWorkers(applied);
    }
}

void GCWorkers::RunAll(GCWorkerTask& task)
{
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    uint32_t previous;
    {
        std::lock_guard<std::mutex> lock(mutex);
        CheckOpen();
        previous = activeWorkers;
        activeWorkers = capacity;
    }
    RunBatch(task);
    std::lock_guard<std::mutex> lock(mutex);
    activeWorkers = previous;
}

void GCWorkers::SetActiveWorkers(uint32_t workers)
{
    CheckCount(workers);
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    std::lock_guard<std::mutex> lock(mutex);
    CheckOpen();
    activeWorkers = workers;
}

void GCWorkers::SetActive()
{
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    std::lock_guard<std::mutex> lock(mutex);
    CheckOpen();
    cycleActive = true;
    requestedWorkers = 0;
}

void GCWorkers::SetInactive()
{
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    std::lock_guard<std::mutex> lock(mutex);
    cycleActive = false;
}

uint32_t GCWorkers::ActiveWorkers() const { return GetSnapshot().activeWorkers; }
bool GCWorkers::IsActive() const { return GetSnapshot().cycleActive; }

void GCWorkers::RequestResize(uint32_t workers)
{
    CheckCount(workers);
    std::lock_guard<std::mutex> lock(mutex);
    if (!closing && workers != activeWorkers && workers != requestedWorkers) {
        requestedWorkers = workers;
    }
}

bool GCWorkers::ShouldWorkerResize() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return requestedWorkers != 0;
}

GCWorkers::Snapshot GCWorkers::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return { generation, capacity, activeWorkers, runningWorkers, remainingWorkers, requestedWorkers,
             cycleActive, closing, stopped, batch, completedBatches, elapsedNanos, workerNanos };
}

void GCWorkers::ThreadsDo(const std::function<void(pthread_t)>& visitor)
{
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    if (!stopped) {
        for (const Worker& worker : threads) {
            visitor(worker.thread);
        }
    }
}

void GCWorkers::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        closing = true;
    }
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopped) {
            return;
        }
        shutdown = true;
        cycleActive = false;
        requestedWorkers = 0;
        dispatched.notify_all();
    }
    for (Worker& worker : threads) {
        CHECK_PTHREAD_CALL(pthread_join, (worker.thread, nullptr), "GCWorkers");
    }
    std::lock_guard<std::mutex> lock(mutex);
    stopped = true;
}

// zRuntimeWorkers.cpp:29-48; workerThread.cpp:41-80, run_task joins
// the borrowed task before returning to the safepoint coordinator.
RuntimeWorkers::RuntimeWorkers(uint32_t count)
{
    CHECK_DETAIL(count > 0, "RuntimeWorkers requires at least one worker");
    threads.resize(count);
    pthread_attr_t attr;
    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "RuntimeWorkers");
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, 512 * 1024), "RuntimeWorkers");
    for (uint32_t id = 0; id < count; ++id) {
        Worker& worker = threads[id];
        worker.owner = this;
        worker.id = id;
        CHECK_PTHREAD_CALL(pthread_create, (&worker.thread, &attr, WorkerEntry, &worker), "RuntimeWorkers");
#ifdef __WIN64
        CHECK_PTHREAD_CALL(pthread_setname_np, (worker.thread, "RuntimeWorker"), "RuntimeWorkers");
#endif
    }
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "RuntimeWorkers");
}

RuntimeWorkers::~RuntimeWorkers()
{
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    {
        std::lock_guard<std::mutex> lock(mutex);
        shutdown = true;
        dispatched.notify_all();
    }
    for (Worker& worker : threads) {
        CHECK_PTHREAD_CALL(pthread_join, (worker.thread, nullptr), "RuntimeWorkers");
    }
}

void* RuntimeWorkers::WorkerEntry(void* argument)
{
    Worker& worker = *static_cast<Worker*>(argument);
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
#ifdef __APPLE__
    CHECK_PTHREAD_CALL(pthread_setname_np, ("RuntimeWorker"), "RuntimeWorkers");
#elif defined(__linux__) || defined(hongmeng)
    CHECK_PTHREAD_CALL(prctl, (PR_SET_NAME, "RuntimeWorker"), "RuntimeWorkers");
    SetThreadPriority(MapleRuntime::GetTid());
#endif
    worker.owner->WorkerLoop(worker.id);
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanDetachNativeThread();
#endif
    return nullptr;
}

void RuntimeWorkers::WorkerLoop(uint32_t id)
{
    uint64_t observedBatch = 0;
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        dispatched.wait(lock, [&] { return shutdown || batch != observedBatch; });
        if (shutdown) {
            return;
        }
        observedBatch = batch;
        GCWorkerTask* task = currentTask;
        lock.unlock();
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanAttachNativeThread();
#endif
        task->Work(id);
        lock.lock();
        if (--remainingWorkers == 0) {
            completed.notify_one();
        }
    }
}

void RuntimeWorkers::Run(GCWorkerTask& task)
{
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    std::unique_lock<std::mutex> lock(mutex);
    currentTask = &task;
    remainingWorkers = ActiveWorkers();
    ++batch;
    dispatched.notify_all();
    completed.wait(lock, [&] { return remainingWorkers == 0; });
    currentTask = nullptr;
}

void RuntimeWorkers::ThreadsDo(const std::function<void(pthread_t)>& visitor)
{
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    for (const Worker& worker : threads) {
        visitor(worker.thread);
    }
}
} // namespace MapleRuntime
