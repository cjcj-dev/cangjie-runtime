// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_RESOURCES_H
#define MRT_COLLECTOR_RESOURCES_H

#include <functional>

#include "Base/Macros.h"
#include "Heap/z/zStat.hpp"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Collector/TaskQueue.h"
#include "Heap/z/zWorkers.hpp"
#include "Inspector/CjHeapData.h"
#include "Heap/Collector/TaskQueue.h"
#include "Heap/z/zDriverPort.hpp"

namespace MapleRuntime {
class Collector;
class CollectorProxy;
#if defined(MRT_TESTABLE_INTERNALS)
class CollectorResourcesTestPeer;
#endif
// CollectorResources provides the resources that a functional collector need,
// such as GC drivers/runtime workers, gc task queue...
class CollectorResources {
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct MarkPublicationFixture;
#endif
public:
    // the collector thread entry routine.
    static void* DirectorThreadEntry(void* arg);
    MRT_EXPORT static void* MinorDriverThreadEntry(void* arg);
    MRT_EXPORT static void* MajorDriverThreadEntry(void* arg);

    // a collectorResources without a collector entity is functionless
    explicit CollectorResources(CollectorProxy& proxy);
    ATTR_NO_INLINE virtual ~CollectorResources() = default;

    void Init();
    void Fini();
    void StopGCWork();
    void LockDriver() { driverLock.lock(); }
    void UnlockDriver() { driverLock.unlock(); }
    void RequestGC(GCReason reason, bool async);
    void WaitForGCFinish();
    // gc main loop
    // Notify that GC has finished.
    // Must be called by gc thread only
    void NotifyGCFinished(uint64_t gcIndex);
    // A collector phase completes here. A driver-owned multi-phase request
    // suppresses this intermediate publication and publishes once at its end.
    int32_t GetGCThreadCount(const bool isConcurrent) const;

    RuntimeWorkers& GetRuntimeWorkers() const { return *runtimeWorkers; }

    GCWorkers& GetWorkers(GCCycleGeneration generation) const;

    // ZYoungType::major_full_roots selects the combined mark-start pause.
    const GCDriverRequest* YoungPreludeRequest() const { return youngPreludeRequest; }

    // Called once in the young mark-start pause, for both minor and
    // combined young/old starts (zGeneration.cpp:600-602,637).
    void NoteYoungMarkStart() { ZStat::Collections().AtYoungMarkStart(youngPreludeRequest != nullptr); }

    // ZResurrection (zResurrection.cpp:35-47): shared by both generations.
    // Block only in the successful old mark-end pause; unblock after the
    // non-strong reference rendezvous, before finalizer enqueue.
    void BlockResurrection() { resurrectionBlocked.store(true, std::memory_order_release); }
    void UnblockResurrection() { resurrectionBlocked.store(false, std::memory_order_release); }
    bool IsResurrectionBlocked() const { return resurrectionBlocked.load(std::memory_order_acquire); }

    bool IsHeapMarked() const { return isHeapMarked; }

    void SetHeapMarked(bool value) { isHeapMarked = value; }

    bool IsGcStarted() const;

    bool IsGCActive() const { return Heap::GetHeap().IsGCEnabled() && isGCActive.load(std::memory_order_relaxed); }

    FinalizerProcessor& GetFinalizerProcessor() { return finalizerProcessor; }

    void BroadcastGCCompletion();
    GCStats& GetGCStats(GCCycleGeneration generation = GCCycleGeneration::OLD);
    void RequestHeapDump(GCTask::TaskType gcTask);

    // ZGC-style per-generation request ports.  Requests on one port never
    // consume or coalesce requests from the other generation.
    GCDriverPort& GetMinorDriverPort() { return minorDriverPort; }
    GCDriverPort& GetMajorDriverPort() { return majorDriverPort; }
    GCDriverPort& GetYoungDriverPort()
    {
        return youngPreludeRequest != nullptr ? majorDriverPort : minorDriverPort;
    }
    void RequestAbort(GCDriverKind kind)
    {
        (kind == GCDriverKind::MINOR ? minorDriverPort : majorDriverPort).Abort().Request();
    }

#if defined(MRT_TESTABLE_INTERNALS)
    friend struct RelocationReceiptTestAccess;
    friend struct MarkPort203TestAccess;
#endif

private:
#if defined(MRT_TESTABLE_INTERNALS)
    friend class CollectorResourcesTestPeer;
#endif

    void StartGCThreads();
    void StopGCThreads();
    void RunDriverLoop(GCDriverKind kind);
    void RunDirectorLoop();
    void EvaluateDirector(uint64_t now);
    bool TakeDriverRequest(GCDriverPort& port, GCDriverRequest& request);
    void CompleteDriverRequest(GCDriverPort& port);
    void RunCollection(Collector& collector, uint64_t index, GCReason reason, bool warmup);

    // Notify the GC thread to start GC, and doesn't wait.
    // Called by mutator.
    // reason: The reason for this GC.
    void RequestAsyncGC(GCReason reason);
    void RequestGCAndWait(GCReason reason);
    void PostIgnoredGcRequest(bool shouldWait);
    bool ExecuteDriverRequest(const GCDriverRequest& request);
    bool ProcessDriverRequest(GCDriverPort& port, const GCDriverRequest& request);
    void CancelDriverRequestLifecycle();
#if defined(MRT_TESTABLE_INTERNALS)
    MRT_EXPORT static bool ShouldWaitForIgnoredGcRequest(GCReason reason, bool async);
    MRT_EXPORT static bool HasSyncTaskCompleted(uint64_t finishedIndex, uint64_t awaitedIndex);
#else
    ALWAYS_INLINE static inline bool ShouldWaitForIgnoredGcRequest(GCReason reason, bool async)
    {
        return !async || g_gcRequests[reason].IsSyncGC();
    }

    ALWAYS_INLINE static inline bool HasSyncTaskCompleted(uint64_t finishedIndex, uint64_t awaitedIndex)
    {
        if (finishedIndex == GCTask::TASK_INDEX_FOR_EXIT) {
            return true;
        }
        MRT_ASSERT(finishedIndex >= GCTask::SYNC_TASK_MIN_INDEX && finishedIndex < GCTask::ASYNC_TASK_INDEX,
                   "finished sync task index must not be a sentinel");
        MRT_ASSERT(awaitedIndex >= GCTask::SYNC_TASK_MIN_INDEX && awaitedIndex < GCTask::ASYNC_TASK_INDEX,
                   "awaited sync task index must not be a sentinel");
        constexpr uint64_t ringSize = GCTask::ASYNC_TASK_INDEX - GCTask::SYNC_TASK_MIN_INDEX;
        constexpr uint64_t halfRing = ringSize / 2;
        uint64_t finishedOrdinal = finishedIndex - GCTask::SYNC_TASK_MIN_INDEX;
        uint64_t awaitedOrdinal = awaitedIndex - GCTask::SYNC_TASK_MIN_INDEX;
        uint64_t forwardDistance = finishedOrdinal >= awaitedOrdinal
            ? finishedOrdinal - awaitedOrdinal
            : ringSize - awaitedOrdinal + finishedOrdinal;
        return forwardDistance <= halfRing;
    }
#endif

    // zCollectedHeap.hpp: heap-owned safepoint workers, separate from both generations.
    RuntimeWorkers* runtimeWorkers = nullptr;
    int32_t gcThreadCount = 1;
    TaskQueue<GCExecutor>* taskQueue = nullptr;
    GCDriverPort minorDriverPort { GCDriverKind::MINOR };
    GCDriverPort majorDriverPort { GCDriverKind::MAJOR };
    // zDriver.cpp:59-72: held by young; old releases it for its body.
    std::mutex driverLock;
    const GCDriverRequest* youngPreludeRequest = nullptr;
#if defined(MRT_GC_UNIT_TESTS)
    // Deterministic unit builds can replace only the task executor.  The
    // default product retains CollectorProxy as its sole owner and ABI shape.
    Collector* testCollector = nullptr;
    std::function<void()> testAfterYoungPrelude;
    std::atomic<size_t> testCompletionCount { 0 };
#endif

    // the collector thread handle.
    pthread_t directorThread = 0;
    std::mutex directorMutex;
    std::condition_variable directorCondition;
    bool directorStopped = false;
    bool directorReevaluate = false;
    bool minorBusy = false;
    bool majorBusy = false;
    ZStat statistics;
    ZStatCycle youngCycle;
    ZStatCycle oldCycle;
    pthread_t gcMainThread = 0;
    pthread_t minorDriverThread = 0;
    pthread_t majorDriverThread = 0;
    int32_t concurrentGcThreadCount = 1;
    std::atomic<pid_t> gcTid{ 0 };
    std::atomic<bool> gcThreadRunning = { false };
    // finishedGcIndex records the currently finished gcIndex
    // may be read by mutator but only be written by gc thread sequentially
    std::atomic<uint64_t> finishedGcIndex = { 0 };
    // protect condition_variable gcFinishedCondVar's status.
    std::mutex gcFinishedCondMutex;
    // notified when GC finished, requires gcFinishedCondMutex
    std::condition_variable gcFinishedCondVar;

    // a switch to disable gc for hotupdate.
    std::atomic<bool> isGCActive = { true };

    // only gc thread can access it, so we don't use atomic type
    bool isHeapMarked = false;
    std::atomic<bool> resurrectionBlocked { false };
    // Represent the number of returned raw pointer
    std::atomic<int> criticalNum{ 0 };
    int gcWorking = 0;
#if defined(_WIN64) || defined(__APPLE__)
    std::condition_variable gcWorkingCV;
    std::mutex gcWorkingMtx;
    __attribute__((always_inline)) inline void WaitUntilGCDone()
    {
        std::unique_lock<std::mutex> gcWorkingLck(gcWorkingMtx);
        gcWorkingCV.wait(gcWorkingLck);
    }

    __attribute__((always_inline)) inline void WakeWhenGCDone()
    {
        std::unique_lock<std::mutex> gcWorkingLck(gcWorkingMtx);
        gcWorkingCV.notify_all();
    }
#endif
    CollectorProxy& collectorProxy;
    FinalizerProcessor finalizerProcessor;
};
// zDriver.cpp:85-107: lock scopes shared by both generation drivers.
class DriverLocker {
public:
    explicit DriverLocker(CollectorResources& resources) : resources(resources) { resources.LockDriver(); }
    ~DriverLocker() { resources.UnlockDriver(); }
    DriverLocker(const DriverLocker&) = delete;
    DriverLocker& operator=(const DriverLocker&) = delete;
private:
    CollectorResources& resources;
};

class DriverUnlocker {
public:
    explicit DriverUnlocker(CollectorResources& resources) : resources(resources) { resources.UnlockDriver(); }
    ~DriverUnlocker() { resources.LockDriver(); }
    DriverUnlocker(const DriverUnlocker&) = delete;
    DriverUnlocker& operator=(const DriverUnlocker&) = delete;
private:
    CollectorResources& resources;
};
} // namespace MapleRuntime
#endif // MRT_COLLECTOR_RESOURCES_H
