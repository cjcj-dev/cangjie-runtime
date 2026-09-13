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
#include "Heap/z/zDriverPort.hpp"

namespace MapleRuntime {
class Collector;
class CollectorProxy;
#if defined(MRT_TESTABLE_INTERNALS)
class CollectorResourcesTestPeer;
#endif
// CollectorResources provides the resources that a functional collector need,
// such as GC drivers and runtime workers.
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

    bool IsGcStarted() const;

    bool IsGCActive() const { return Heap::GetHeap().IsGCEnabled(); }

    FinalizerProcessor& GetFinalizerProcessor() { return finalizerProcessor; }

    GCStats& GetGCStats(GCCycleGeneration generation = GCCycleGeneration::OLD);

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
    bool ExecuteDriverRequest(const GCDriverRequest& request);
    bool ProcessDriverRequest(GCDriverPort& port, const GCDriverRequest& request);
    void CancelDriverRequestLifecycle(GCDriverKind kind);
    // zCollectedHeap.hpp: heap-owned safepoint workers, separate from both generations.
    RuntimeWorkers* runtimeWorkers = nullptr;
    int32_t gcThreadCount = 1;
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
    pthread_t minorDriverThread = 0;
    pthread_t majorDriverThread = 0;
    int32_t concurrentGcThreadCount = 1;
    std::atomic<bool> gcThreadRunning = { false };
    std::atomic<bool> resurrectionBlocked { false };
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
