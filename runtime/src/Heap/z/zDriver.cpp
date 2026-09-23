// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zBreakpoint.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#if defined(__linux__) || defined(hongmeng)
#include <sched.h>
#endif
#include <thread>

#include "Base/SysCall.h"
#include "CangjieRuntime.h"
#include "Heap/z/zMark.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Common/Runtime.h"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Common/RunType.h"
#include "Common/ScopedObjectAccess.h"
#include "LoaderManager.h"
#include "Mutator/MutatorManager.h"



namespace MapleRuntime {

static const ZStatPhaseCollection ZPhaseCollectionMajor("Major Collection", false);
static const ZStatPhaseCollection ZPhaseCollectionMinor("Minor Collection", true);
ZLock* ZDriver::_lock;
ZDriverMinor* ZDriver::_minor;
ZDriverMajor* ZDriver::_major;

static bool ShouldPrecleanYoung(GCReason reason);
static bool ShouldClearAllSoftReferences(GCReason reason);

void ZDriver::initialize() { _lock = new ZLock(); }

void ZDriver::lock() { _lock->lock(); }

void ZDriver::unlock() { _lock->unlock(); }

void ZDriver::set_minor(ZDriverMinor* minor) { _minor = minor; }

void ZDriver::set_major(ZDriverMajor* major) { _major = major; }

ZDriverMinor* ZDriver::minor() { return _minor; }

ZDriverMajor* ZDriver::major() { return _major; }

ZDriverMinor::ZDriverMinor() : ZDriver()
{
    ZDriver::set_minor(this);
    set_name("ZDriverMinor");
    create_and_start();
}

ZDriverMajor::ZDriverMajor() : ZDriver()
{
    ZDriver::set_major(this);
    set_name("ZDriverMajor");
    create_and_start();
}

extern "C" uintptr_t MRT_StopGCWork()
{
    Heap::GetHeap().StopGCWork();
    return 0;
}

// zDriver.cpp:118-127,319-328: each driver names itself and starts in its
// constructor; run_thread is the request loop (zDriver.cpp:201-225,463-488)
// and terminate closes the port so the loop's receive returns (:227-231).
ZDriver::ZDriver() : _gc_cause(GC_REASON_INVALID) {}

void ZDriver::set_gc_cause(GCReason cause) { _gc_cause = cause; }
GCReason ZDriver::gc_cause() const { return _gc_cause; }

void ZDriverMinor::run_thread()
{
    for (;;) {
        const ZDriverRequest request = _port.receive();
        DriverLocker locker;
        abortpoint();
        gc(request);
        abortpoint();
        _port.ack();
        handle_alloc_stalls();
        ZDirector::evaluate_rules();
    }
}

void ZDriverMajor::run_thread()
{
    for (;;) {
        const ZDriverRequest request = _port.receive();
        DriverLocker locker;
        ZBreakpoint::AtBeforeGC();
        abortpoint();
        gc(request);
        abortpoint();
        _port.ack();
        handle_alloc_stalls();
        ZBreakpoint::AtAfterGC();
    }
}

// ZGC zDriver.cpp:193-198,454-460: generation-specific stall ownership.
void ZDriverMinor::handle_alloc_stalls() const
{
    Heap::GetHeap().page_allocator().HandleAllocStallingForYoung();
}

void ZDriverMajor::handle_alloc_stalls() const
{
    Heap::GetHeap().page_allocator().HandleAllocStallingForOld(Heap::GetHeap().GetFinalizerProcessor()
        .GetReferenceProcessor().uses_clear_all_soft_reference_policy());
}

void ZDriverMinor::terminate()
{
    _port.send_async(ZDriverRequest(GC_REASON_INVALID, 0, 0));
}

void ZDriverMajor::terminate()
{
    _port.send_async(ZDriverRequest(GC_REASON_INVALID, 0, 0));
}

bool ZDriverMinor::is_busy() const
{
    return _port.is_busy();
}

bool ZDriverMajor::is_busy() const
{
    return _port.is_busy();
}

void ZDriverMinor::collect(const ZDriverRequest& request)
{
    switch (request.cause()) {
        case GC_REASON_YOUNG:
            _port.send_sync(request);
            break;
        case GC_REASON_TIMER:
        case GC_REASON_ALLOCATION_RATE:
        case GC_REASON_ALLOCATION_STALL:
        case GC_REASON_HIGH_USAGE:
            _port.send_async(request);
            break;
        default:
            CHECK(false);
            break;
    }
}

void ZDriverMajor::collect(const ZDriverRequest& request)
{
    switch (request.cause()) {
        case GC_REASON_USER:
        case GC_REASON_DCMD_GC_RUN:
        case GC_REASON_FORCE:
            _port.send_sync(request);
            break;
        case GC_REASON_TIMER:
        case GC_REASON_ALLOCATION_RATE:
        case GC_REASON_PROACTIVE:
        case GC_REASON_WARMUP:
        case GC_REASON_ALLOCATION_STALL:
            _port.send_async(request);
            break;
        case GC_REASON_WB_BREAKPOINT:
            ZBreakpoint::StartGC();
            _port.send_async(request);
            break;
        default:
            CHECK(false);
            break;
    }
}

// ZGC zDriver.cpp:43-57: the driver owns the request cause for the scope.
template<typename DriverT>
class ZGCCauseSetter {
public:
    ZGCCauseSetter(DriverT* driver, GCReason cause) : _driver(driver)
    {
        _driver->set_gc_cause(cause);
    }
    ~ZGCCauseSetter() { _driver->set_gc_cause(GC_REASON_INVALID); }
private:
    DriverT* _driver;
};

class ZDriverScopeMinor {
private:
    GCIdMark _gc_id;
    GCReason _gc_cause;
    ZGCCauseSetter<ZDriverMinor> _gc_cause_setter;
    ZStatTimer _stat_timer;
public:
    explicit ZDriverScopeMinor(const ZDriverRequest& request)
        : _gc_id(), _gc_cause(request.cause()),
          _gc_cause_setter(ZDriver::minor(), _gc_cause),
          _stat_timer(ZPhaseCollectionMinor)
    {
        ZGeneration::young()->Workers()->set_active_workers(request.young_nworkers());
    }
};

class ZDriverScopeMajor {
private:
    GCIdMark _gc_id;
    GCReason _gc_cause;
    ZGCCauseSetter<ZDriverMajor> _gc_cause_setter;
    ZStatTimer _stat_timer;
public:
    explicit ZDriverScopeMajor(const ZDriverRequest& request)
        : _gc_id(), _gc_cause(request.cause()),
          _gc_cause_setter(ZDriver::major(), _gc_cause),
          _stat_timer(ZPhaseCollectionMajor)
    {
        ZGeneration::young()->Workers()->set_active_workers(request.young_nworkers());
        ZGeneration::old()->Workers()->set_active_workers(request.old_nworkers());
        Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor()
            .set_soft_reference_policy(ShouldClearAllSoftReferences(request.cause()));
    }
};

void ZDriverMinor::gc(const ZDriverRequest& request)
{
    ZDriverScopeMinor scope(request);
    ZGCIdMinor minor_id(GCIdMark::Current());
    ZGeneration::young()->collect(ZYoungType::minor);
}

void ZDriverMajor::collect_young(const ZDriverRequest& request)
{
    ZGCIdMajor major_id(GCIdMark::Current(), 'Y');
    if (ShouldPrecleanYoung(request.cause())) {
        ZGeneration::young()->collect(ZYoungType::major_full_preclean);
        abortpoint();
        ZGeneration::young()->collect(ZYoungType::major_full_roots);
    } else {
        ZGeneration::young()->collect(ZYoungType::major_partial_roots);
    }
    abortpoint();
    Heap::GetHeap().page_allocator().HandleAllocStallingForYoung();
}

void ZDriverMajor::collect_old()
{
    ZGCIdMajor major_id(GCIdMark::Current(), 'O');
    ZGeneration::old()->collect();
}

void ZDriverMajor::gc(const ZDriverRequest& request)
{
    ZDriverScopeMajor scope(request);
    collect_young(request);
    abortpoint();
    collect_old();
}

static bool ShouldClearAllSoftReferences(GCReason reason)
{
    // ZGC zDriver.cpp:232-268: stall and explicit full collections clear soft refs.
    switch (reason) {
        case GC_REASON_FORCE:
        case GC_REASON_ALLOCATION_STALL:
            return true;
        case GC_REASON_USER:
        case GC_REASON_DCMD_GC_RUN:
        case GC_REASON_TIMER:
        case GC_REASON_ALLOCATION_RATE:
        case GC_REASON_PROACTIVE:
        case GC_REASON_WARMUP:
        case GC_REASON_WB_BREAKPOINT:
            break;
        default:
            CHECK(false);
    }
    const auto& manager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    return manager.IsAllocationStallingForOld();
}

static bool ShouldPrecleanYoung(GCReason reason)
{
    // ZGC zDriver.cpp:270-299: explicit full collections, including breakpoints.
    switch (reason) {
        case GC_REASON_USER:
        case GC_REASON_DCMD_GC_RUN:
        case GC_REASON_FORCE:
        case GC_REASON_WB_BREAKPOINT:
        case GC_REASON_ALLOCATION_STALL:
            return true;
        case GC_REASON_TIMER:
        case GC_REASON_ALLOCATION_RATE:
        case GC_REASON_PROACTIVE:
        case GC_REASON_WARMUP:
            break;
        default:
            CHECK(false);
    }
    // ZGC zDriver.cpp:294-299: requests that have seen young but not old.
    const auto& manager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    if (manager.IsAllocationStallingForOld()) {
        return true;
    }

    // ZGC zDriver.cpp:301-312: clearing all soft references is the last
    // attempt before OOM, so it must also preclean young. The driver locker
    // keeps allocation stalls stable between the two policy decisions.
    MRT_ASSERT(!ShouldClearAllSoftReferences(reason),
               "Clearing all soft references without pre-cleaning young gen");
    return false;
}


#ifdef COV_SIGNALHANDLE
extern "C" void __gcov_dump(void);
#endif

bool GCExecutor::Execute(void* owner)
{
    MRT_ASSERT(owner != nullptr, "task queue owner ptr should not be null!");

    switch (taskType) {
        case GCTask::TaskType::TASK_TYPE_TERMINATE_GC: {
            return false;
        }
        case GCTask::TaskType::TASK_TYPE_INVOKE_GC: {
            ZCollectedHeap::heap()->collect(gcReason);
            break;
        }
        case GCTask::TaskType::TASK_TYPE_DUMP_HEAP: {
            CjHeapData* cjHeapData = new CjHeapData();
            if (cjHeapData != nullptr) {
                cjHeapData->DumpHeap();
                delete cjHeapData;
            } else {
                LOG(RTLOG_ERROR, "cjHeapData Init Failed");
            }
#ifdef COV_SIGNALHANDLE
            __gcov_dump();
#endif
            break;
        }
        case GCTask::TaskType::TASK_TYPE_DUMP_HEAP_IDE: {
#if defined(__OHOS__) && (__OHOS__ == 1)
            CjHeapDataForIDE* heapSnapshotJSONSerializer = new CjHeapDataForIDE();
            if (heapSnapshotJSONSerializer != nullptr) {
                heapSnapshotJSONSerializer->Serialize();
                delete heapSnapshotJSONSerializer;
            } else {
                LOG(RTLOG_ERROR, "heapSnapshotJSONSerializer Init Failed");
            }
            break;
#endif
        }

        case GCTask::TaskType::TASK_TYPE_DUMP_HEAP_OOM: {
            CjHeapData* cjHeapData = new CjHeapData(true);
            if (cjHeapData != nullptr) {
                cjHeapData->DumpHeap();
                delete cjHeapData;
            } else {
                LOG(RTLOG_ERROR, "cjHeapData Init Failed");
            }
#ifdef COV_SIGNALHANDLE
            __gcov_dump();
#endif
            break;
        }
        default:
            LOG(RTLOG_ERROR, "[GC] Error task type: %u ignored!", static_cast<uint32_t>(taskType));
            break;
    }
    return true;
}
}
