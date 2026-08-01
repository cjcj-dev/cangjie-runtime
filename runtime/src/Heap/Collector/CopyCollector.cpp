// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CopyCollector.h"

#include "Allocator/RegionSpace.h"
#include "Common/Runtime.h"
#include "Heap/FixEdgeSet.h"
#include "Heap/ForwardFactTable.h"
#include "Heap/RelocationDiagnosticTable.h"
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
    TypeInfo* typeInfo = fromObj.GetTypeInfo();
    CHECK_E(memmove_s(reinterpret_cast<void*>(to), size, reinterpret_cast<void*>(from), size) != EOK, "memmove_s fail");
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanFixShadow(reinterpret_cast<void*>(from), reinterpret_cast<void*>(to), size);
#endif
    // Every object mover funnels through CopyObject. Move the holder-relative
    // fix-edge carrier only after the payload is committed; the destination
    // range simultaneously invalidates carriers belonging to overwritten data.
    FixEdgeSet::Instance().RelocateHolder(from, to, size);
    // r1missbucket: independent observation only. In particular, retain the
    // identity compact copies that ForwardFactTable intentionally rejects.
    RelocationDiagnosticTable::Instance().Record(const_cast<BaseObject*>(&fromObj), &toObj, size, typeInfo);
    // R2.1: copy-time fact — both addresses in hand, payload committed. All
    // producers (ForwardObjectExclusive + CompactRegion×3) funnel here.
    // Aborted paths never reach this point (no half entry).
    ForwardFactTable::Instance().Record(const_cast<BaseObject*>(&fromObj), &toObj, size);
}

void CopyCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    ScopedEntryTrace trace("CJRT_GC_START");
    // prevent other threads stop-the-world during GC.
    // this may be removed in the future.
    ScopedSTWLock stwLock;
    // ScopedStopTheWorld stw;

    gcReason = reason;
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    PreGarbageCollection(!IsYoungGCReason(reason));
#else
    PreGarbageCollection(reason != GC_REASON_YOUNG);
#endif
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
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    if (!IsYoungGCReason(reason)) {
#else
    if (reason != GC_REASON_YOUNG) {
#endif
        UpdateGCStats();
    }
    uint64_t gcTimeNs = gcStats.gcEndTime - gcStats.gcStartTime;
    ScheduleTraceEvent(TRACE_EV_GC_DONE, -1, nullptr, 0);
    double rate = (static_cast<double>(gcStats.collectedBytes) / gcTimeNs) * (static_cast<double>(NS_PER_S) / MB);
    VLOG(REPORT, "total gc time: %s us, collection rate %.3lf MB/s\n", Pretty(gcTimeNs / NS_PER_US).Str(), rate);
    g_gcCount++;
    g_gcTotalTimeUs += (gcTimeNs / NS_PER_US);
    g_gcCollectedTotalBytes += gcStats.collectedBytes;
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    if (!IsYoungGCReason(reason)) {
#else
    if (reason != GC_REASON_YOUNG) {
#endif
        gcStats.collectionRate = rate;
    }
}

void CopyCollector::ForwardFromSpace()
{
    ScopedEntryTrace trace("CJRT_GC_FORWARD");
    TransitionToGCPhase(GCPhase::GC_PHASE_FORWARD, true);

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    GCStats& stats = GetGCStats();
    stats.liveBytesBeforeGC = space.AllocatedBytes();
    stats.fromSpaceSize = space.FromSpaceSize();
    space.ForwardFromSpace(GetThreadPool());
}

void CopyCollector::RefineFromSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    stats.smallGarbageSize = space.RefineFromSpace();
}
} // namespace MapleRuntime
