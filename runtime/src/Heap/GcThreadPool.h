// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_GC_THREAD_POOL_H
#define MRT_GC_THREAD_POOL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <vector>

#include "Base/LogFile.h"
#include "Base/Macros.h"

namespace MapleRuntime {
// ZWorkers / ZTask (zWorkers.cpp:46-137, zTask.hpp:33-56).
// A task is borrowed until Run returns; it must not throw from Work/ResizeWorkers.
class GCWorkerTask {
public:
    virtual ~GCWorkerTask() = default;
    virtual void Work(uint32_t workerId) = 0;
};

class GCRestartableWorkerTask : public GCWorkerTask {
public:
    // Called only after every participant has returned its private work.
    virtual void ResizeWorkers(uint32_t workers) = 0;
};

class GCWorkers {
public:
    enum class Generation : uint8_t { YOUNG, OLD };
    struct Snapshot {
        Generation generation;
        uint32_t capacity;
        uint32_t activeWorkers;
        uint32_t runningWorkers;
        uint32_t remainingWorkers;
        uint32_t requestedWorkers;
        bool cycleActive;
        bool closing;
        bool stopped;
        uint64_t batch;
        uint64_t completedBatches;
        uint64_t elapsedNanos;
        uint64_t workerNanos; // Sum of each completed batch's duration * participants.
    };

    explicit GCWorkers(Generation generation, uint32_t capacity, int32_t priority = 0);
    ~GCWorkers();
    GCWorkers(const GCWorkers&) = delete;
    GCWorkers& operator=(const GCWorkers&) = delete;

    // One coordinator per set. Run, RunAll, SetActiveWorkers and cycle changes
    // serialize. Workers must not call these methods or Stop on their own set.
    void Run(GCWorkerTask& task);
    void Run(GCRestartableWorkerTask& task);
    void RunAll(GCWorkerTask& task);
    void SetActiveWorkers(uint32_t workers); // Valid range [1, capacity].
    void SetActive(); // Begins a cycle and clears previous resize requests.
    void SetInactive();
    uint32_t ActiveWorkers() const;
    bool IsActive() const;
    void RequestResize(uint32_t workers);
    bool ShouldWorkerResize() const;
    Snapshot GetSnapshot() const;

    // The callback runs while thread lifetime is protected; it must not reenter
    // this set or retain thread handles past the callback.
    void ThreadsDo(const std::function<void(pthread_t)>& visitor);
    // Close admission, finish the borrowed task (including restart), then wake
    // and join all threads. Concurrent/repeated Stop is safe. No cancellation.
    void Stop();

private:
    struct Worker {
        GCWorkers* owner;
        uint32_t id;
        pthread_t thread;
    };
    static void* WorkerEntry(void* argument);
    void WorkerLoop(uint32_t id);
    void RunBatch(GCWorkerTask& task);
    void CheckCount(uint32_t workers) const;
    void CheckOpen() const;

    const Generation generation;
    const uint32_t capacity;
    const int32_t priority;
    std::mutex coordinatorMutex;
    mutable std::mutex mutex;
    std::condition_variable dispatched;
    std::condition_variable completed;
    std::vector<Worker> threads;
    uint32_t activeWorkers;
    uint32_t runningWorkers = 0;
    uint32_t remainingWorkers = 0;
    uint32_t requestedWorkers = 0;
    bool cycleActive = false;
    bool closing = false;
    bool shutdown = false;
    bool stopped = false;
    uint64_t batch = 0;
    uint64_t completedBatches = 0;
    uint64_t elapsedNanos = 0;
    uint64_t workerNanos = 0;
    GCWorkerTask* currentTask = nullptr;
};

// ZRuntimeWorkers owns a separate, fixed-size set for safepoint work.
// Run borrows the task until all workers have returned; the caller does not
// participate. Task callbacks must not throw or reenter their own worker set.
class RuntimeWorkers {
public:
    explicit RuntimeWorkers(uint32_t workers);
    ~RuntimeWorkers();
    RuntimeWorkers(const RuntimeWorkers&) = delete;
    RuntimeWorkers& operator=(const RuntimeWorkers&) = delete;

    void Run(GCWorkerTask& task);
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
    GCWorkerTask* currentTask = nullptr;
    uint64_t batch = 0;
    uint32_t remainingWorkers = 0;
    bool shutdown = false;
};
} // namespace MapleRuntime

#endif // MRT_GC_THREAD_POOL_H
