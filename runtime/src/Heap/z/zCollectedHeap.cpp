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

#include "Base/ImmortalWrapper.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zStat.hpp"
#include "Common/BaseObject.h"
#include "Common/SuspendibleThreadSet.h"
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
        _concurrent_gc_threads = static_cast<int32_t>(ConcGCThreads);
        VLOG(REPORT, "concurrent gc thread count %u, young %u, old %u",
             ConcGCThreads, ZYoungGCThreads, ZOldGCThreads);
        _heap.young().InitializeWorkers(ZYoungGCThreads);
        _heap.old().InitializeWorkers(ZOldGCThreads);
    }


}



// ZGC zCollectedHeap.cpp:137-146. Cangjie allocation sizes are bytes.
uintptr_t ZCollectedHeap::allocate_new_tlab(size_t minSize, size_t requestedSize, size_t* actualSize)
{
    (void)minSize;
    const uintptr_t addr = _heap.alloc_tlab(AlignUp(requestedSize, size_t{8}));
    if (addr != 0) { *actualSize = requestedSize; }
    return addr;
}

void ZCollectedHeap::collect(GCReason reason)
{
    if (!_heap.IsGCEnabled()) return;
    // ZGC zCollectedHeap.cpp:174-205: external causes select the generation
    // budgets here; only the driver decides how to enqueue the request.
    switch (reason) {
        case GC_REASON_YOUNG:
            _driver_minor->collect(ZDriverRequest(reason, ZYoungGCThreads, 0));
            break;
        case GC_REASON_USER:
        case GC_REASON_DCMD_GC_RUN:
        case GC_REASON_FORCE:
        case GC_REASON_WB_BREAKPOINT:
            _driver_major->collect(ZDriverRequest(reason, ZYoungGCThreads, ZOldGCThreads));
            break;
        default:
            CHECK(false);
            break;
    }
}

void ZCollectedHeap::stop()
{
    ZAbort::abort();
    Heap::GetHeap().page_allocator().StopStalledAllocations();
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

namespace MapleRuntime {
// ZGC zCollectedHeap.cpp:339-349. Cangjie stack-watermark publication is
// performed by the mutator suspension handshake; GC workers rendezvous here.
void ZCollectedHeap::safepoint_synchronize_begin()
{
    ZGeneration::young()->synchronize_relocation();
    ZGeneration::old()->synchronize_relocation();
    SuspendibleThreadSet::synchronize();
}

void ZCollectedHeap::safepoint_synchronize_end()
{
    SuspendibleThreadSet::desynchronize();
    ZGeneration::old()->desynchronize_relocation();
    ZGeneration::young()->desynchronize_relocation();
}
} // namespace MapleRuntime
