// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CopyCollector.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Base/GcLog.h"
#include "Heap/z/zStat.hpp"
#include "Allocator/RegionSpace.h"
#include "Heap/z/zDirector.hpp"
#include "Common/Runtime.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#include "schedule.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
void CopyCollector::PostGarbageCollection(GCCycleGeneration generation, uint64_t gcIndex)
{
    reinterpret_cast<RegionSpace&>(theAllocator).DumpRegionStats("region statistics when gc ends");
    TracingCollector::PostGarbageCollection(generation, gcIndex);
    MutatorManager::Instance().DestroyExpiredMutators();
}

void CopyCollector::ForwardFromSpace(GCCycleGeneration generation)
{
    ScopedEntryTrace trace("CJRT_GC_FORWARD");
    TransitionToGCPhase(GCPhase::GC_PHASE_FORWARD, true, generation == GCCycleGeneration::YOUNG);

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    GCStats& stats = GetGCStats(generation);
    stats.liveBytesBeforeGC = space.AllocatedBytes();
    stats.fromSpaceSize = space.FromSpaceSize();
    if (generation == GCCycleGeneration::YOUNG) {
        space.ForwardFromSpace<Generation::Young>(GetWorkers(GCCycleGeneration::YOUNG));
    } else {
        space.ForwardFromSpace<Generation::Old>(GetWorkers(GCCycleGeneration::OLD));
    }

}

void CopyCollector::RefineFromSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    stats.smallGarbageSize = space.RefineFromSpace();
}
} // namespace MapleRuntime
