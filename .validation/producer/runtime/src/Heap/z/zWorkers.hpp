// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_Z_ZWORKERS_HPP
#define MRT_GC_Z_ZWORKERS_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

#include "Base/LogFile.h"
#include "Heap/z/workerThread.hpp"

namespace MapleRuntime {
enum class ZGenerationId : uint8_t; // zGenerationId.hpp
class ZRestartableTask;
class ZStatWorkers;
class ZTask;

// zWorkers.hpp:38-67. The generation's view of its WorkerThreads: six fields,
// no task state, no timing, no stop protocol. ZLock is std::mutex here (I14).
class ZWorkers {
private:
    WorkerThreads _workers;
    const char* const _generation_name;
    std::mutex _resize_lock;
    std::atomic<uint32_t> _requested_nworkers;
    bool _is_active;
    ZStatWorkers* const _stats;

public:
    // zWorkers.cpp:45-65. max_nworkers is ZYoungGCThreads/ZOldGCThreads in
    // ZGC (zArguments); this runtime passes the concurrent budget in.
    ZWorkers(ZGenerationId id, uint32_t max_nworkers, ZStatWorkers* stats);
    ZWorkers(const ZWorkers&) = delete;
    ZWorkers& operator=(const ZWorkers&) = delete;

    bool is_active() const;
    uint32_t active_workers() const;
    void set_active_workers(uint32_t nworkers);
    void set_active();
    void set_inactive();

    void run(ZTask* task);
    void run(ZRestartableTask* task);
    void run_all(ZTask* task);

    void threads_do(const std::function<void(WorkerThread*)>& tc) const;

    // Worker resizing
    std::mutex* resizing_lock();
    ZStatWorkers* stat_workers() { return _stats; }
    void request_resize_workers(uint32_t nworkers);

    bool should_worker_resize();
};
} // namespace MapleRuntime
#endif // MRT_GC_Z_ZWORKERS_HPP
