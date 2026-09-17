// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zPageAllocator.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/Collector/CopyCollector.h"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"
#include "Heap/z/zRelocationSetSelector.inline.hpp"
#include "Heap/z/zForwarding.inline.hpp"

namespace MapleRuntime {

ZRelocationSetSelectorGroupStats::ZRelocationSetSelectorGroupStats() = default;

ZRelocationSetSelectorGroup::ZRelocationSetSelectorGroup(const char* name, ZPageType page_type, size_t max_page_size,
                                                         size_t object_size_limit, double fragmentation_limit)
    : _name(name),
      _page_type(page_type),
      _max_page_size(max_page_size),
      _object_size_limit(object_size_limit),
      _fragmentation_limit(fragmentation_limit),
      _page_fragmentation_limit(static_cast<size_t>(max_page_size * (fragmentation_limit / 100.0))),
      _live_pages(),
      _not_selected_pages(),
      _forwarding_entries(0),
      _stats()
{}

bool ZRelocationSetSelectorGroup::is_disabled()
{
    return _page_type == ZPageType::medium && !ZPageSizeMediumEnabled;
}

bool ZRelocationSetSelectorGroup::is_selectable()
{
    return _page_type != ZPageType::large;
}

size_t ZRelocationSetSelectorGroup::partition_index(const ZPage* page) const
{
    const size_t partitionSize = page->size() >> NumPartitionsShift;
    if (partitionSize == 0) {
        return 0;
    }
    const size_t index = page->live_bytes() / partitionSize;
    return index < static_cast<size_t>(NumPartitions) ? index : static_cast<size_t>(NumPartitions - 1);
}

void ZRelocationSetSelectorGroup::semi_sort()
{
    int partitions[NumPartitions] = {};
    ZArrayIterator<ZPage*> iter1(&_live_pages);
    for (ZPage* page; iter1.next(&page);) {
        partitions[partition_index(page)]++;
    }
    int finger = 0;
    for (int i = 0; i < NumPartitions; i++) {
        const int slots = partitions[i];
        partitions[i] = finger;
        finger += slots;
    }
    const int npages = _live_pages.length();
    ZArray<ZPage*> sorted_live_pages(npages, npages, nullptr);
    ZArrayIterator<ZPage*> iter2(&_live_pages);
    for (ZPage* page; iter2.next(&page);) {
        const size_t index = partition_index(page);
        const int dest = partitions[index]++;
        sorted_live_pages.at_put(dest, page);
    }
    _live_pages.swap(&sorted_live_pages);
}

void ZRelocationSetSelectorGroup::select_inner()
{
    const int npages = _live_pages.length();
    int selected_from = 0;
    int selected_to = 0;
    size_t selected_forwarding_entries = 0;
    size_t from_live_bytes = 0;
    size_t from_forwarding_entries = 0;
    size_t live_bytes_per_age[kPageAgeCount] = {};
    size_t npages_per_age[kPageAgeCount] = {};
    semi_sort();
    const double denom = static_cast<double>(_max_page_size - _object_size_limit);
    for (int from = 1; from <= npages; from++) {
        ZPage* const page = _live_pages.at(from - 1);
        const size_t page_live_bytes = page->live_bytes();
        from_live_bytes += page_live_bytes;
        from_forwarding_entries += ZForwarding::nentries(page);
        live_bytes_per_age[untype(page->age())] += page_live_bytes;
        npages_per_age[untype(page->age())] += 1;
        const int to = denom <= 0.0 ? from : static_cast<int>(std::ceil(static_cast<double>(from_live_bytes) / denom));
        const int diff_from = from - selected_from;
        const int diff_to = to - selected_to;
        const double percentToOfFrom =
            (diff_from != 0) ? (static_cast<double>(diff_to) / static_cast<double>(diff_from) * 100.0) : 0.0;
        const double diff_reclaimable = 100.0 - percentToOfFrom;
        if (diff_reclaimable > _fragmentation_limit) {
            selected_from = from;
            selected_to = to;
            selected_forwarding_entries = from_forwarding_entries;
        }
    }
    size_t rejected_live_bytes_per_age[kPageAgeCount] = {};
    size_t rejected_npages_per_age[kPageAgeCount] = {};
    for (int i = selected_from; i < _live_pages.length(); i++) {
        ZPage* const page = _live_pages.at(i);
        if (page->is_young()) {
            _not_selected_pages.append(page);
        }
        rejected_live_bytes_per_age[untype(page->age())] += page->live_bytes();
        rejected_npages_per_age[untype(page->age())] += 1;
    }
    _live_pages.trunc_to(selected_from);
    _forwarding_entries = selected_forwarding_entries;
    for (uint32_t i = 0; i < kPageAgeCount; ++i) {
        _stats[i]._relocate = live_bytes_per_age[i] - rejected_live_bytes_per_age[i];
        _stats[i]._npages_selected = npages_per_age[i] - rejected_npages_per_age[i];
    }
    (void)selected_to;
}

void ZRelocationSetSelectorGroup::select()
{
    if (is_disabled()) {
        return;
    }
    if (is_selectable()) {
        select_inner();
    } else {
        const int npages = _live_pages.length();
        for (int from = 1; from <= npages; from++) {
            _not_selected_pages.append(_live_pages.at(from - 1));
        }
    }
}

ZRelocationSetSelector::ZRelocationSetSelector()
    : ZRelocationSetSelector(0.0)
{}

ZRelocationSetSelector::ZRelocationSetSelector(double fragmentation_limit)
    : _small("Small", ZPageType::small, ZPageSizeSmall, ZObjectSizeLimitSmall, fragmentation_limit),
      _medium("Medium", ZPageType::medium, ZPageSizeMediumMax, ZObjectSizeLimitMedium, fragmentation_limit),
      _large("Large", ZPageType::large, 0, 0, fragmentation_limit),
      _empty_pages()
{}

void ZRelocationSetSelector::select()
{
    _large.select();
    _medium.select();
    _small.select();
}

ZRelocationSetSelectorStats ZRelocationSetSelector::stats() const
{
    ZRelocationSetSelectorStats stats;
    for (PageAge age : kPageAgeRangeAll) {
        const uint32_t i = untype(age);
        stats._small[i] = _small.stats(age);
        stats._medium[i] = _medium.stats(age);
        stats._large[i] = _large.stats(age);
    }
    stats._has_relocatable_pages = total() > 0 ? 1 : 0;
    return stats;
}

void RegionManager::ReassembleFromSpace()
{
    fromRegionList.MergeRegionList(unmovableFromRegionList);
}

void RegionManager::CountLiveObject(const BaseObject* obj)
{
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(obj));
    region->inc_live(1, obj->GetSize());
}

namespace {
// ZGenerationPagesIterator + ZPage::is_relocatable (zGeneration.cpp:209-213).
// Existing intrusive allocation lists are the Cangjie page-table adapter.
bool IsOldRelocationCandidate(const ZPage* region)
{
    return region->GetOwnerGeneration() == Generation::Old && region->IsRelocatable();
}
}

void RegionManager::AssembleSmallGarbageCandidates()
{
    // The young collection and old selection share the physical FROM list.
    // Establish the old selector's input here, before cleanup or forwarding.
    ZPage* region = fromRegionList.GetHeadRegion();
    while (region != nullptr) {
        ZPage* next = region->GetNextRegion();
        if (!IsOldRelocationCandidate(region)) {
            fromRegionList.DeleteRegion(region);
            ParkUnmovableFromRegion(region);
        }
        region = next;
    }
    auto select = [this](RegionList& list, bool recent) {
        ZPage* region = list.GetHeadRegion();
        while (region != nullptr) {
            ZPage* next = region->GetNextRegion();
            if (IsOldRelocationCandidate(region) && !region->IsNotRelocatableThisCycle()) {
                list.DeleteRegion(region);
                if (recent) RecentFullAccounting::Dequeue(1, region->GetUnitCount());
                fromRegionList.PrependRegion(region);
            }
            region = next;
        }
    };
    select(rawPointerPinnedRegionList, false);
    select(recentFullRegionList, true);
    select(unmovableFromRegionList, false);
}

void RegionManager::AssembleLargeGarbageCandidates()
{
    ZPage* region = oldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        ZPage* next = region->GetNextRegion();
        if (!IsOldRelocationCandidate(region)) {
            oldLargeRegionList.DeleteRegion(region);
            recentLargeRegionList.PrependRegion(region);
        }
        region = next;
    }
    region = recentLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        ZPage* next = region->GetNextRegion();
        if (IsOldRelocationCandidate(region)) {
            recentLargeRegionList.DeleteRegion(region);
            oldLargeRegionList.PrependRegion(region);
        }
        region = next;
    }
}

void RegionManager::ClearNotRelocatableThisCycleFlags()
{
    auto clearList = [](RegionList& list) {
        (void)list;
    };
    clearList(tlRegionList);
    clearList(recentFullRegionList);
    clearList(unmovableFromRegionList);
    clearList(fromRegionList);
    clearList(recentPinnedRegionList);
    clearList(oldPinnedRegionList);
    clearList(rawPointerPinnedRegionList);
    clearList(recentLargeRegionList);
    clearList(oldLargeRegionList);
    // Region caches may hold stamped regions until HandleTraceRegions merges them.
    clearList(fullTraceRegions);
    clearList(largeTraceRegions);
}

// routedest: drop the destination holds of one route generation. Called from
// PrepareFromRegionList, immediately after the ghost dispel walk and before the next
// generation's destinations are enrolled — placing it there rather than at the three
// PrepareForwardTable call sites is what makes it immune to a missed site, and there are
// three, two of them inside a single minor (WCollector.cpp:5117 and :5570) plus the major
// PostTrace one (:2124).
//
// Walks the same eleven lists as ClearNotRelocatableThisCycleFlags, and reports the gauge
// before clearing: holds that leak never get dropped and show up as monotonic growth in
// held_regions, which is the only way to tell that failure apart from the opposite one.


void RegionManager::AssemblePinnedGarbageCandidates(bool collectAll)
{
    ZPage* region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        ZPage* next = region->GetNextRegion();
        if (!IsOldRelocationCandidate(region)) {
            oldPinnedRegionList.DeleteRegion(region);
            recentPinnedRegionList.PrependRegion(region);
        }
        region = next;
    }
    region = recentPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        ZPage* next = region->GetNextRegion();
        if (IsOldRelocationCandidate(region)) {
            recentPinnedRegionList.DeleteRegion(region);
            oldPinnedRegionList.PrependRegion(region);
        }
        region = next;
    }
    region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        ZPage* next = region->GetNextRegion();
        if (collectAll && region->GetRawPointerObjectCount() > 0) {
            oldPinnedRegionList.DeleteRegion(region);
            rawPointerPinnedRegionList.PrependRegion(region);
        }
        region = next;
    }
}

YoungCollectionStats RegionManager::PrepareYoungGarbageCandidates(const std::function<void(ZPage*)>& visitor)
{
    PublishTLABStatistics();
    YoungCollectionStats stats;
    uint64_t subStart = TimeUtil::NanoSeconds();
    ZPage* oldRegion = fromRegionList.GetHeadRegion();
    while (oldRegion != nullptr) {
        ZPage* next = oldRegion->GetNextRegion();
        ++stats.fromVisited;
        stats.fromVisitedUnits += oldRegion->GetUnitCount();
        fromRegionList.DeleteRegion(oldRegion);
        ParkUnmovableFromRegion(oldRegion);
        oldRegion = next;
    }
    stats.reparkNs = TimeUtil::NanoSeconds() - subStart;

    subStart = TimeUtil::NanoSeconds();
    ZPage* region = unmovableFromRegionList.GetHeadRegion();
    while (region != nullptr) {
        ZPage* next = region->GetNextRegion();
        ++stats.unmovableVisited;
        stats.unmovableVisitedUnits += region->GetUnitCount();
        if (!region->IsYoungRegion()) {
            region = next;
            continue;
        }
        ++stats.unmovableYoung;
        // twoflags: notRelocatable is major-Assemble only. Young mark re-establishes
        // liveness for POST_TRACE-stamped regions — do not skip minor CSet.
        // routedest: that reasoning is about liveness and does not transfer. A route
        // destination is excluded here on address ownership, not on whether its contents are
        // reachable. This loop matters most of the four: every mutator thread-local region is
        // young (RegionSpace.cpp takes the youngRegion = true default), and the destination
        // recorded at RegionManager.cpp:1957 is exactly such a region — so before this gate a
        // minor collected a live route's destination while honouring nothing.
        const uint64_t visitorStart = TimeUtil::NanoSeconds();
        visitor(region);
        stats.visitorNs += TimeUtil::NanoSeconds() - visitorStart;
        ++stats.candidateRegions;
        stats.candidateBytes += region->GetRegionAllocatedSize();
        if (region->GetRawPointerObjectCount() == 0) {
            const uint64_t moveStart = TimeUtil::NanoSeconds();
            unmovableFromRegionList.DeleteRegion(region);
            fromRegionList.PrependRegion(region);
            stats.listMoveNs += TimeUtil::NanoSeconds() - moveStart;
        }
        region = next;
    }
    stats.unmovableNs = TimeUtil::NanoSeconds() - subStart;

    subStart = TimeUtil::NanoSeconds();
    region = recentFullRegionList.GetHeadRegion();
    while (region != nullptr) {
        ZPage* next = region->GetNextRegion();
        ++stats.recentFullVisited;
        stats.recentFullVisitedUnits += region->GetUnitCount();
        if (!region->IsYoungRegion()) {
            region = next;
            continue;
        }
        ++stats.recentFullYoung;
        // routedest: same exclusion as the unmovable young loop above.
        const uint64_t visitorStart = TimeUtil::NanoSeconds();
        visitor(region);
        stats.visitorNs += TimeUtil::NanoSeconds() - visitorStart;
        ++stats.candidateRegions;
        stats.candidateBytes += region->GetRegionAllocatedSize();
        if (region->GetRawPointerObjectCount() != 0) {
            region = next;
            continue;
        }
        const size_t units = region->GetUnitCount();
        const uint64_t moveStart = TimeUtil::NanoSeconds();
        recentFullRegionList.DeleteRegion(region);
        RecentFullAccounting::Dequeue(1, units);
        fromRegionList.PrependRegion(region);
        stats.listMoveNs += TimeUtil::NanoSeconds() - moveStart;
        region = next;
    }
    stats.recentFullNs = TimeUtil::NanoSeconds() - subStart;
    return stats;
}

void RemoveRegionLocked(RegionList* regionList, ZPage* region)
{
    regionList->DeleteRegionLocked(region);
}
namespace {
// Claim FROM under the from-list lock. AddRawPointerObject may retype to
// PINNED after ExemptFromRegions snapshots the list (RegionManager.h:507;
// CI face del->IsFromRegion at post_trace). ZGC skips !is_relocatable
// (zGeneration.cpp:211-213); a lost claim is the same skip, not a relaxed CHECK.
bool ClaimFromRegion(RegionList& fromList, ZPage* del, const char* site)
{
    if (fromList.TryDeleteRegion(del)) {
        return true;
    }
    const unsigned t = 0u;
    const unsigned rs = static_cast<unsigned>(del->RelocateObserve());
    LOG(RTLOG_ERROR, "[GCV2][isfromreg] site=%s skip type=%u route=%u young=%u", site, t, rs,
        static_cast<unsigned>(del->IsYoungRegion()));
    CHECK_DETAIL(del->OnNamedList("raw pointer pinned regions") ||
                     del->OnNamedList("escaped from regions") ||
                     del->IsGarbageRegion(),
                 "[isfromreg] site=%s unexpected type=%u route=%u", site, t, rs);
    return false;
}

} // namespace

// Cost-model CSet (ZRelocationSetSelector.cpp:114-196) after mark, before flip.
// Semi-sort by per-page live fraction, then select the last profitable prefix.
size_t RegionManager::ExemptFromRegions()
{
    Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD).select_relocation_set(false);
    return fromRegionList.GetUnitCount() * ZPage::UNIT_SIZE;
}

} // namespace MapleRuntime
