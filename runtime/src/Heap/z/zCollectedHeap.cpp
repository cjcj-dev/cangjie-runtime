// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zCollectedHeap.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <thread>
#if defined(__linux__) || defined(hongmeng)
#include <sched.h>
#endif

#include "Base/ImmortalWrapper.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zStat.hpp"
#include "Common/BaseObject.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Common/StateWord.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zThread.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Base/TimeUtils.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/Mutator.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
ZCollectedHeap* ZCollectedHeap::_collected_heap = nullptr;

ZCollectedHeap* ZCollectedHeap::heap()
{
    if (_collected_heap == nullptr) {
        HeapParam params{};
        params.heapSize = 64 * ZGranuleSize / 1024;
        params.regionSize = ZGranuleSize / 1024;
        params.exemptionThreshold = 0.8;
        create(params, 0.5);
    }
    return _collected_heap;
}

void ZCollectedHeap::create(const HeapParam& param, double garbageThreshold)
{
    // Arguments are finalized by the caller before constructing the allocator,
    // page table and generations (ZGC zHeap.cpp:60-68).
    static ImmortalWrapper<ZCollectedHeap> collected(param, garbageThreshold);
    _collected_heap = &*collected;
}

ZCollectedHeap::ZCollectedHeap(const HeapParam& param, double garbageThreshold)
    : _initializer(nullptr),
      _heap(param, garbageThreshold),
      _driver_minor(new ZDriverMinor()),
      _driver_major(new ZDriverMajor()),
      _director(new ZDirector()),
      _stat(new ZStat()),
      _runtime_workers()
{
}

ZCollectedHeap::~ZCollectedHeap() = default;

void ZCollectedHeap::initialize_gc()
{
    ZAbort::reset();
    ZStat::Initialize();
    ZStatMutatorAllocRate::initialize();
    const uint64_t now = TimeUtil::NanoSeconds();
    _heap.young().CycleStats().Initialize(now);
    _heap.old().CycleStats().Initialize(now);
    initialize_gc_workers();
    _finalizer_processor.Start();
    StringDedup::Instance().Start();
    if (Uncommitter::Enabled()) {
        LOG(RTLOG_INFO, "Uncommit: Enabled delay=%zus",
            static_cast<size_t>(Uncommitter::DelayNs() / SECOND_TO_NANO_SECOND));
    } else {
        LOG(RTLOG_INFO, "Uncommit: Disabled");
    }
}

void ZCollectedHeap::finalize_gc()
{
    MRT_ASSERT(!_finalizer_processor.IsRunning(), "Invalid finalizerProcessor status");
    MRT_ASSERT(!_gc_thread_running.load(std::memory_order_relaxed), "Invalid GC thread status");
}

void ZCollectedHeap::initialize_gc_workers()
{
    if (_heap.young().Workers() == nullptr) {
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
        const size_t maxHeap = _heap.GetMaxCapacity();
        const auto& regions = static_cast<RegionSpace&>(_heap.GetAllocator()).GetRegionManager();
        const size_t regionBytes = regions.GetThreadLocalRegionSize();
        CHECK_DETAIL(regionBytes != 0, "worker region budget must be initialized");
        const size_t heapWorkers = maxHeap / 50 / regionBytes;
        const uint64_t cpus = activeProcessorCount;
        _concurrent_gc_threads = static_cast<int32_t>(std::max<size_t>(1,
            std::min<size_t>((cpus + 3) / 4, heapWorkers)));
        ConcGCThreads = static_cast<uint32_t>(_concurrent_gc_threads);
        ZYoungGCThreads = ConcGCThreads;
        ZOldGCThreads = ConcGCThreads;
        VLOG(REPORT,
             "concurrent gc thread count %d, active processor count %u, affinity detected %d, region bytes %zu",
             _concurrent_gc_threads, activeProcessorCount, affinityDetected, regionBytes);

        _heap.young().InitializeWorkers(_concurrent_gc_threads);
        _heap.old().InitializeWorkers(_concurrent_gc_threads);
    }


}



void ZCollectedHeap::collect(GCReason reason, bool async)
{
    CHECK(reason < GC_REASON_MAX);
    if (!_heap.IsGCEnabled()) return;
    if (reason == GC_REASON_WB_BREAKPOINT) {
        _driver_major->collect(ZDriverRequest(reason, 0, 0));
        return;
    }
    ZDriverPort& port = reason == GC_REASON_YOUNG
        ? _driver_minor->port() : _driver_major->port();
    const ZDriverRequest request(reason, 0, 0);
    if (async) {
        CHECK(!g_gcRequests[reason].IsSyncGC());
        port.send_async(request);
    } else {
        ScopedEnterSaferegion enterSaferegion(false);
        port.send_sync(request);
    }
}

void ZCollectedHeap::stop()
{
    ZAbort::abort();
    ZCollectedHeap* collected = heap();
    if (collected->_finalizer_processor.IsRunning()) {
        collected->_finalizer_processor.Stop();
    }
    if (collected->_gc_thread_running.load(std::memory_order_acquire)) {
        for (ZThread* thread : { static_cast<ZThread*>(collected->_director),
                                 static_cast<ZThread*>(collected->_driver_major),
                                 static_cast<ZThread*>(collected->_driver_minor) }) {
            thread->stop();
        }
        delete collected->_director;
        delete collected->_driver_minor;
        delete collected->_driver_major;
        collected->_director = nullptr;
        collected->_driver_minor = nullptr;
        collected->_driver_major = nullptr;
        Heap::GetHeap().young().StopWorkers();
        Heap::GetHeap().old().StopWorkers();
        collected->_gc_thread_running.store(false, std::memory_order_release);
    }
    if (collected->_stat != nullptr) {
        collected->_stat->stop();
        delete collected->_stat;
        collected->_stat = nullptr;
    }
    StringDedup::Instance().Stop();
    // Native teardown does not destroy this process-lifetime heap. The drivers
    // and generation workers above can no longer submit safepoint work, so join
    // the runtime pool explicitly before runtime services are torn down.
    collected->_runtime_workers.stop();
}

} // namespace MapleRuntime
