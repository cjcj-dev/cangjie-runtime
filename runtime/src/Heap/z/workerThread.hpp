// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_SHARED_WORKERTHREAD_HPP
#define MRT_GC_SHARED_WORKERTHREAD_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <pthread.h>

#include "Base/Semaphore.h"
#include "Heap/z/zGCIdPrinter.hpp"

namespace MapleRuntime {
class WorkerTaskDispatcher;
class WorkerThread;
class WorkerThreadExitTask;

// gc/shared/workerThread.hpp:39-56. A task to be worked on by worker threads.
// The collection id is captured where the task is constructed.
class WorkerTask {
private:
    const char* _name;
    const uint64_t _gc_id;

public:
    explicit WorkerTask(const char* name) : _name(name), _gc_id(GCIdMark::Current()) {}
    virtual ~WorkerTask() = default;

    const char* name() const { return _name; }
    uint64_t gc_id() const { return _gc_id; }

    virtual void work(uint32_t worker_id) = 0;
};

// gc/shared/workerThread.hpp:58-83. WorkerThreads dispatcher implemented with semaphores.
class WorkerTaskDispatcher {
    // The task currently being dispatched to the WorkerThreads.
    WorkerTask* _task;

    std::atomic<uint32_t> _started;
    std::atomic<uint32_t> _not_finished;

    // Semaphore used to start the WorkerThreads.
    Semaphore _start_semaphore;
    // Semaphore used to notify the coordinator that all workers are done.
    Semaphore _end_semaphore;

public:
    WorkerTaskDispatcher();

    // Coordinator API.

    // Distributes the task out to num_workers workers.
    // Returns when the task has been completed by all workers.
    void coordinator_distribute_task(WorkerTask* task, uint32_t num_workers);

    // Worker API.

    // Waits for a task to become available to the worker and runs it.
    void worker_run_task();
};

// gc/shared/workerThread.hpp:85-129. A set of worker threads to execute tasks.
class WorkerThreads {
private:
    const char* const _name;
    WorkerThread** _workers;
    const uint32_t _max_workers;
    // _created_workers publishes the initialized prefix of _workers.
    // Writers release-store to it after initializing an entry. Readers
    // load-acquire before accessing _workers to not access uninitalized
    // data.
    std::atomic<uint32_t> _created_workers;
    uint32_t _active_workers;
    WorkerTaskDispatcher _dispatcher;
    std::mutex _stop_lock;

    WorkerThread* create_worker(uint32_t name_suffix);

protected:
    virtual void on_create_worker(WorkerThread* worker) { (void)worker; }

public:
    WorkerThreads(const char* name, uint32_t max_workers);
    // Infrastructure difference: HotSpot worker pools live until VM exit.
    // Native runtime teardown and in-process fixtures must join their pthreads;
    // stop is also called explicitly for owners with process-lifetime storage.
    virtual ~WorkerThreads();
    WorkerThreads(const WorkerThreads&) = delete;
    WorkerThreads& operator=(const WorkerThreads&) = delete;

    void initialize_workers();
    // Like task dispatch, initialization/restart and stop require the owner
    // to have quiesced task coordinators. Concurrent stop calls are serialized.
    // After return initialize_workers() may recreate workers in this pool.
    void stop();

    uint32_t max_workers() const { return _max_workers; }
    uint32_t created_workers() const { return _created_workers.load(std::memory_order_acquire); }
    uint32_t active_workers() const { return _active_workers; }

    uint32_t set_active_workers(uint32_t num_workers);

    void threads_do(const std::function<void(WorkerThread*)>& tc) const;

    const char* name() const { return _name; }

    // Run a task using the current active number of workers, returns when the task is done.
    void run_task(WorkerTask* task);

    // Run a task with the given number of workers, returns when the task is done.
    void run_task(WorkerTask* task, uint32_t num_workers);
};

// gc/shared/workerThread.hpp:131-152. The worker id is thread local; tasks
// read it through worker_id() instead of receiving it as an argument.
class WorkerThread {
    friend class WorkerTaskDispatcher;
    friend class WorkerThreads;
    friend class WorkerThreadExitTask;

private:
    static thread_local uint32_t _worker_id;
    // Infrastructure difference (see ~WorkerThreads): the exit request that
    // the destructor dispatches is observed by the running thread here.
    static thread_local bool _exit_requested;

    WorkerTaskDispatcher* const _dispatcher;
    pthread_t _thread;
    char _name[32];

    static void set_worker_id(uint32_t worker_id) { _worker_id = worker_id; }
    static void* entry(void* arg);

public:
    static uint32_t worker_id() { return _worker_id; }

    WorkerThread(const char* name_prefix, uint32_t which, WorkerTaskDispatcher* dispatcher);

    const char* name() const { return _name; }
    pthread_t os_thread() const { return _thread; }

    void run();
};

// gc/shared/workerThread.hpp:154-171. Temporarily try to set the number of
// active workers. It's not guaranteed that it succeeds, and users need to
// query the number of active workers.
class WithActiveWorkers {
private:
    WorkerThreads* const _workers;
    const uint32_t _prev_active_workers;

public:
    WithActiveWorkers(WorkerThreads* workers, uint32_t num_workers)
        : _workers(workers), _prev_active_workers(workers->active_workers())
    {
        _workers->set_active_workers(num_workers);
    }

    ~WithActiveWorkers() { _workers->set_active_workers(_prev_active_workers); }
};
} // namespace MapleRuntime
#endif // MRT_GC_SHARED_WORKERTHREAD_HPP
