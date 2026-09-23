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
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/shared/collectedHeap.hpp"
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
    const int partitionSizeShift = Log2Exact(partitionSize);
    return (NumPartitions - 1) - (page->live_bytes() >> partitionSizeShift);
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
        DCHECK(sorted_live_pages.at(dest) == nullptr);
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

void ZRelocationSetSelector::check_selected_relocatable() const
{
    auto check = [](const ZArray<ZPage*>* pages) {
        if (pages == nullptr) {
            return;
        }
        for (int i = 0; i < pages->length(); ++i) {
            ZPage* page = pages->at(i);
            CHECK_DETAIL(page->is_relocatable(),
                         "selected page must be relocatable start=%#zx", page->GetRegionStart());
        }
    };
    check(selected_small());
    check(selected_medium());
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

void RegionManager::CountLiveObject(const BaseObject* obj)
{
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(obj));
    region->inc_live(1, obj->GetSize());
}

void RegionManager::AssembleSmallGarbageCandidates() {}

void RegionManager::AssembleLargeGarbageCandidates() {}

} // namespace MapleRuntime
