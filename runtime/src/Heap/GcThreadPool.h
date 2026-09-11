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
#include <queue>
#include <vector>

#include "Base/LogFile.h"
#include "Base/Macros.h"
#include "Heap/HeapWork.h"

// thread pool implementation
namespace MapleRuntime {
class GCThreadPool;

class GCPoolThread {
public:
    // schedule priority for GC threads.
    // only superuser can lower priority.
    static constexpr int32_t GC_THREAD_PRIORITY = 0;            // default priority
    static constexpr int32_t GC_THREAD_STW_PRIORITY = 0;        // priority used in stw stage
    static constexpr int32_t GC_THREAD_CONCURRENT_PRIORITY = 0; // priority used in concurrent stage
    static constexpr int32_t PRIORITY_PROMOTE_STEP = -5;        // priority promote step

    GCPoolThread(GCThreadPool* threadPool, const char* threadName, size_t threadId, size_t stackSize);
    ~GCPoolThread();

    void SetPriority(int32_t prior) const;

    // get pthread of thread
    pthread_t GetThread() const { return pthread; }

    // get thread id of thread
    pid_t GetTid() const { return tid; }

    static void* WorkerFunc(void* param);
#if defined(__linux__) || defined(hongmeng)
    static void SetThreadPriority(pid_t tid, int32_t priority);
#endif
private:
    size_t id;
    pthread_t pthread;
    pid_t tid;
    CString name;
    GCThreadPool* pool;
};

// manual
// new  (SetMaxActiveThreadNum(optional) addWork  startPool waitFinish)^. Exit delete
// if need to change MaxActiveThreadNum, should waitFinish or stop pool at first
class GCThreadPool {
public:
    // Constructor for thread pool, 1) Create threads, 2) wait all thread created & sleep
    // name is the thread pool name. thread name = Pool_$(poolname)_ThreadId_$(threadId)
    // maxThreadNum is the max thread number in pool.
    // prior is the priority of threads in pool.
    GCThreadPool(const char* name, int32_t maxThreadNum, int32_t prior);

    // Destructor for thread pool, 1) close pool 2) wait thread in pool to exit, 3) release resources of class
    ~GCThreadPool();

    // Set priority of each thread in pool.
    void SetPriority(int32_t prior) const;

    // Set max active thread number of pool, redundant thread hangup in sleep condition var.
    // notify more waiting thread get to work when pool is running.
    // Range [0 - maxThreadNum]. Zero leaves all work to the calling GC thread.
    void SetMaxActiveThreadNum(int32_t num);

    // Get max active thread number of pool.
    int32_t GetMaxActiveThreadNum() const { return maxActiveThreadNum; }

    // Get max thread number of pool, default = maxThreadNum.
    int32_t GetMaxThreadNum() const { return maxThreadNum; }

    int32_t GetWaitingThreadNumber() const { return waitingThreadNum; }

    // Add new task to task queue , task should inherit from HeapWork.
    void AddWork(HeapWork* task);

    // Start thread pool, notify all sleep threads to get to work
    void Start();

    // Wait all task in task queue finished, if pool stopped, only wait until current executing task finish
    // after all task finished, stop pool
    // addToExecute indicate whether the caller thread execute task
    void WaitFinish();

    // used in none-parallel concurrent mark
    void DrainWorkQueue();

    // Notify & Wait all thread waiting for task to sleep
    void Stop();

    // Notify all thread in pool to exit , notify all waitFinish thread to return,nonblock ^.
    void Exit();

    // Remove all task in task queue
    void ClearAllWork();

    // Get work count in queue
    size_t GetWorkCount()
    {
        std::unique_lock<std::mutex> taskLock(poolMutex);
        return workQueue.size();
    }

    // Get all GCPoolThread in pool
    const std::vector<GCPoolThread*>& GetThreads() const { return threads; }

private:
    // thread default stack size 512 KB.
    static constexpr size_t DEFAULT_STACK_SIZE = (512 * 1024);
    int32_t priority;

    CString name;
    // pool stop or running state
    std::atomic<bool> running;
    // is pool exit
    std::atomic<bool> exit;
    // all task put in task queue
    std::queue<HeapWork*> workQueue;

    // active thread 0 ..... maxActiveThreadNum .....maxThreadNum
    // max thread number in pool
    int32_t maxThreadNum;

    // max active thread number, redundant thread hang up in threadSleepingCondVar
    int32_t maxActiveThreadNum;

    // current active thread, when equals to zero, no thread running, all thread slept
    int32_t currActiveThreadNum;

    // current watiing thread, when equals to currActiveThreadNum
    // no thread executing, all task finished
    int32_t waitingThreadNum;

    // single lock
    std::mutex poolMutex;

    // hangup when no task available
    std::condition_variable taskEmptyCondVar;

    // hangup when too much active thread or pool stopped
    std::condition_variable threadSleepingCondVar;

    // hangup when there is thread executing
    std::condition_variable allWorkDoneCondVar;

    // hangup when there is thread active
    std::condition_variable allThreadStopped;

    // use for profiling
    std::vector<GCPoolThread*> threads;

    // is pool running or stopped
    bool IsRunning() const { return running.load(std::memory_order_relaxed); }

    bool IsExited() const { return exit.load(std::memory_order_relaxed); }

    friend class GCPoolThread;
};

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

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
// Dump cpu clock time of parallel task
class ScopedCpuTime {
public:
    explicit ScopedCpuTime(GCThreadPool& threadPool, LogType type);
    ~ScopedCpuTime();

private:
    LogType logType;
#if defined(__linux__) || defined(hongmeng)
    size_t threadCount = 0;
    std::vector<clockid_t> cid;
    std::vector<struct timespec> workerStart;
#endif
};
#endif
} // namespace MapleRuntime

#endif // MRT_GC_THREAD_POOL_H
