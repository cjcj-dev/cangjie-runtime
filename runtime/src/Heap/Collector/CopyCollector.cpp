// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CopyCollector.h"

#include <cstdlib>
#include <cstring>

#include "Base/GcLog.h"
#include "Allocator/RegionSpace.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/HealPairDiag.h"
#include "Common/Runtime.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/SatbBuffer.h"
#include "ObjectModel/RefField.inline.h"
#include "schedule.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
void CopyCollector::PostGarbageCollection(uint64_t gcIndex)
{
    reinterpret_cast<RegionSpace&>(theAllocator).DumpRegionStats("region statistics when gc ends");
    TracingCollector::PostGarbageCollection(gcIndex);
    MutatorManager::Instance().DestroyExpiredMutators();
}

void CopyCollector::CopyObject(const BaseObject& fromObj, BaseObject& toObj, size_t size) const
{
    uintptr_t from = reinterpret_cast<uintptr_t>(&fromObj);
    uintptr_t to = reinterpret_cast<uintptr_t>(&toObj);
    const bool stall = HealPairDiag::Enabled() && HealPairDiag::MidCopyStallNs() > 0 && size > 8;
    if (HealPairDiag::Enabled()) {
        HealPairDiag::NoteCopy(reinterpret_cast<const void*>(from), reinterpret_cast<const void*>(to), size, 0);
    }
    if (stall) {
        // Tip first so WaitRoutedTipReady / relocate_or_remap can admit the to-address
        // while high offsets are still the reservation zero. Default-off (COPYSTALL_NS).
        CHECK_E(memmove_s(reinterpret_cast<void*>(to), 8, reinterpret_cast<void*>(from), 8) != EOK, "memmove_s fail");
        HealPairDiag::MaybeMidCopyStall(size);
        CHECK_E(memmove_s(reinterpret_cast<void*>(to + 8), size - 8, reinterpret_cast<void*>(from + 8), size - 8) !=
                    EOK,
                "memmove_s fail");
    } else {
        CHECK_E(memmove_s(reinterpret_cast<void*>(to), size, reinterpret_cast<void*>(from), size) != EOK,
                "memmove_s fail");
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanFixShadow(reinterpret_cast<void*>(from), reinterpret_cast<void*>(to), size);
#endif
    if (HealPairDiag::Enabled()) {
        HealPairDiag::NoteCopy(reinterpret_cast<const void*>(from), reinterpret_cast<const void*>(to), size, 1);
    }
}

void CopyCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    ScopedEntryTrace trace("CJRT_GC_START");
    // prevent other threads stop-the-world during GC.
    // this may be removed in the future.
    ScopedSTWLock stwLock;
    // ScopedStopTheWorld stw;

    gcReason = reason;
    PreGarbageCollection(reason != GC_REASON_YOUNG);
    ScheduleTraceEvent(TRACE_EV_GC_START, -1, nullptr, 0);
    VLOG(REPORT, "[GC] Start %s %s gcIndex= %lu", GetCollectorName(), g_gcRequests[gcReason].name, gcIndex);
    GCStats& gcStats = GetGCStats();
    gcStats.collectedBytes = 0;
    gcStats.gcStartTime = TimeUtil::NanoSeconds();

    DoGarbageCollection();

    if (reason == GC_REASON_OOM) {
        Heap::GetHeap().GetAllocator().ReclaimGarbageMemory(true);
    }

    PostGarbageCollection(gcIndex);
    gcStats.gcEndTime = TimeUtil::NanoSeconds();
    // Emitted here rather than from GCStats::Dump, because UpdateGCStats below (and so Dump) is
    // skipped for young collections: a minor would produce no cycle record and its phases would
    // be attributed to the next major.
    GcLog::Cycle(GcLog::CompleteCycle(), reason == GC_REASON_YOUNG ? "minor" : "major",
                 g_gcRequests[reason].name, gcStats.gcStartTime, gcStats.gcEndTime - gcStats.gcStartTime,
                 gcStats.liveBytesBeforeGC, gcStats.liveBytesAfterGC, gcStats.collectedBytes,
                 Heap::GetHeap().GetUsedPageSize(), gcStats.GetThreshold());
    if (reason != GC_REASON_YOUNG) {
        UpdateGCStats();
    }
    uint64_t gcTimeNs = gcStats.gcEndTime - gcStats.gcStartTime;
    ScheduleTraceEvent(TRACE_EV_GC_DONE, -1, nullptr, 0);
    double rate = (static_cast<double>(gcStats.collectedBytes) / gcTimeNs) * (static_cast<double>(NS_PER_S) / MB);
    VLOG(REPORT, "total gc time: %s us, collection rate %.3lf MB/s\n", Pretty(gcTimeNs / NS_PER_US).Str(), rate);
    g_gcCount.fetch_add(1, std::memory_order_release);
    g_gcTotalTimeUs.fetch_add(gcTimeNs / NS_PER_US, std::memory_order_release);
    g_gcCollectedTotalBytes.fetch_add(gcStats.collectedBytes, std::memory_order_release);
    gcStats.collectionRate = rate;
    if (reason != GC_REASON_YOUNG) {
        GCStats::SetPrevGCFinishTime(TimeUtil::NanoSeconds());
    }
    collectorResources.NotifyGCFinished(gcIndex);
}

void CopyCollector::ForwardFromSpace()
{
    ScopedEntryTrace trace("CJRT_GC_FORWARD");
    TransitionToGCPhase(GCPhase::GC_PHASE_FORWARD, true);

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    GCStats& stats = GetGCStats();
    stats.liveBytesBeforeGC = space.AllocatedBytes();
    stats.fromSpaceSize = space.FromSpaceSize();
    GarbRegionDiag::CensusBeforeForward("pre-forward");
    GCThreadPool* copyPool = GetThreadPool();
    const char* poolKind = "shared";
    if (gcReason == GC_REASON_YOUNG) {
        const char* forceSerialEnv = std::getenv("MRT_GCV2_EVACPAR_FORCE_SERIAL");
        const bool forceSerial =
            forceSerialEnv != nullptr && std::strcmp(forceSerialEnv, "1") == 0;
        GCThreadPool* evacuationPool = collectorResources.GetEvacuationThreadPool();
        if (forceSerial) {
            copyPool = nullptr;
            poolKind = "serial";
        } else if (evacuationPool != nullptr) {
            copyPool = evacuationPool;
            poolKind = "dedicated";
        }
        VLOG(REPORT,
             "[GCV2][evacpar][copy] parallel=%u workers=%d pool=%s forceSerial=%u",
             static_cast<unsigned>(copyPool != nullptr),
             copyPool == nullptr ? 1 : copyPool->GetMaxThreadNum() + 1, poolKind,
             static_cast<unsigned>(forceSerial));
    }
    space.ForwardFromSpace(copyPool);
}

void CopyCollector::RefineFromSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    stats.smallGarbageSize = space.RefineFromSpace();
}
} // namespace MapleRuntime
