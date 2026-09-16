// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RELOCATION_SET_INLINE_H
#define MRT_RELOCATION_SET_INLINE_H

#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zGeneration.hpp"

namespace MapleRuntime {
    template<Generation G>
inline void RegionManager::PrepareFromRegionList()
    {
        size_t retainedRegions = 0;
        size_t retainedBytes = 0;
        size_t markQuarantinedRegions = 0;
        size_t markQuarantinedBytes = 0;
        ghostFromRegionList.VisitAllGhostRegions(
            [this, &retainedRegions, &retainedBytes, &markQuarantinedRegions,
             &markQuarantinedBytes](ZPage* region) {
            DLOG(REGION, "visit ghost from region %p@[%#zx, %#zx)", region, region->GetRegionStart(),
                 region->GetRegionEnd());
            // Count ghost garbage retention before dispel (historical GhostRetention metric).
            if (region->IsGhostFromRegion() && region->IsGarbageRegion()) {
                ++retainedRegions;
                retainedBytes += region->GetGhostRegionSize();
            }
            region->DispelGhostFromRegion();
            if (TryTakeGarbageRegionAfterDispel(region)) {
                // mark-epoch gate (OPTION_2): do not publish to dirty tree until major mark ends.
                // Mutator TakeRegion would ClearUnits payload while concurrent mark may still
                // follow plain SATB edges into this range (REPORT-tracewin 16/16).
                size_t bytes = region->GetRegionSize();
                ReclaimRegionToMarkQuarantine(region);
                ++markQuarantinedRegions;
                markQuarantinedBytes += bytes;
            }
        });
        // A7 cost baseline: deferring the dispel by one cycle would hold roughly this much extra,
        // so the number has to be on the table before the change, not after.  LOG rather than
        // VLOG(REPORT) for the same reason as the gcreclaim probe -- REPORT lands in a separate
        // sink and a probe that looks silent has cost this campaign a turn before.
        LOG(RTLOG_ERROR, "[GhostRetention] retained_regions=%zu retained_bytes=%zu", retainedRegions, retainedBytes);
        VLOG(REPORT, "[MarkQuarantine] installed_regions=%zu installed_bytes=%zu held_units=%u",
             markQuarantinedRegions, markQuarantinedBytes, freeRegionManager.GetMarkQuarantineUnitCount());

        // routedest: the walk above retired the whole outgoing route generation —
        // DispelGhostFromRegion clears the ghost bit and sets routeState NORMAL in one
        // statement, after which both product readers fail (RouteRegion soft-nulls on
        // !IsGhostFromRegion, and the `|| IsCompacted()` bypass is false too). Drop the
        // destination holds those routes were keeping alive, before the next generation's
        // destinations are enrolled by the PrepareForwardableRegion walk below.
        //
        // Dropping a hold does not by itself make a region reclaimable: it must still be
        // picked up by a later Assemble / PrepareYoungGarbageCandidates, evacuated and
        // collected. That is why this ordering does not have to be defended against the
        // reclaim schedules that are not phase-driven — the mutator garbage fast path and
        // the finalizer both reach a live region only through TakeReclaimableGarbageRegion,
        // and a held region never reaches garbageRegionList in the first place.

        // ZGeneration::select_relocation_set (zGeneration.cpp:205-225) has
        // already selected this generation. The arena's owner CHECK validates
        // that producer contract; do not silently repair its input here.
        Heap::GetHeap().GetCollector().GetGenerationCycle(
            G == Generation::Young ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD)
            .relocation_set().install_from_regions(fromRegionList);
        fromRegionList.VisitAllRegions([](ZPage* region) {
            DLOG(REGION, "visit from region %p@[%#zx+%zu, %#zx)", region, region->GetRegionStart(),
                 region->is_marked() ? region->live_bytes() : 0, region->GetRegionEnd());
            region->PrepareForwardableRegion<G>();
            // ZGC installs the page and its forwarding record as one relocation-set
            // operation (zRelocationSet.cpp:91-96).  Keep the equivalent invariant
            // at this GC phase boundary: a from-region must not become visible to
            // either major PostTrace or minor evacuation without its page carrier.
            CHECK_DETAIL(region->HasFromPageMetadata(),
                         "from-page carrier missing after prepare region=%p generation=%u",
                         region, static_cast<unsigned>(G));
        });

        fromRegionList.CopyListTo(ghostFromRegionList);
    }

} // namespace MapleRuntime
#endif
