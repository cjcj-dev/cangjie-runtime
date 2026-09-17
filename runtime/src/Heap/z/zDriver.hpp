// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_RESOURCES_H
#define MRT_COLLECTOR_RESOURCES_H

#include <cstdint>
#include <functional>

#include "Base/Macros.h"
#include "Heap/z/zStat.hpp"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Collector/TaskQueue.h"
#include "Heap/z/zThread.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Inspector/CjHeapData.h"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zDriverPort.hpp"
#include "Heap/z/zResurrection.inline.hpp"

namespace MapleRuntime {

enum class GCDriverKind : uint8_t { MINOR, MAJOR };

class Collector;
class CollectorResources;
#if defined(MRT_TESTABLE_INTERNALS)
class CollectorResourcesTestPeer;
#endif

// zDriver.hpp:48-119: ZDriverMinor/ZDriverMajor are ZThreads whose run_thread
// receives requests from their port and whose terminate closes that port.
class ZDriver : public ZThread {
public:
    ZDriver(CollectorResources& resources, GCDriverKind kind);
    void run_thread() override;
    void terminate() override;
    bool is_busy() const;
protected:
    CollectorResources& resources;
    const GCDriverKind kind;
};

class ZDriverMinor final : public ZDriver {
public:
    explicit ZDriverMinor(CollectorResources& resources) : ZDriver(resources, GCDriverKind::MINOR) {}
    void collect(const ZDriverRequest& request);
};

class ZDriverMajor final : public ZDriver {
public:
    explicit ZDriverMajor(CollectorResources& resources) : ZDriver(resources, GCDriverKind::MAJOR) {}
    void collect(const ZDriverRequest& request);
};

// CollectorResources provides the resources that a functional collector need,
// such as GC drivers and workers.
class CollectorResources {
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct MarkPublicationFixture;
#endif
    friend class ZDirector;
    friend class ZDriver;
    friend struct RelocationReceiptTestAccess;
    friend struct MarkPort203TestAccess;
public:
    // a collectorResources without a collector entity is functionless
    explicit CollectorResources(Collector& collector);
    ATTR_NO_INLINE virtual ~CollectorResources() = default;

    void Init();
    void Fini();
    void StopGCWork();
    void LockDriver() { driverLock.lock(); }
    void UnlockDriver() { driverLock.unlock(); }
    void RequestGC(GCReason reason, bool async);

    ZWorkers& GetWorkers(ZGenerationId generation) const;

    // ZYoungType::major_full_roots selects the combined mark-start pause.

    // Called once in the young mark-start pause, for both minor and
    // combined young/old starts (zGeneration.cpp:600-602,637).
    void NoteYoungMarkStart(ZYoungType type)
    {
        ZStat::Collections().AtYoungMarkStart(type == ZYoungType::major_full_roots ||
                                             type == ZYoungType::major_partial_roots);
    }

    // ZResurrection (zResurrection.cpp:35-47): shared by both generations.
    // Block only in the successful old mark-end pause; unblock after the
    // non-strong reference rendezvous, before finalizer enqueue.
    void BlockResurrection() { ZResurrection::block(); }
    void UnblockResurrection() { ZResurrection::unblock(); }
    bool IsResurrectionBlocked() const { return ZResurrection::is_blocked(); }

    bool IsGcStarted() const;

    bool IsGCActive() const { return Heap::GetHeap().IsGCEnabled(); }

    FinalizerProcessor& GetFinalizerProcessor() { return finalizerProcessor; }
    Collector& bound_collector() { return collector; }

    GCStats& GetGCStats(ZGenerationId generation = ZGenerationId::old);

    // ZGC-style per-generation request ports.  Requests on one port never
    // consume or coalesce requests from the other generation.
    ZDriverPort& GetMinorDriverPort() { return minorDriverPort; }
    ZDriverPort& GetMajorDriverPort() { return majorDriverPort; }
    ZDriverPort& GetYoungDriverPort();
    void RequestAbort(GCDriverKind kind)
    {
        ZAbort::abort();
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
    bool start_gc(uint64_t now);
    void CompleteDriverRequest(ZDriverPort& port);
    void RunCollection(Collector& collector, uint64_t index, GCReason reason, bool warmup);
    void RunYoungCollection(Collector& collector, uint64_t index, ZYoungType type, bool warmup);
    bool ShouldPrecleanYoung(GCReason reason) const;

    // Notify the GC thread to start GC, and doesn't wait.
    // Called by mutator.
    // reason: The reason for this GC.
    void RequestAsyncGC(GCReason reason);
    void RequestGCAndWait(GCReason reason);
    bool ExecuteDriverRequest(const ZDriverRequest& request);
    bool ProcessDriverRequest(ZDriverPort& port, const ZDriverRequest& request);
    void CancelDriverRequestLifecycle(GCDriverKind kind);
    ZDriverPort minorDriverPort;
    ZDriverPort majorDriverPort;
    // zDriver.cpp:59-72: held by young; old releases it for its body.
    std::mutex driverLock;
#if defined(MRT_GC_UNIT_TESTS) || defined(MRT_TESTABLE_INTERNALS)
public:
    Collector* testCollector = nullptr;
private:
    std::function<void()> testAfterYoungPrelude;
    std::atomic<size_t> testCompletionCount { 0 };
#endif

    // zCollectedHeap.cpp:65-71 / zHeap.hpp: the concurrent GC threads are
    // created when GC starts and stopped through ConcurrentGCThread::stop.
    ZDirector* director = nullptr;
    ZDriverMinor* minorDriver = nullptr;
    ZDriverMajor* majorDriver = nullptr;
    std::mutex directorMutex;
    std::condition_variable directorCondition;
    bool directorStopped = false;
    bool directorReevaluate = false;
    bool minorBusy = false;
    bool majorBusy = false;
    ZStat* statistics = nullptr;
    int32_t concurrentGcThreadCount = 1;
    std::atomic<bool> gcThreadRunning = { false };
    Collector& collector;
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
