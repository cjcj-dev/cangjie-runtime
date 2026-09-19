// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zAbort.hpp"
#include "Heap/z/zMark.hpp"
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
#include "Heap/z/zRelocationSetSelector.inline.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zForwardingEntry.hpp"
#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zRelocate.hpp"



namespace MapleRuntime {

static const ZStatSubPhase PCollectFromSpaceGarbage("CollectFromSpaceGarbage", ZGenerationId::old);
static const ZStatSubPhase PPostTrace("PostTrace", ZGenerationId::old);

void ZGenerationOld::PostTrace()
{
    ZStatTimerOld zstatTimer(PPostTrace);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().HandleTraceRegions();
    // Value-only cycle roots still depend on the preceding relocation receipts.
    // Complete their owner handoff while that authority is queryable.
    // zGeneration.cpp:1261 mark_end does not reset forwarding.
#if defined(MRT_TESTABLE_INTERNALS)
    Heap::GetHeap().cross_vm().ObserveExportOwnershipForTest(false);
#endif
    Heap::GetHeap().cross_vm().PrepareCycleRef();
#if defined(MRT_TESTABLE_INTERNALS)
    Heap::GetHeap().cross_vm().ObserveExportOwnershipForTest(true);
#endif
    CollectLargeGarbage();
    CollectPinnedGarbage();
    // zGeneration.cpp:1042 / :1131-1133: reset previous set before select.
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).reset_relocation_set();
    if (ZAbort::should_abort()) {
        return;
    }
    ZRelocate::RefineFromSpace();
    space.PrepareFromSpace<Generation::Old>();
    // OPTION_2 mark-epoch release: TRACE+CLEAR_SATB done; publish quarantined post-dispel
    // units (from this PrepareForwardTable and any prior minor) to dirty for reuse.
    // INV-1 closed: concurrent mark can no longer follow plain edges into these ranges.
    space.GetRegionManager().ReleaseMarkQuarantine();
}
void ZGenerationOld::CollectSmallSpace()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    {
        ZStatTimerOld zstatTimer(PCollectFromSpaceGarbage);
        space.CollectFromSpaceGarbage();
    }

    VLOG(REPORT, "start to release heap garbage memory");
#if defined(__EULER__)
    Heap::GetHeap().GetAllocator().TryReclaimGarbageMemory();
#endif
    Heap::GetHeap().GetFinalizerProcessor().NotifyToReclaimGarbage();
}
} // namespace MapleRuntime

namespace MapleRuntime {
void RegionManager::ResetFlipPromotedPages()
{
}

ZRelocationSet::ZRelocationSet(ZGeneration* generation)
    : _generation(generation),
      _allocator(),
      _forwardings(nullptr),
      _nforwardings(0)
{
}

ZWorkers* ZRelocationSet::workers() const { return _generation != nullptr ? _generation->Workers() : nullptr; }

class ZRelocationSetInstallTask : public ZTask {
private:
    ZRelocationSet* _relocation_set;
    ZForwardingAllocator* const _allocator;
    ZForwarding** _forwardings;
    const size_t _nforwardings;
    const ZArray<ZPage*>* _small;
    const ZArray<ZPage*>* _medium;
    ZArrayParallelIterator<ZPage*> _small_iter;
    ZArrayParallelIterator<ZPage*> _medium_iter;

    void install(ZForwarding* forwarding, size_t index)
    {
        ZPage* const page = forwarding->page();
        page->ClearRelocationResiduals();
        _relocation_set->generation()->forwarding_table().insert(forwarding);
        if (page->GetOwnerGeneration() == Generation::Young) {
            page->PublishForwardingCarrier<Generation::Young>();
        } else {
            page->PublishForwardingCarrier<Generation::Old>();
        }
        _forwardings[index] = forwarding;
    }

public:
    ZRelocationSetInstallTask(ZRelocationSet* relocation_set, const ZRelocationSetSelector* selector)
        : ZTask("ZRelocationSetInstallTask"),
          _relocation_set(relocation_set),
          _allocator(&relocation_set->_allocator),
          _forwardings(nullptr),
          _nforwardings(static_cast<size_t>(selector->selected_small()->length()) +
                        static_cast<size_t>(selector->selected_medium()->length())),
          _small(selector->selected_small()),
          _medium(selector->selected_medium()),
          _small_iter(selector->selected_small()),
          _medium_iter(selector->selected_medium())
    {
        const size_t relocation_set_size = _nforwardings * sizeof(ZForwarding*);
        const size_t forwardings_size = _nforwardings * sizeof(ZForwarding);
        const size_t forwarding_entries_size = selector->forwarding_entries() * sizeof(ZForwardingEntry);
        _allocator->reset(relocation_set_size + forwardings_size + forwarding_entries_size);
        _forwardings = new (_allocator->alloc(relocation_set_size)) ZForwarding*[_nforwardings];
    }

    ~ZRelocationSetInstallTask() { CHECK(_allocator->is_full()); }

    virtual void work()
    {
        ZArray<ZPage*> relocate_promoted;
        for (size_t page_index; _small_iter.next_index(&page_index);) {
            ZPage* page = _small->at(static_cast<int>(page_index));
            ZForwarding* const forwarding = ZForwarding::alloc(_allocator, page, page->age());
            install(forwarding, static_cast<size_t>(_medium->length()) + page_index);
            if (forwarding->is_promotion()) {
                relocate_promoted.push(page);
            }
        }
        for (size_t page_index; _medium_iter.next_index(&page_index);) {
            ZPage* page = _medium->at(static_cast<int>(page_index));
            ZForwarding* const forwarding = ZForwarding::alloc(_allocator, page, page->age());
            install(forwarding, page_index);
            if (forwarding->is_promotion()) {
                relocate_promoted.push(page);
            }
        }
        _relocation_set->register_relocate_promoted(relocate_promoted);
    }

    ZForwarding** forwardings() const { return _forwardings; }
    size_t nforwardings() const { return _nforwardings; }
};

void ZRelocationSet::install(const ZRelocationSetSelector* selector)
{
    ZRelocationSetInstallTask task(this, selector);
    if (ZWorkers* w = workers()) {
        w->run(&task);
    } else {
        task.work();
    }
    _forwardings = task.forwardings();
    _nforwardings = task.nforwardings();
    // zRelocationSet.cpp:179: forwarding-allocator usage after install.
    if (_generation != nullptr) {
        _generation->StatRelocation()->AtInstallRelocationSet(_allocator.size());
    }
}

void ZRelocationSet::install_from_regions(RegionList& regions)
{
    if (_nforwardings != 0) {
        return;
    }
    ZRelocationSetSelector selector;
    regions.VisitAllRegions([&](ZPage* region) {
        selector.add_selected_small(region, ZForwarding::nentries(region));
    });
    install(&selector);
}

static void destroy_and_clear(RegionManager* page_allocator, ZArray<ZPage*>* array)
{
    for (int i = 0; i < array->length(); ++i) {
        // zRelocationSet.cpp:182-189: every registered page is destroyed, no
        // skip arm; a page still mapped here means registration was wrong.
        ZPage* const page = array->at(i);
        page_allocator->safe_destroy_page(page);
    }
    array->clear();
}

void ZRelocationSet::reset(RegionManager* page_allocator)
{
    ZRelocationSetIterator iter(this);
    for (ZForwarding* forwarding; iter.next(&forwarding);) {
        forwarding->~ZForwarding();
    }
    _nforwardings = 0;
    _forwardings = nullptr;
    destroy_and_clear(page_allocator, &_in_place_relocate_promoted_pages);
    destroy_and_clear(page_allocator, &_flip_promoted_pages);
    _relocate_promoted_pages.clear();
}

void ZRelocationSet::register_flip_promoted(const ZArray<ZPage*>& pages)
{
    std::lock_guard<std::mutex> locker(_promotion_lock);
    for (int i = 0; i < pages.length(); ++i) {
        // zRelocationSet.cpp:208: no duplicates allowed.
        CHECK(!_flip_promoted_pages.contains(pages.at(i)));
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

void ZGeneration::reset_relocation_set()
{
    ZRelocationSetIterator iter(&_relocation_set);
    for (ZForwarding* forwarding; iter.next(&forwarding);) {
        _forwarding_table.remove(forwarding);
    }
    _relocation_set.reset(&Heap::GetHeap().page_allocator());
}

} // namespace MapleRuntime
