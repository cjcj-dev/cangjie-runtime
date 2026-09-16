// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/WCollector/WCollector.h"
#include "Heap/Allocator/RegionList.h"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRelocationSet.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

#include "Concurrency/Concurrency.h"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
#include "Heap/Collector/ExportOwnershipTestObservations.h"

void WCollector::PostTrace()
{
    MRT_PHASE_TIMER(ZStatPhases::PPostTrace);
    TransitionToGCPhase(GC_PHASE_POST_TRACE, true);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    space.GetRegionManager().HandleTraceRegions();
    // Value-only cycle roots still depend on the preceding relocation receipts.
    // Complete their owner handoff while that authority is queryable.
    // zGeneration.cpp:1261 mark_end does not reset forwarding.
#if defined(MRT_TESTABLE_INTERNALS)
    ObserveExportOwnershipForTest(false);
#endif
    PrepareCycleRef();
#if defined(MRT_TESTABLE_INTERNALS)
    ObserveExportOwnershipForTest(true);
#endif
    // ZGeneration::select_relocation_set (zGeneration.cpp:205-225): selection
    // consumes the completed mark, and only this generation's relocatable pages.
    space.AssembleGarbageCandidates();
    // reclaim large objects immediately after tracing is done.
    CollectLargeGarbage();
    CollectPinnedGarbage();
    RefineFromSpace();
    // zGeneration.cpp:1042 / :1131-1133: old resets its own previous set
    // after non-strong processing and before select. Young tables stay
    // until young's ResetRelocationSet.
    Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD).reset_relocation_set();
    // ZGenerationOld::collect (zGeneration.cpp:1044): stop after reset,
    // before selecting the next relocation set.
    if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
        return;
    }
    fwdTable.PrepareForwardTable<Generation::Old>();
    // OPTION_2 mark-epoch release: TRACE+CLEAR_SATB done; publish quarantined post-dispel
    // units (from this PrepareForwardTable and any prior minor) to dirty for reuse.
    // INV-1 closed: concurrent mark can no longer follow plain edges into these ranges.
    space.GetRegionManager().ReleaseMarkQuarantine();
}
void WCollector::CollectSmallSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    {
        MRT_PHASE_TIMER(ZStatPhases::PCollectFromSpaceGarbage);
        stats.collectedBytes += stats.smallGarbageSize;
        space.CollectFromSpaceGarbage();
    }

    size_t candidateBytes = stats.fromSpaceSize + stats.pinnedSpaceSize + stats.largeSpaceSize;
    stats.garbageRatio = (candidateBytes > 0) ? static_cast<float>(stats.collectedBytes) / candidateBytes : 0;

    stats.liveBytesAfterGC = space.AllocatedBytes();

    VLOG(REPORT,
         "collect %zu B: old small %zu - %zu B, old pinned %zu - %zu B, old large %zu - %zu B. garbage ratio %.2f%%",
         stats.collectedBytes, stats.fromSpaceSize, stats.smallGarbageSize, stats.pinnedSpaceSize,
         stats.pinnedGarbageSize, stats.largeSpaceSize, stats.largeGarbageSize,
         stats.garbageRatio * 100); // The base of the percentage is 100

    VLOG(REPORT, "start to release heap garbage memory");
#if defined(__EULER__)
    Heap::GetHeap().GetAllocator().TryReclaimGarbageMemory();
#endif
    collectorResources.GetFinalizerProcessor().NotifyToReclaimGarbage();
}
} // namespace MapleRuntime

namespace MapleRuntime {
void RegionManager::AddFlipPromotedPage(ZPage* region)
{
    auto original = region->CloneForPromotion();
    std::lock_guard<std::mutex> lock(flipPromotedMutex);
    flipPromotedPages.push_back(std::move(original));
}


// ZRelocationSet::reset: the original young pages are released with the
// previous set, after its forwarding entries and remset scan consumers.
void RegionManager::ResetFlipPromotedPages()
{
    std::lock_guard<std::mutex> lock(flipPromotedMutex);
    flipPromotedPages.clear();
}

ZRelocationSet::ZRelocationSet(ZGeneration* generation)
    : _generation(generation),
      _allocator(),
      _forwardings(nullptr),
      _nforwardings(0)
{
}

ZWorkers* ZRelocationSet::workers() const { return _generation != nullptr ? _generation->Workers() : nullptr; }

void ZRelocationSet::install(const ZRelocationSetSelector* selector) { (void)selector; }

void ZRelocationSet::install_from_regions(RegionList& regions)
{
    size_t n = 0;
    size_t budget = 0;
    regions.VisitAllRegions([&](ZPage* region) {
        ++n;
        const size_t live = region->is_marked() ? region->live_objects() : (region->GetRegionSize() >> 3);
        const size_t entries = ZForwarding::nentries(live);
        size_t bytes = 0;
        (void)ZForwarding::AttachedArray::allocation_size(entries, &bytes);
        (void)ZForwardingAllocator::add_to_budget(bytes, &budget);
    });
    budget += n * sizeof(ZForwarding*);
    _allocator.reset(budget);
    _forwardings = static_cast<ZForwarding**>(_allocator.alloc(n * sizeof(ZForwarding*)));
    _nforwardings = 0;
    regions.VisitAllRegions([&](ZPage* region) {
        const size_t live = region->is_marked() ? region->live_objects() : (region->GetRegionSize() >> 3);
        ZForwarding* forwarding = ZForwarding::alloc(live, region->GetRegionStart(), ZAddressHeapBase,
            region->GetRegionSize(), region, region->GetRegionLifeId(), &_allocator);
        _generation->forwarding_table().insert(forwarding);
        _forwardings[_nforwardings++] = forwarding;
    });
}

void ZRelocationSet::reset(ZPageAllocator* page_allocator)
{
    (void)page_allocator;
    ZRelocationSetIterator iter(this);
    for (ZForwarding* forwarding; iter.next(&forwarding);) {
        forwarding->~ZForwarding();
    }
    _nforwardings = 0;
    _forwardings = nullptr;
}

void ZRelocationSet::register_flip_promoted(const ZArray<ZPage*>& pages)
{
    std::lock_guard<std::mutex> locker(_promotion_lock);
    for (int i = 0; i < pages.length(); ++i) {
        _flip_promoted_pages.push(pages.at(i));
    }
}

void ZRelocationSet::register_relocate_promoted(const ZArray<ZPage*>& pages)
{
    std::lock_guard<std::mutex> locker(_promotion_lock);
    for (int i = 0; i < pages.length(); ++i) {
        _relocate_promoted_pages.push(pages.at(i));
    }
}

void ZRelocationSet::register_in_place_relocate_promoted(ZPage* page)
{
    std::lock_guard<std::mutex> locker(_promotion_lock);
    _in_place_relocate_promoted_pages.push(page);
}

void GenerationCycle::reset_relocation_set()
{
    ZRelocationSetIterator iter(&_relocation_set);
    for (ZForwarding* forwarding; iter.next(&forwarding);) {
        _forwarding_table.remove(forwarding);
    }
    _relocation_set.reset(nullptr);
}

} // namespace MapleRuntime
