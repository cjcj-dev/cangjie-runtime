// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/workerThread.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#if defined(__linux__) || defined(hongmeng)
#include <sys/prctl.h>
#endif

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/Panic.h"
#include "Mutator/ThreadLocal.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
// gc/shared/workerThread.cpp:34-39
WorkerTaskDispatcher::WorkerTaskDispatcher()
    : _task(nullptr), _started(0), _not_finished(0), _start_semaphore(), _end_semaphore() {}

// gc/shared/workerThread.cpp:41-61
void WorkerTaskDispatcher::coordinator_distribute_task(WorkerTask* task, uint32_t num_workers)
{
    CHECK_DETAIL(num_workers > 0, "must use at least one worker, deadlocks otherwise");

    // No workers are allowed to read the state variables until they have been signaled.
    _task = task;
    _not_finished.store(num_workers, std::memory_order_relaxed);

    // Dispatch 'num_workers' number of tasks.
    _start_semaphore.signal(num_workers);

    // Wait for the last worker to signal the coordinator.
    _end_semaphore.wait();

    // No workers are allowed to read the state variables after the coordinator has been signaled.
    DCHECK(_not_finished.load(std::memory_order_relaxed) == 0);
    _task = nullptr;
    _started.store(0, std::memory_order_relaxed);
}

// gc/shared/workerThread.cpp:63-83
void WorkerTaskDispatcher::worker_run_task()
{
    // Wait for the coordinator to dispatch a task.
    _start_semaphore.wait();

    // Get and set worker id.
    const uint32_t worker_id = _started.fetch_add(1u, std::memory_order_relaxed);
    WorkerThread::set_worker_id(worker_id);

    // Run task.
    {
        GCIdMark gc_id_mark(_task->gc_id());
        _task->work(worker_id);
    }

    // Mark that the worker is done with the task.
    // The worker is not allowed to read the state variables after this line.
    const uint32_t not_finished = _not_finished.fetch_sub(1u, std::memory_order_acq_rel) - 1u;

    // The last worker signals to the coordinator that all work is completed.
    if (not_finished == 0) {
        _end_semaphore.signal();
    }
}

// gc/shared/workerThread.cpp:85-91
WorkerThreads::WorkerThreads(const char* name, uint32_t max_workers)
    : _name(name), _workers(new WorkerThread*[max_workers]()), _max_workers(max_workers), _created_workers(0),
      _active_workers(0), _dispatcher() {}

// Infrastructure difference (see ~WorkerThreads): the only task the pool
// itself ever dispatches. It carries no GC semantics.
class WorkerThreadExitTask final : public WorkerTask {
public:
    WorkerThreadExitTask() : WorkerTask("WorkerThreadExitTask") {}
    void work(uint32_t) override { WorkerThread::_exit_requested = true; }
};

WorkerThreads::~WorkerThreads()
{
    stop();
    delete[] _workers;
}

void WorkerThreads::stop()
{
    std::lock_guard<std::mutex> guard(_stop_lock);
    const uint32_t created = created_workers();
    if (created != 0) {
        WorkerThreadExitTask task;
        _dispatcher.coordinator_distribute_task(&task, created);
        for (uint32_t i = 0; i < created; ++i) {
            CHECK_PTHREAD_CALL(pthread_join, (_workers[i]->os_thread(), nullptr), "WorkerThreads");
            delete _workers[i];
            _workers[i] = nullptr;
        }
    }
    _active_workers = 0;
    _created_workers.store(0, std::memory_order_release);
}

// gc/shared/workerThread.cpp:93-98. No UseDynamicNumberOfGCThreads flag
// (I15): every worker set is static.
void WorkerThreads::initialize_workers()
{
    const uint32_t initial_active_workers = _max_workers;
    if (set_active_workers(initial_active_workers) != initial_active_workers) {
        CHECK_DETAIL(false, "Failed to create %s worker threads", _name);
    }
}

// gc/shared/workerThread.cpp:114-131. os::create_thread(worker, os::gc_thread)
// + os::start_thread(worker) collapse into one pthread_create here.
WorkerThread* WorkerThreads::create_worker(uint32_t name_suffix)
{
    WorkerThread* const worker = new WorkerThread(_name, name_suffix, &_dispatcher);

    pthread_attr_t attr;
    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "WorkerThreads");
    // os::gc_thread selects VMThreadStackSize (os_posix.cpp:1233-1240).
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, 1024 * 1024), "WorkerThreads");
    const int ret = ::pthread_create(&worker->_thread, &attr, WorkerThread::entry, worker);
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "WorkerThreads");
    if (ret != 0) {
        delete worker;
        return nullptr;
    }

    on_create_worker(worker);

    return worker;
}

// gc/shared/workerThread.cpp:133-156
uint32_t WorkerThreads::set_active_workers(uint32_t num_workers)
{
    CHECK_DETAIL(num_workers > 0 && num_workers <= _max_workers,
                 "Invalid number of active workers %u (should be 1-%u)", num_workers, _max_workers);

    uint32_t local_created_workers = created_workers();
    while (local_created_workers < num_workers) {
        WorkerThread* const worker = create_worker(local_created_workers);
        if (worker == nullptr) {
            LOG(RTLOG_ERROR, "Failed to create worker thread");
            break;
        }

        _workers[local_created_workers] = worker;
        local_created_workers++;
        _created_workers.store(local_created_workers, std::memory_order_release);
    }

    _active_workers = std::min(local_created_workers, num_workers);

    VLOG(REPORT, "%s: using %u out of %u workers", _name, _active_workers, _max_workers);

    return _active_workers;
}

// gc/shared/workerThread.cpp:158-163
void WorkerThreads::threads_do(const std::function<void(WorkerThread*)>& tc) const
{
    const uint32_t local_created_workers = created_workers();
    for (uint32_t i = 0; i < local_created_workers; i++) {
        tc(_workers[i]);
    }
}

// gc/shared/workerThread.cpp:200-209. set_indirect_states/clear_indirect_states
// are ASSERT-only HotSpot Thread flags (no Thread object here, I17).
void WorkerThreads::run_task(WorkerTask* task)
{
    _dispatcher.coordinator_distribute_task(task, _active_workers);
}

void WorkerThreads::run_task(WorkerTask* task, uint32_t num_workers)
{
    WithActiveWorkers with_active_workers(this, num_workers);
    run_task(task);
}

// gc/shared/workerThread.cpp:211-222
thread_local uint32_t WorkerThread::_worker_id = UINT32_MAX;
thread_local bool WorkerThread::_exit_requested = false;

WorkerThread::WorkerThread(const char* name_prefix, uint32_t name_suffix, WorkerTaskDispatcher* dispatcher)
    : _dispatcher(dispatcher), _thread()
{
    (void)std::snprintf(_name, sizeof(_name), "%s#%u", name_prefix, name_suffix);
}

// Thread::call_run for a NamedThread (thread.cpp): register the OS thread
// with this runtime's thread-local data (I17: GC threads are bare pthreads)
// and name it, then enter run().
void* WorkerThread::entry(void* arg)
{
    WorkerThread* const worker = static_cast<WorkerThread*>(arg);
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
#ifdef __APPLE__
    CHECK_PTHREAD_CALL(pthread_setname_np, (worker->_name), "WorkerThread");
#elif defined(__linux__) || defined(hongmeng)
    CHECK_PTHREAD_CALL(prctl, (PR_SET_NAME, worker->_name), "WorkerThread");
#endif
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanAttachNativeThread();
#endif
    worker->run();
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanDetachNativeThread();
#endif
    return nullptr;
}

void WorkerThread::run()
{
    while (true) {
        _dispatcher->worker_run_task();
        if (_exit_requested) {
            return;
        }
    }
}
} // namespace MapleRuntime
