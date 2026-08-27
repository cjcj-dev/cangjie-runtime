// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CollectorResources.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#if defined(__linux__) || defined(hongmeng)
#include <sched.h>
#endif
#include <thread>

#include "Base/SysCall.h"
#include "CollectorProxy.h"
#include "MutatorAllocRate.h"
#include "Common/RunType.h"
#include "Common/ScopedObjectAccess.h"
#include "LoaderManager.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
extern "C" uintptr_t MRT_StopGCWork()
{
    Heap::GetHeap().StopGCWork();
    return 0;
}

void* CollectorResources::GCMainThreadEntry(void* arg)
{
#ifdef __APPLE__
    int ret = pthread_setname_np("gc-main-thread");
    CHECK_E(UNLIKELY(ret != 0), "pthread setname in CollectorResources::StartGCThreads() return %d rather than 0",
            ret);
#elif defined(__linux__) || defined(hongmeng)
    int ret = prctl(PR_SET_NAME, "gc-main-thread");
    CHECK_E(UNLIKELY(ret != 0), "pthread setname in CollectorResources::StartGCThreads() return %d rather than 0",
            ret);
#endif
    
    MRT_ASSERT(arg != nullptr, "GCMainThreadEntry arg=nullptr");
    // set current thread as a gc thread.
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);

    LOG(RTLOG_INFO, "[GC] CollectorResources Thread begin.");

#if defined(__linux__) || defined(hongmeng)
    // set thread priority.
    GCPoolThread::SetThreadPriority(MapleRuntime::GetTid(), GCPoolThread::GC_THREAD_PRIORITY);
#endif

    // run event loop in this thread.
    CollectorResources* collectorResources = reinterpret_cast<CollectorResources*>(arg);
    collectorResources->RunTaskLoop();

    LOG(RTLOG_INFO, "[GC] CollectorResources Thread end.");
    return nullptr;
}

void* CollectorResources::MinorDriverThreadEntry(void* arg)
{
    auto* resources = reinterpret_cast<CollectorResources*>(arg);
    MRT_ASSERT(resources != nullptr, "MinorDriverThreadEntry arg=nullptr");
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
    resources->RunDriverLoop(GCDriverKind::MINOR);
    return nullptr;
}

void* CollectorResources::MajorDriverThreadEntry(void* arg)
{
    auto* resources = reinterpret_cast<CollectorResources*>(arg);
    MRT_ASSERT(resources != nullptr, "MajorDriverThreadEntry arg=nullptr");
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
    resources->RunDriverLoop(GCDriverKind::MAJOR);
    return nullptr;
}

void CollectorResources::Init()
{
    minorDriverPort.Reset();
    majorDriverPort.Reset();
    taskQueue = new TaskQueue<GCExecutor>;
    taskQueue->Init();
    finishedGcIndex = GCTask::SYNC_TASK_MIN_INDEX;
    StartGCThreads();
    finalizerProcessor.Start();
    gcStats.Init();
    MutatorAllocRate::initialize();
}

void CollectorResources::Fini()
{
    MRT_ASSERT(!finalizerProcessor.IsRunning(), "Invalid finalizerProcessor status");
    MRT_ASSERT(!gcThreadRunning.load(std::memory_order_relaxed), "Invalid GC thread status");
    taskQueue->Fini();
    delete taskQueue;
    taskQueue = nullptr;
    minorDriverPort.Stop();
    majorDriverPort.Stop();
}

void CollectorResources::StopGCWork()
{
    finalizerProcessor.Stop();
    // Close both ports before joining either driver.  This is the shutdown
    // acknowledgement for synchronous callers: WaitForAck observes stopped
    // and returns immediately instead of enqueueing into an abandoned queue.
    minorDriverPort.Stop();
    majorDriverPort.Stop();
    TerminateGCTask();
    StopGCThreads();
}

// Send terminate task to gc thread.
void CollectorResources::TerminateGCTask()
{
    if (gcThreadRunning.load(std::memory_order_acquire) == false) {
        return;
    }

    TaskQueue<GCExecutor>::TaskFilter filter = [](GCExecutor&, GCExecutor&) { return false; };
    GCExecutor task(GCTask::TaskType::TASK_TYPE_TERMINATE_GC);
    (void)taskQueue->EnqueueSync(task, filter); // enqueue to sync queue
}

// Usually called from main thread, wait for collector thread to exit.
void CollectorResources::StopGCThreads()
{
    if (gcThreadRunning.load(std::memory_order_acquire) == false) {
        return;
    }
    int ret = ::pthread_join(minorDriverThread, nullptr);
    CHECK_E(UNLIKELY(ret != 0), "::pthread_join(minor) in StopGCThreads() return %d", ret);
    ret = ::pthread_join(majorDriverThread, nullptr);
    CHECK_E(UNLIKELY(ret != 0), "::pthread_join(major) in StopGCThreads() return %d", ret);
    // wait the thread pool stopped.
    if (gcThreadPool != nullptr) {
        gcThreadPool->Exit();
        delete gcThreadPool;
        gcThreadPool = nullptr;
    }
    if (evacuationThreadPool != nullptr) {
        evacuationThreadPool->Exit();
        delete evacuationThreadPool;
        evacuationThreadPool = nullptr;
    }
    gcThreadRunning.store(false, std::memory_order_release);
}

void CollectorResources::RunTaskLoop()
{
    // Compatibility loop used by the deterministic gc_unit harness. Product
    // startup uses two dedicated loops below; this loop retains the old
    // single-consumer shape solely for tests that inject a fake Collector.
    gcTid.store(MapleRuntime::GetTid(), std::memory_order_release);
    // Keep the control queue (termination/heap dump) separate from the two
    // generation ports.  Round-robin polling gives each driver progress even
    // when the other generation is continuously requesting collections.
    while (true) {
        GCDriverRequest request {};
        bool haveRequest = minorDriverPort.TryDequeue(request);
        GCDriverPort* port = &minorDriverPort;
        if (!haveRequest) {
            haveRequest = majorDriverPort.TryDequeue(request);
            port = &majorDriverPort;
        }
        if (haveRequest) {
            if (!port->Abort().Poll()) {
                ExecuteDriverRequest(request);
            }
            port->Acknowledge(request.sequence);
            continue;
        }

        GCExecutor controlTask;
        if (taskQueue != nullptr && taskQueue->TryDequeue(controlTask)) {
#if defined(MRT_GC_UNIT_TESTS)
            void* owner = testCollector != nullptr ? static_cast<void*>(testCollector)
                                                   : static_cast<void*>(&collectorProxy);
#else
            void* owner = static_cast<void*>(&collectorProxy);
#endif
            if (!controlTask.Execute(owner)) {
                break;
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    NotifyGCFinished(GCTask::TASK_INDEX_FOR_EXIT);
}

void CollectorResources::RunDriverLoop(GCDriverKind kind)
{
    gcTid.store(MapleRuntime::GetTid(), std::memory_order_release);
    GCDriverPort& port = kind == GCDriverKind::MINOR ? minorDriverPort : majorDriverPort;
    const bool ownsControlQueue = kind == GCDriverKind::MAJOR;
    auto nextTimeout = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (true) {
        GCDriverRequest request {};
        if (port.TryDequeue(request)) {
            if (!port.Abort().Poll()) {
                ExecuteDriverRequest(request);
            }
            port.Acknowledge(request.sequence);
            continue;
        }
        // Stop closes the port and wakes all waiters.  A driver exits once its
        // in-flight request has reached the acknowledgement point.
        if (port.IsStopped()) {
            break;
        }

        if (ownsControlQueue) {
            GCExecutor controlTask;
            if (taskQueue != nullptr && taskQueue->TryDequeue(controlTask)) {
#if defined(MRT_GC_UNIT_TESTS)
                void* owner = testCollector != nullptr ? static_cast<void*>(testCollector)
                                                       : static_cast<void*>(&collectorProxy);
#else
                void* owner = static_cast<void*>(&collectorProxy);
#endif
                if (!controlTask.Execute(owner)) {
                    break;
                }
                nextTimeout = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                continue;
            }
            // TaskQueue::Dequeue historically injected TASK_TYPE_TIMEOUT_GC
            // after one second of idleness. Keep that observable semantic in
            // the split-driver wait path (OpenJDK zDriver timeout/backup arm).
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextTimeout) {
                if (taskQueue != nullptr && Heap::GetHeap().IsGCEnabled()) {
                    taskQueue->EnqueueAsync(GCExecutor(GCTask::TaskType::TASK_TYPE_TIMEOUT_GC));
                }
                nextTimeout = now + std::chrono::seconds(1);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    NotifyGCFinished(GCTask::TASK_INDEX_FOR_EXIT);
}

bool CollectorResources::ExecuteDriverRequest(const GCDriverRequest& request)
{
#if defined(MRT_GC_UNIT_TESTS)
    Collector* collector = testCollector != nullptr ? testCollector : static_cast<Collector*>(&collectorProxy);
#else
    Collector* collector = static_cast<Collector*>(&collectorProxy);
#endif
    // A major cycle is the old driver plus its young prelude.  The prelude is
    // submitted through the minor identity instead of mutating WCollector's
    // reason/statistics fields mid-cycle.
    if (request.reason != GC_REASON_YOUNG) {
        // Major's young prelude is a real request consumed by the minor
        // driver.  The abortpoint below is deliberately between prelude and
        // old, matching zDriver.cpp:416-452: cancellation prevents old work.
#if defined(MRT_GC_UNIT_TESTS)
        collector->RunGarbageCollection(GCTask::ASYNC_TASK_INDEX, GC_REASON_YOUNG);
        if (majorDriverPort.Abort().Poll()) {
            return false;
        }
#else
        const uint64_t prelude = minorDriverPort.EnqueueSync(GC_REASON_YOUNG);
        if (!minorDriverPort.WaitForAck(prelude) || majorDriverPort.Abort().Poll()) {
            return false;
        }
#endif
    }
    if (request.reason != GC_REASON_YOUNG && majorDriverPort.Abort().Poll()) {
        return false;
    }
    VLOG(GCPHASE, "[GCV2][driver] kind=%s seq=%llu reason=%u ack=pending",
         request.reason == GC_REASON_YOUNG ? "minor" : "major",
         static_cast<unsigned long long>(request.sequence), request.reason);
    collector->RunGarbageCollection(request.asynchronous ? GCTask::ASYNC_TASK_INDEX : request.sequence,
                                    request.reason);
    NotifyGCFinished(request.asynchronous ? GCTask::ASYNC_TASK_INDEX : request.sequence);
    return true;
}

// For the ignored gc request, check whether need to wait for current gc finish
void CollectorResources::PostIgnoredGcRequest(bool shouldWait)
{
    if (shouldWait && isGcStarted.load(std::memory_order_seq_cst)) {
        ScopedEnterSaferegion safeRegion(false);
        WaitForGCFinish();
    }
}

#if defined(MRT_TESTABLE_INTERNALS)
bool CollectorResources::ShouldWaitForIgnoredGcRequest(GCReason reason, bool async)
{
    return !async || g_gcRequests[reason].IsSyncGC();
}

bool CollectorResources::HasSyncTaskCompleted(uint64_t finishedIndex, uint64_t awaitedIndex)
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

void CollectorResources::RequestAsyncGC(GCReason reason)
{
    GCDriverPort& port = reason == GC_REASON_YOUNG ? minorDriverPort : majorDriverPort;
    port.EnqueueAsync(reason);
}

void CollectorResources::RequestGCAndWait(GCReason reason)
{
    // Enter saferegion since current thread may blocked by locks.
    ScopedEnterSaferegion enterSaferegion(false);
    GCDriverPort& port = reason == GC_REASON_YOUNG ? minorDriverPort : majorDriverPort;
    const uint64_t sequence = port.EnqueueSync(reason);
    (void)port.WaitForAck(sequence);
}

void CollectorResources::RequestGC(GCReason reason, bool async)
{
    if (!IsGCActive()) {
        return;
    }

    GCRequest& request = g_gcRequests[reason];
    uint64_t curTime = TimeUtil::NanoSeconds();
    request.SetPrevRequestTime(curTime);
#if defined(MRT_GC_UNIT_TESTS)
    Collector& requestOwner = testCollector != nullptr ? *testCollector : static_cast<Collector&>(collectorProxy);
#else
    CollectorProxy& requestOwner = collectorProxy;
#endif
    if (requestOwner.ShouldIgnoreRequest(request)) {
        DLOG(ALLOC, "ignore gc request");
        PostIgnoredGcRequest(ShouldWaitForIgnoredGcRequest(reason, async));
    } else if (async) {
        RequestAsyncGC(reason);
    } else {
        RequestGCAndWait(reason);
    }
}

void CollectorResources::NotifyGCFinished(uint64_t gcIndex)
{
    std::unique_lock<std::mutex> lock(gcFinishedCondMutex);
    isGcStarted.store(false, std::memory_order_release);
    if (gcIndex != GCTask::ASYNC_TASK_INDEX) { // sync gc, need set taskIndex
        finishedGcIndex.store(gcIndex, std::memory_order_release);
    }
    gcFinishedCondVar.notify_all();
    BroadcastGCCompletion();
}

void CollectorResources::WaitForGCFinish()
{
    uint64_t startTime = TimeUtil::MicroSeconds();
    std::unique_lock<std::mutex> lock(gcFinishedCondMutex);
    uint64_t curWaitGcIndex = finishedGcIndex.load();
    std::function<bool()> pred = [this, curWaitGcIndex] {
        return (!IsGcStarted() || (curWaitGcIndex != finishedGcIndex) ||
                (finishedGcIndex == GCTask::TASK_INDEX_FOR_EXIT));
    };
#ifdef __OHOS__
    std::chrono::seconds waitTime(2); // 2 seconds
    gcFinishedCondVar.wait_for(lock, waitTime, pred);
#else
    gcFinishedCondVar.wait(lock, pred);
#endif
    uint64_t stopTime = TimeUtil::MicroSeconds();
    uint64_t diffTime = stopTime - startTime;
    VLOG(REPORT, "WaitForGCFinish cost %zu us", diffTime);
}

void CollectorResources::StartGCThreads()
{
    bool expected = false;
    if (gcThreadRunning.compare_exchange_strong(expected, true, std::memory_order_acquire) == false) {
        return;
    }
    // starts the thread pool.
    if (gcThreadPool == nullptr) {
        // Off by default: on real_load the formula picks 23 workers on a 32-core
        // domain and costs 2.22x task-clock for 1.14x wall (REPORT-jvmparam),
        // which reproduces the earlier no-headroom result from REPORT-gcthreads.
        const char* jvmThreadsEnv = std::getenv("MRT_GCV2_JVM_GC_THREADS");
        const bool useJvmThreads = jvmThreadsEnv != nullptr && std::strcmp(jvmThreadsEnv, "1") == 0;
        unsigned int activeProcessorCount = std::thread::hardware_concurrency();
        bool affinityDetected = false;
#if defined(__linux__) || defined(hongmeng)
        cpu_set_t cpuSet;
        CPU_ZERO(&cpuSet);
        if (sched_getaffinity(0, sizeof(cpuSet), &cpuSet) == 0) {
            int affinityProcessorCount = CPU_COUNT(&cpuSet);
            if (affinityProcessorCount > 0) {
                activeProcessorCount = static_cast<unsigned int>(affinityProcessorCount);
                affinityDetected = true;
            }
        }
#endif
        activeProcessorCount = std::max(activeProcessorCount, 1U);
        if (useJvmThreads) {
            constexpr unsigned int parallelThreadSwitchPoint = 8;
            constexpr unsigned int parallelThreadNumerator = 5;
            constexpr unsigned int parallelThreadDenominator = 8;
            unsigned int parallelThreads = activeProcessorCount <= parallelThreadSwitchPoint ?
                activeProcessorCount : parallelThreadSwitchPoint +
                    (activeProcessorCount - parallelThreadSwitchPoint) * parallelThreadNumerator /
                        parallelThreadDenominator;
            gcThreadCount = static_cast<int32_t>(parallelThreads);
            concurrentGcThreadCount = std::max(static_cast<int32_t>((parallelThreads + 2) / 4), 1);
        } else {
            gcThreadCount = 2;
            concurrentGcThreadCount = 2;
        }
        int32_t helperThreads = gcThreadCount - 1;
        VLOG(REPORT,
             "total gc thread count %d, helper thread count %d, concurrent gc thread count %d, "
             "active processor count %u, affinity detected %d, jvm formula %d",
             gcThreadCount, helperThreads, concurrentGcThreadCount, activeProcessorCount, affinityDetected,
             useJvmThreads);
        gcThreadPool = new (std::nothrow) GCThreadPool("gc", helperThreads, GCPoolThread::GC_THREAD_PRIORITY);
        CHECK_DETAIL(gcThreadPool != nullptr, "new GCThreadPool failed");

        // evacpar: copy already owns work by region, but the shared product pool
        // is normally fixed at two total workers.  A dedicated opt-in pool lets
        // the copy phase scale without also widening ref-fix/mark work.  Unset,
        // malformed, one, and out-of-affinity values preserve the old pool.
        const char* evacWorkersEnv = std::getenv("MRT_GCV2_EVACPAR_WORKERS");
        if (evacWorkersEnv != nullptr && evacWorkersEnv[0] != '\0') {
            char* end = nullptr;
            long requested = std::strtol(evacWorkersEnv, &end, 10);
            bool valid = end != evacWorkersEnv && *end == '\0' && requested >= 2 &&
                static_cast<unsigned long>(requested) <= activeProcessorCount;
            if (valid) {
                int32_t evacHelpers = static_cast<int32_t>(requested) - 1;
                evacuationThreadPool =
                    new (std::nothrow) GCThreadPool("evac", evacHelpers, GCPoolThread::GC_THREAD_STW_PRIORITY);
                CHECK_DETAIL(evacuationThreadPool != nullptr, "new evacuation GCThreadPool failed");
                VLOG(REPORT,
                     "[GCV2][evacpar][config] workers=%ld activeProcessorCount=%u dedicated=1",
                     requested, activeProcessorCount);
            } else {
                VLOG(REPORT,
                     "[GCV2][evacpar][config] invalid workers=%s activeProcessorCount=%u dedicated=0",
                     evacWorkersEnv, activeProcessorCount);
            }
        }
    }

    // ZGC shape: two independent drivers, each consuming only its generation
    // port. The major driver also owns the legacy control/timeout queue.
    if (::pthread_create(&minorDriverThread, nullptr, CollectorResources::MinorDriverThreadEntry, this) != 0) {
        MRT_ASSERT(0, "pthread_create minor driver failed!");
    }
    if (::pthread_create(&majorDriverThread, nullptr, CollectorResources::MajorDriverThreadEntry, this) != 0) {
        minorDriverPort.Stop();
        (void)::pthread_join(minorDriverThread, nullptr);
        MRT_ASSERT(0, "pthread_create major driver failed!");
    }
    // Keep the historical handle as an alias for diagnostics that name the
    // collector's main thread; shutdown joins both concrete driver handles.
    gcMainThread = majorDriverThread;
    // set thread name.
#ifdef __WIN64
    int ret = pthread_setname_np(majorDriverThread, "gc-major-driver");
    CHECK_E(UNLIKELY(ret != 0), "pthread_setname_np() in CollectorResources::StartGCThreads() return %d rather than 0",
            ret);
#endif
}

int32_t CollectorResources::GetGCThreadCount(const bool isConcurrent) const
{
    if (GetThreadPool() == nullptr) {
        return 1;
    }
    return isConcurrent ? concurrentGcThreadCount : gcThreadCount;
}

void CollectorResources::BroadcastGCCompletion()
{
    gcWorking = 0;
#if defined(_WIN64) || defined(__APPLE__)
    WakeWhenGCDone();
#else
    (void)Futex(&gcWorking, FUTEX_WAKE_PRIVATE, INT_MAX);
#endif
}

void CollectorResources::RequestHeapDump(GCTask::TaskType gcTask)
{
    TaskQueue<GCExecutor>::TaskFilter filter = [](GCExecutor&, GCExecutor&) { return false; };
    GCExecutor dumpTask = GCExecutor(gcTask);
    taskQueue->EnqueueSync(dumpTask, filter);
}

} // namespace MapleRuntime
