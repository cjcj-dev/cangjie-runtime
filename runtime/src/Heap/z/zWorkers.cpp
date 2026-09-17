// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zWorkers.inline.hpp"

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zTask.hpp"

namespace MapleRuntime {
// zWorkers.cpp:33-43
static const char* workers_name(ZGenerationId id)
{
    return (id == ZGenerationId::young) ? "ZWorkerYoung" : "ZWorkerOld";
}

static const char* generation_name(ZGenerationId id)
{
    return (id == ZGenerationId::young) ? "Young" : "Old";
}

// zWorkers.cpp:45-65
ZWorkers::ZWorkers(ZGenerationId id, uint32_t max_nworkers, ZStatWorkers* stats)
    : _workers(workers_name(id), max_nworkers),
      _generation_name(generation_name(id)),
      _resize_lock(),
      _requested_nworkers(0),
      _is_active(false),
      _stats(stats)
{
    LOG(RTLOG_INFO, "GC Workers for %s Generation: %u (static)", _generation_name, _workers.max_workers());

    // Initialize worker threads
    _workers.initialize_workers();
    _workers.set_active_workers(_workers.max_workers());
    if (_workers.active_workers() != _workers.max_workers()) {
        CHECK_DETAIL(false, "Failed to create ZWorkers");
    }
}

bool ZWorkers::is_active() const
{
    return _is_active;
}

uint32_t ZWorkers::active_workers() const
{
    return _workers.active_workers();
}

void ZWorkers::set_active_workers(uint32_t nworkers)
{
    VLOG(REPORT, "Using %u Workers for %s Generation", nworkers, _generation_name);
    std::lock_guard<std::mutex> locker(_resize_lock);
    _workers.set_active_workers(nworkers);
}

void ZWorkers::set_active()
{
    std::lock_guard<std::mutex> locker(_resize_lock);
    _is_active = true;
    _requested_nworkers.store(0, std::memory_order_relaxed);
}

void ZWorkers::set_inactive()
{
    std::lock_guard<std::mutex> locker(_resize_lock);
    _is_active = false;
}

// zWorkers.cpp:92-106
void ZWorkers::run(ZTask* task)
{
    VLOG(GCPHASE, "Executing %s using %s with %u workers", task->name(), _workers.name(), active_workers());

    {
        std::lock_guard<std::mutex> locker(_resize_lock);
        _stats->at_start(active_workers());
    }

    _workers.run_task(task->worker_task());

    {
        std::lock_guard<std::mutex> locker(_resize_lock);
        _stats->at_end();
    }
}

// zWorkers.cpp:108-124
void ZWorkers::run(ZRestartableTask* task)
{
    for (;;) {
        // Run task
        run(static_cast<ZTask*>(task));

        std::lock_guard<std::mutex> locker(_resize_lock);
        const uint32_t requested = _requested_nworkers.load(std::memory_order_relaxed);
        if (requested == 0) {
            // Task completed
            return;
        }

        // Restart task with requested number of active workers
        _workers.set_active_workers(requested);
        task->resize_workers(active_workers());
        _requested_nworkers.store(0, std::memory_order_relaxed);
    }
}

// zWorkers.cpp:126-137
void ZWorkers::run_all(ZTask* task)
{
    // Get and set number of active workers
    const uint32_t prev_active_workers = _workers.active_workers();
    _workers.set_active_workers(_workers.max_workers());

    // Execute task using all workers
    VLOG(GCPHASE, "Executing %s using %s with %u workers", task->name(), _workers.name(), active_workers());
    _workers.run_task(task->worker_task());

    // Restore number of active workers
    _workers.set_active_workers(prev_active_workers);
}

void ZWorkers::threads_do(const std::function<void(WorkerThread*)>& tc) const
{
    _workers.threads_do(tc);
}

std::mutex* ZWorkers::resizing_lock()
{
    return &_resize_lock;
}

// zWorkers.cpp:147-166
void ZWorkers::request_resize_workers(uint32_t nworkers)
{
    DCHECK(nworkers != 0);

    std::lock_guard<std::mutex> locker(_resize_lock);

    if (_requested_nworkers.load(std::memory_order_relaxed) == nworkers) {
        // Already requested
        return;
    }

    if (_workers.active_workers() == nworkers) {
        // Already the right amount of threads
        return;
    }

    VLOG(REPORT, "Adjusting Workers for %s Generation: %u -> %u", _generation_name, _workers.active_workers(),
         nworkers);

    _requested_nworkers.store(nworkers, std::memory_order_relaxed);
}
} // namespace MapleRuntime
