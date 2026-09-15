// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <vector>

#include "Base/LogFile.h"
#include "Base/Macros.h"

#include "Heap/z/zTask.hpp"
namespace MapleRuntime {
// ZRuntimeWorkers owns a separate, fixed-size set for safepoint work.
// Run borrows the task until all workers have returned; the caller does not
// participate. Task callbacks must not throw or reenter their own worker set.
class RuntimeWorkers {
public:
    explicit RuntimeWorkers(uint32_t workers);
    ~RuntimeWorkers();
    RuntimeWorkers(const RuntimeWorkers&) = delete;
    RuntimeWorkers& operator=(const RuntimeWorkers&) = delete;

    void Run(ZTask& task);
    uint32_t ActiveWorkers() const { return threads.size(); }
    void ThreadsDo(const std::function<void(pthread_t)>& visitor);
#if defined(__linux__) || defined(hongmeng)
    // Shared platform priority setup, independent of generation ownership.
    static void SetThreadPriority(pid_t tid, int32_t priority = 0);
#endif

private:
    struct Worker {
        RuntimeWorkers* owner;
        uint32_t id;
        pthread_t thread;
    };
    static void* WorkerEntry(void* argument);
    void WorkerLoop(uint32_t id);

    std::vector<Worker> threads;
    std::mutex coordinatorMutex;
    std::mutex mutex;
    std::condition_variable dispatched;
    std::condition_variable completed;
    WorkerTask* currentTask = nullptr;
    uint64_t batch = 0;
    uint32_t remainingWorkers = 0;
    bool shutdown = false;
};
}
