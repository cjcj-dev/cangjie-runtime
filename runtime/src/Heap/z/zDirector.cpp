
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zDriver.hpp"

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
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Common/Runtime.h"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Common/RunType.h"
#include "Common/ScopedObjectAccess.h"
#include "LoaderManager.h"
#include "Mutator/MutatorManager.h"


namespace MapleRuntime {
// zDirector.cpp:73-80: the director names itself and starts in its
// constructor; run_thread (:916-930) is the sampling loop and terminate
// (:932-936) sets the stop flag under the monitor and notifies.
ZDirector::ZDirector(CollectorResources& resources) : resources(resources)
{
    set_name("ZDirector");
    create_and_start();
}

void ZDirector::run_thread()
{
    resources.RunDirectorLoop();
}

void ZDirector::terminate()
{
    std::lock_guard<std::mutex> locker(resources.directorMutex);
    resources.directorStopped = true;
    resources.directorCondition.notify_all();
}
} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zDriver.hpp"

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
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Common/Runtime.h"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Common/RunType.h"
#include "Common/ScopedObjectAccess.h"
#include "LoaderManager.h"
#include "Mutator/MutatorManager.h"


namespace MapleRuntime {
void CollectorResources::RunDirectorLoop()
{
    GcMetronome metronome(TimeUtil::NanoSeconds());
    std::unique_lock<std::mutex> lock(directorMutex);
    while (!directorStopped) {
        const uint64_t now = TimeUtil::NanoSeconds();
        const bool tick = metronome.Poll(now);
        if (tick || directorReevaluate) {
            directorReevaluate = false;
            EvaluateDirector(now);
            continue;
        }
        directorCondition.wait_for(lock, std::chrono::nanoseconds(metronome.DeadlineNs() - now),
                                   [this] { return directorStopped || directorReevaluate; });
    }
}

void CollectorResources::EvaluateDirector(uint64_t now)
{
    // zDirector.cpp:875-927: one synchronized snapshot feeds all rules and
    // active-worker adjustment. No driver lifecycle lock is acquired here.
    if (Runtime::CurrentRef() == nullptr || !IsGCActive()) {
        return;
    }
    auto& regions = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    GcTriggerInputs in = ZStat::SampleDirectorStats(now,
        collectorProxy.GetZGeneration(ZGenerationId::young).CycleStats(),
        collectorProxy.GetZGeneration(ZGenerationId::old).CycleStats(), regions,
        GetWorkers(ZGenerationId::young), GetWorkers(ZGenerationId::old),
        static_cast<uint32_t>(concurrentGcThreadCount));
    in.minorBusy = minorBusy || minorDriverPort.Pending() != 0;
    in.majorBusy = majorBusy || majorDriverPort.Pending() != 0;
    const GcTriggerDecision decision = DecideGcTrigger(in);
    const GcWorkerSelection selection = SelectGcWorkers(in, in.workerCapacity,
        in.lastYoungWorkers, decision.kind == GcTriggerKind::MAJOR);
    if (decision.kind != GcTriggerKind::NONE) {
        g_gcTriggerYoungWorkers.store(selection.youngWorkers, std::memory_order_relaxed);
        g_gcTriggerOldWorkers.store(selection.oldWorkers, std::memory_order_relaxed);
        NoteGcTriggerRule(decision.rule);
        if (decision.kind == GcTriggerKind::MAJOR) {
            const GCReason reason = decision.rule == GcTriggerRule::TIMER ? GC_REASON_BACKUP : GC_REASON_HEU;
            majorDriverPort.EnqueueAsync(reason, selection.youngWorkers, selection.oldWorkers,
                                         decision.rule == GcTriggerRule::WARMUP);
        } else {
            minorDriverPort.EnqueueAsync(GC_REASON_YOUNG, selection.youngWorkers);
            if (in.oldWorkersActive && in.activeOldWorkers != selection.oldWorkers) {
                GetWorkers(ZGenerationId::old).request_resize_workers(selection.oldWorkers);
            }
        }
        return;
    }
    // zDirector.cpp:725-780: only an active young collection provides the
    // pressure signal for live resizing; the existing worker task consumes it.
    if (in.youngWorkersActive) {
        GcTriggerInputs hard = in;
        hard.softMaxBytes = in.capacityBytes;
        const auto request = RuleDynamicAllocRate(hard, in.workerCapacity,
            in.lastYoungWorkers, false);
        if (!request.trigger) {
            return;
        }
        uint32_t desired = std::max(request.workers, in.activeYoungWorkers);
        desired = std::min(in.workerCapacity, in.activeYoungWorkers + 2 * (desired - in.activeYoungWorkers));
        const auto adjusted = SelectWorkerThreads(in, desired, in.workerCapacity, in.oldWorkersActive);
        if (in.oldWorkersActive && in.activeOldWorkers != adjusted.oldWorkers) {
            GetWorkers(ZGenerationId::old).request_resize_workers(adjusted.oldWorkers);
        }
        if (in.activeYoungWorkers != adjusted.youngWorkers) {
            GetWorkers(ZGenerationId::young).request_resize_workers(adjusted.youngWorkers);
        }
    }
}


} // namespace MapleRuntime
