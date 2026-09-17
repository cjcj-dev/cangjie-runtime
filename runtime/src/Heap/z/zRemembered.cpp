// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zRemembered.hpp"
#include "Heap/z/zRemembered.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/WCollector/WCollectorInternal.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Common/BaseObject.h"
#include "Common/SuspendibleThreadSet.h"
#include "Mutator/ThreadLocal.h"

#include <atomic>

namespace MapleRuntime {

#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif
namespace WCollectorInternal {
bool HolderObjectIsLive(BaseObject* holder)
{
    if (holder == nullptr || !Heap::IsHeapAddress(holder) || !holder->IsValidObject()) {
        return false;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(holder));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return false;
    }
    if (region->IsAllocating()) {
        return true;
    }
    return region->is_object_strongly_live(from_object(holder));
}

bool SlotHeldByLiveObject(const void* slot)
{
    if (slot == nullptr || !Heap::IsHeapAddress(slot)) {
        return false;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(slot));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return false;
    }
    if (region->IsAllocating()) {
        return true;
    }
    const MAddress base = region->find_base_unsafe(reinterpret_cast<MAddress>(slot));
    BaseObject* holder = base == 0 ? nullptr : from_region_addr(base);
    if (holder == nullptr || reinterpret_cast<MAddress>(slot) - reinterpret_cast<MAddress>(holder) >=
        RegionSpace::GetAllocSize(*holder)) {
        return false;
    }
    return HolderObjectIsLive(holder);
}
} // namespace WCollectorInternal
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

ZRemembered::FoundOld::FoundOld()
    : _allocated_bitmap_0(), _allocated_bitmap_1(), _bitmaps{ nullptr, nullptr }, _current(0)
{}

void ZRemembered::FoundOld::ensure()
{
    if (_allocated_bitmap_0) {
        return;
    }
    const BitMap::idx_t bits = static_cast<BitMap::idx_t>(ZAddressOffsetMax >> ZGranuleSizeShift);
    _allocated_bitmap_0.reset(new CHeapBitMap(bits, true));
    _allocated_bitmap_1.reset(new CHeapBitMap(bits, true));
    _bitmaps[0] = _allocated_bitmap_0.get();
    _bitmaps[1] = _allocated_bitmap_1.get();
}

CHeapBitMap* ZRemembered::FoundOld::current_bitmap()
{
    ensure();
    return _bitmaps[_current];
}

CHeapBitMap* ZRemembered::FoundOld::previous_bitmap()
{
    ensure();
    return _bitmaps[_current ^ 1];
}

void ZRemembered::FoundOld::flip()
{
    ensure();
    _current ^= 1;
}

void ZRemembered::FoundOld::clear_previous()
{
    previous_bitmap()->clear_range(0, previous_bitmap()->size());
}

void ZRemembered::FoundOld::register_page(ZPage* page)
{
    CHECK(!page->IsYoungRegion());
    const BitMap::idx_t index =
        static_cast<BitMap::idx_t>(untype(page->start()) >> ZGranuleSizeShift);
    current_bitmap()->par_set_bit(index, std::memory_order_relaxed);
}

ZRemembered::ZRemembered() : _page_table(nullptr), _old_forwarding_table(nullptr), _page_allocator(nullptr), _found_old()
{}

void ZRemembered::bind(ZPageTable* page_table, const ZForwardingTable* old_forwarding_table,
                       RegionManager* page_allocator)
{
    _page_table = page_table;
    _old_forwarding_table = old_forwarding_table;
    _page_allocator = page_allocator;
}

void ZRemembered::flip_found_old_sets()
{
    _found_old.flip();
}

void ZRemembered::clear_found_old_previous_set()
{
    _found_old.clear_previous();
}

void ZRemembered::register_found_old(ZPage* page)
{
    CHECK(!page->IsYoungRegion());
    _found_old.register_page(page);
}

template<typename Function>
void ZRemembered::oops_do_forwarded_via_containing(const std::vector<ZRememberedSetContaining>* array,
                                                   Function function) const
{
    MAddress from_addr = 0;
    MAddress to_addr = 0;
    size_t object_size = 0;
    for (const ZRememberedSetContaining containing : *array) {
        if (from_addr != containing._addr) {
            from_addr = containing._addr;
            BaseObject* to = Heap::GetHeap().GetCollector().relocate_or_remap_object(
                reinterpret_cast<BaseObject*>(from_addr), ZGenerationId::old);
            to_addr = reinterpret_cast<MAddress>(to);
            object_size = to != nullptr ? RegionSpace::GetAllocSize(*to) : 0;
        }
        const uintptr_t field_offset = containing._field_addr - from_addr;
        if (field_offset < object_size) {
            function(reinterpret_cast<volatile zpointer*>(to_addr + field_offset));
        }
    }
}

bool ZRemembered::should_scan_page(ZPage* page) const
{
    Collector& collector = Heap::GetHeap().GetCollector();
    const GCPhase phase = collector.GetGCPhase(GCCycleGeneration::OLD);
    if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
        return true;
    }
    ZForwarding* forwarding = collector.GetGenerationCycle(GCCycleGeneration::OLD).forwarding(
        untype(ZOffset::address_unsafe(page->start())));
    if (forwarding == nullptr) {
        return true;
    }
    if (!forwarding->relocated_remembered_fields_is_concurrently_scanned()) {
        return true;
    }
    return false;
}

bool ZRemembered::scan_page_and_clear_remset(ZPage* page) const
{
    Collector& collector = Heap::GetHeap().GetCollector();
    const bool can_trust_live_bits =
        page->is_relocatable() && collector.GetGCPhase(GCCycleGeneration::OLD) != GCPhase::GC_PHASE_ENUM &&
        collector.GetGCPhase(GCCycleGeneration::OLD) != GCPhase::GC_PHASE_TRACE;
    bool result = false;
    if (!can_trust_live_bits) {
        page->oops_do_remembered([&](volatile zpointer* p) { result |= scan_field(p); });
    } else if (page->is_marked()) {
        page->oops_do_remembered_in_live([&](volatile zpointer* p) { result |= scan_field(p); });
    }
    if (!can_trust_live_bits || page->is_marked()) {
        page->clear_remset_previous();
    }
    return result;
}

static void fill_containing(std::vector<ZRememberedSetContaining>* array, ZPage* page)
{
    ZRememberedSetContainingIterator iter(page);
    for (ZRememberedSetContaining containing; iter.next(&containing);) {
        array->push_back(containing);
    }
}

struct ZRememberedScanForwardingContext {
    std::vector<ZRememberedSetContaining> _containing_array;
};

bool ZRemembered::scan_forwarding(ZForwarding* forwarding, void* context_void) const
{
    auto* context = static_cast<ZRememberedScanForwardingContext*>(context_void);
    bool result = false;
    if (forwarding->retain_page(&generation_relocate_queue())) {
        forwarding->relocated_remembered_fields_notify_concurrent_scan_of();
        context->_containing_array.clear();
        fill_containing(&context->_containing_array, forwarding->page());
        forwarding->release_page();
        oops_do_forwarded_via_containing(&context->_containing_array, [&](volatile zpointer* p) {
            result |= scan_field(p);
        });
    } else {
        forwarding->relocated_remembered_fields_apply_to_published([&](MAddress field) {
            result |= scan_field(reinterpret_cast<volatile zpointer*>(field));
        });
    }
    return result;
}

ZRemsetTableIterator::ZRemsetTableIterator(ZRemembered* remembered, bool previous)
    : _remembered(remembered),
      _bm(previous ? remembered->_found_old.previous_bitmap() : remembered->_found_old.current_bitmap()),
      _page_table(remembered->_page_table),
      _old_forwarding_table(remembered->_old_forwarding_table),
      _claimed(0)
{}

bool ZRemsetTableIterator::next(ZRemsetTableEntry* entry_addr)
{
    BitMap::idx_t prev = __atomic_load_n(&_claimed, __ATOMIC_RELAXED);
    for (;;) {
        if (prev == _bm->size()) {
            return false;
        }
        const BitMap::idx_t page_index = _bm->find_first_set_bit(prev);
        if (page_index == _bm->size()) {
            __atomic_compare_exchange_n(&_claimed, &prev, page_index, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
            return false;
        }
        BitMap::idx_t expected = prev;
        if (!__atomic_compare_exchange_n(&_claimed, &expected, page_index + 1, false, __ATOMIC_RELAXED,
                                         __ATOMIC_RELAXED)) {
            prev = expected;
            continue;
        }
        ZForwarding* forwarding = nullptr;
        Collector& collector = Heap::GetHeap().GetCollector();
        const GCPhase phase = collector.GetGCPhase(GCCycleGeneration::OLD);
        if (phase == GCPhase::GC_PHASE_PREFORWARD || phase == GCPhase::GC_PHASE_FORWARD) {
            forwarding = _old_forwarding_table->at(page_index);
        }
        ZPage* page = _page_table->at(page_index);
        if (page != nullptr && page->IsYoungRegion()) {
            page = nullptr;
        }
        if (page == nullptr && forwarding == nullptr) {
            prev = page_index + 1;
            continue;
        }
        entry_addr->_forwarding = forwarding;
        entry_addr->_page = page;
        return true;
    }
}

void ZRemembered::remap_current(ZRemsetTableIterator* iter)
{
    for (ZRemsetTableEntry entry; iter->next(&entry);) {
        CHECK(entry._forwarding == nullptr);
        CHECK(entry._page != nullptr);
        entry._page->oops_do_current_remembered([](volatile zpointer* p) {
            (void)ZBarrier::load_barrier_on_oop_field(p);
        });
    }
}

bool ZRemembered::scan_field(volatile zpointer* p) const
{
    RefField<>& field = *reinterpret_cast<RefField<>*>(const_cast<zpointer*>(p));
    const zaddress addr = ZBarrier::RemsetBarrierOnOopField(field);
    if (!is_null(addr) && Heap::is_young(untype(addr))) {
        remember(p);
        return true;
    }
    return false;
}

void ZRemembered::flip()
{
    ZRememberedSet::flip();
    flip_found_old_sets();
}

class ZRememberedScanMarkFollowTask : public ZRestartableTask {
private:
    ZRemembered* const _remembered;
    ZMark* const _mark;
    ZRemsetTableIterator _remset_table_iterator;

public:
    ZRememberedScanMarkFollowTask(ZRemembered* remembered, ZMark* mark)
        : ZRestartableTask("ZRememberedScanMarkFollowTask"),
          _remembered(remembered),
          _mark(mark),
          _remset_table_iterator(remembered, true)
    {
        _mark->PrepareWork();
        ZPage::EnableSafeDestroy();
    }

    ~ZRememberedScanMarkFollowTask()
    {
        ZPage::DisableSafeDestroy();
        _mark->FinishWork();
        _remembered->clear_found_old_previous_set();
    }

    void work_inner()
    {
        ZRememberedScanForwardingContext context;
        if (!_mark->FollowWorkPartial()) {
            return;
        }
        for (ZRemsetTableEntry entry; _remset_table_iterator.next(&entry);) {
            bool left_marking = false;
            ZForwarding* forwarding = entry._forwarding;
            ZPage* page = entry._page;
            if (forwarding != nullptr) {
                bool found_roots = _remembered->scan_forwarding(forwarding, &context);
                ZVerify::AfterScan(forwarding);
                if (found_roots) {
                    left_marking = !_mark->FollowWorkPartial();
                }
            }
            if (page != nullptr) {
                if (_remembered->should_scan_page(page)) {
                    bool found_roots = _remembered->scan_page_and_clear_remset(page);
                    if (found_roots && !left_marking) {
                        left_marking = !_mark->FollowWorkPartial();
                    }
                }
                _remembered->register_found_old(page);
            }
            SuspendibleThreadSet::yield();
            if (left_marking) {
                return;
            }
        }
        _mark->FollowWorkComplete(false);
    }

    void work() override
    {
        SuspendibleThreadSetJoiner sts_joiner;
        work_inner();
        (void)_mark->Flush(ThreadLocal::GetThreadLocalData());
    }

    void resize_workers(uint32_t nworkers) override
    {
        _mark->ResizeWorkers(nworkers);
    }
};

void ZRemembered::scan_and_follow(ZMark* mark)
{
    {
        ZRememberedScanMarkFollowTask task(this, mark);
        ZWorkers* workers = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG).Workers();
        if (workers != nullptr) {
            workers->run(&task);
        } else {
            task.work();
        }
        if (mark->PollStop() || !mark->TryTerminateFlush()) {
            return;
        }
    }
    mark->MarkFollow();
}

} // namespace MapleRuntime
