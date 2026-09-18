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
#include "Heap/z/zStringDedup.hpp"
#include "Heap/z/zThread.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Base/TimeUtils.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/Mutator.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
ZCollectedHeap* ZCollectedHeap::heap()
{
    // Universe::initialize_heap creates the collector after VM/platform
    // initialization (universe.cpp:962-963, zArguments.cpp:243-245).
    // A DSO constructor here would observe dynamic page-size globals before
    // their initialization, depending on static archive link order.
    static ImmortalWrapper<ZCollectedHeap> collected;
    return &*collected;
}

ZCollectedHeap::ZCollectedHeap()
    : _heap(),
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
    _heap.GetGCStats(ZGenerationId::young).Init();
    _heap.GetGCStats(ZGenerationId::old).Init();
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
        _finalizer_processor.GetReferenceProcessor().set_workers(_heap.old().Workers());
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
}

} // namespace MapleRuntime
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zMark.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zStat.hpp"
#include "Common/BaseObject.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Common/StateWord.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {




namespace {


// zc7fix: is_mark_good fast path may admit plain non-heap slots (g_cjMarkBadMask all-zero on
// uncoloured non-null). Count rejects before IsValidObject/IsMarkedObject.




const char* HandVerdictName(HandVerdict verdict)
{
    switch (verdict) {
        case HandVerdict::Forwarded: return "Forwarded";
        case HandVerdict::ZeroHeader: return "ZeroHeader";
        case HandVerdict::Usable: return "Usable";
    }
    return "Unknown";
}

const char* ToAnswerName(int)
{
    return "table";
}

} // namespace

// F5: when FindToVersion returns null, never silently hand back a dead/zeroed from.
// Legal null (high-live / raw-pin survivor still at from, ghost=0) keeps returning obj.
// Illegal null (D: old tag + ghost already dispelled + from cleared) fails loudly here.
// See reports/REPORT-nullenum.md LEGAL_NULL_SET; reports/REPORT-tagaba.md F5.
// Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94.
// Synchronous root operations consume the generation of their phase context.
// Unlike ZStackWatermark, these closures do not retain frames across relocations.






// loadfc: best-effort detection verdict. Same header-word shape as Barrier.cpp's former staleguard
// judge (StateWord.h:215-228: bits 0-47 TypeInfo, bits 48-49 stateCode; FORWARDED=3). This one
// relaxed read classifies the observed word; it does not establish object lifetime or happens-before.



// loadfc (zBarrier.inline.hpp:327-343): the slow path must produce a verified current version or
// stop the mutator in a controlled, attributable place -- never hand back a structurally dead
// from-address. The [LOADFC] tag is the population-accounting signature.


// Virtual default: this collector type does not implement the method. Always abort;
// body is out-of-line so HeapGcState.h stays free of FormatLog / string payloads.
}
