// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zWorkers.hpp"

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
        WorkerTask* task = currentTask;
        // workerThread.cpp:68-73: thread-local worker id and the task's gc id.
        WorkerThread::set_worker_id(id);
        GCIdMark gcId(task->gc_id());
        lock.unlock();
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanAttachNativeThread();
#endif
        task->work(id);
        // ZMarkTask::work: publish both generations before reporting completion.
        ThreadLocal::FlushCurrentThreadMarkStacks();
        lock.lock();
        if (--remainingWorkers == 0) {
            completed.notify_one();
        }
    }
}

void RuntimeWorkers::Run(ZTask& task)
{
    ThreadLocal::FlushCurrentThreadMarkStacks();
    std::lock_guard<std::mutex> coordinator(coordinatorMutex);
    std::unique_lock<std::mutex> lock(mutex);
    currentTask = task.worker_task();
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
}
