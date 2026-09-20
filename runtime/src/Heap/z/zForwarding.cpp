// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zForwarding.hpp"

#include "Heap/z/zPage.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/Allocator/RegionSpace.h"

#include <cstdio>
#include <cstdlib>
#include <sched.h>

#include "Heap/z/zVerify.hpp"
#include <unordered_set>
#include <vector>

namespace MapleRuntime {
uint32_t ZForwarding::young_seqnum()
{
    return static_cast<uint32_t>(Heap::GetHeap().young().Snapshot().sequence);
}


uint32_t ZForwarding::nentries(const ZPage* page)
{
    return static_cast<uint32_t>(nentries(static_cast<size_t>(page->live_objects())));
}

ZForwarding* ZForwarding::alloc(ZForwardingAllocator* allocator, ZPage* page, PageAge to_age)
{
    const size_t nentries = ZForwarding::nentries(page);
    void* const addr = AttachedArray::alloc(allocator, nentries);
    // Retain the source page until relocation finishes (ZGC zForwarding.hpp:63).
    page->ClearRelocationResiduals();
    ZForwarding* forwarding = ::new (addr) ZForwarding(page, page->GetRegionStart(), ZAddressHeapBase,
        page->GetRegionSize(), nentries, page->GetRegionLifeId(), page->age(), to_age,
        static_cast<size_t>(page->object_alignment_shift()));
    return forwarding;
}

namespace {
thread_local ZForwarding* currentPageWork = nullptr;
}

ZForwarding::PageWorkScope::PageWorkScope(ZForwarding* forwarding, bool complete)
    : previous(currentPageWork), forwarding(forwarding), complete(complete)
{
    if (complete) CHECK(forwarding != nullptr && forwarding->claim());
    currentPageWork = forwarding;
}
ZForwarding::PageWorkScope::~PageWorkScope()
{
    if (complete) {
        if (forwarding->ref_count().load(std::memory_order_acquire) != 0) forwarding->release_page();
        forwarding->detach_page();
        forwarding->mark_done();
    }
    currentPageWork = previous;
}
ZForwarding* ZForwarding::CurrentPageWork() { return currentPageWork; }

ZPage::InPlaceClaimScope::InPlaceClaimScope(ZPage* region, ZForwarding::Retire site)
    : owner(forwarding_for_page(region))
{
    (void)site;
    if (region == nullptr) return;
    if (!owner) {
        return;
    }
    const int32_t before = owner->ref_count().load(std::memory_order_acquire);
    const bool borrowed = ZForwarding::CurrentPageWork() == owner;
    if (before == 0 || (!borrowed && !owner->claim())) {
        owner->detach_page();
    } else if (before > 0) {
        owner->in_place_relocation_claim_page();
        retiring = true;
    }
}

void ZForwarding::WaitPageDone(ZForwarding* forwarding)
{
    if (forwarding == nullptr) {
        return;
    }
    // Legacy page cleanup runs inside the completion owner itself.
    if (CurrentPageWork() == forwarding || forwarding->is_done()) return;
    auto& queue = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().GetZRelocateQueue();
    const auto request = queue.Add(forwarding);
    CHECK_DETAIL(request.accepted, "forwarding wait requires a page task");
    queue.Wait(request.forwarding);
}


} // namespace MapleRuntime

namespace MapleRuntime {
bool ZForwarding::claim()
{
    bool expected = false;
    return _claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
}

bool ZForwarding::retain_page(ZRelocateQueue* queue)
{
    for (;;) {
        int32_t n = _ref_count.load(std::memory_order_acquire);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            queue->add_and_wait(this);
            return false;
        }
        if (_ref_count.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
}

void ZForwarding::release_page()
{
        int32_t count = _ref_count.load(std::memory_order_relaxed);
        for (;;) {
            CHECK(count != 0);
            const int32_t next = count > 0 ? count - 1 : count + 1;
            if (_ref_count.compare_exchange_weak(count, next, std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
                if (next == 0 || next == -1) {
                    std::lock_guard<std::mutex> lock(_ref_lock);
                    _ref_changed.notify_all();
                }
                return;
            }
        }
    }

ZPage* ZForwarding::detach_page()
{
        if (_ref_count.load(std::memory_order_acquire) != 0) {
            std::unique_lock<std::mutex> lock(_ref_lock);
            _ref_changed.wait(lock, [this] { return _ref_count.load(std::memory_order_acquire) == 0; });
        }
        return _page;
    }

void ZForwarding::mark_done()
{
    _done.store(true, std::memory_order_release);
}

bool ZForwarding::is_done() const
{
    return _done.load(std::memory_order_acquire);
}

void ZForwarding::in_place_relocation_claim_page()
{
        for (;;) {
            int32_t count = _ref_count.load(std::memory_order_relaxed);
            CHECK(count > 0);
            if (!_ref_count.compare_exchange_weak(count, -count, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
                continue;
            }
            if (count != 1) {
                std::unique_lock<std::mutex> lock(_ref_lock);
                _ref_changed.wait(lock, [this] { return _ref_count.load(std::memory_order_acquire) == -1; });
            }
            break;
        }
    }

void ZForwarding::in_place_relocation_start(MAddress relocated_watermark)
{
    (void)relocated_watermark;
    _in_place.store(true, std::memory_order_release);
    _in_place_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    _in_place_top_at_start = _page != nullptr ? _page->GetRegionAllocPtr() : 0;
}

void ZForwarding::in_place_relocation_finish()
{
    if (_from_age == PageAge::old || _to_age != PageAge::old) {
        if (_page != nullptr) {
            _page->reset_livemap();
        }
    }
    _in_place_thread.store(std::thread::id(), std::memory_order_relaxed);
}

bool ZForwarding::in_place_relocation_is_below_top_at_start(MAddress offset) const
{
    return _in_place_thread.load(std::memory_order_relaxed) == std::this_thread::get_id() &&
           offset < _in_place_top_at_start;
}
}

namespace MapleRuntime {
ZPage* ZForwarding::page() const { return _page; }

bool ZForwarding::page_life_current() const
{
    return _page != nullptr && _page->GetRegionLifeId() == _page_life_id;
}
}

namespace MapleRuntime {
bool ZForwarding::relocated_remembered_fields_published_contains(MAddress field)
    {
        std::lock_guard<std::mutex> lock(_relocated_fields_lock);
        for (MAddress entry : _relocated_remembered_fields_array) {
            if (entry == field) { return true; }
        }
        return false;
    }
}

namespace MapleRuntime {
void ZForwarding::relocated_remembered_fields_after_relocate()
    {
        _relocated_remembered_fields_publish_young_seqnum = young_seqnum();
        if (young_marking()) {
            relocated_remembered_fields_publish();
        }
    }
}

namespace MapleRuntime {
void ZForwarding::relocated_remembered_fields_publish()
    {
        ZPublishState expected = ZPublishState::none;
        if (!_relocated_remembered_fields_state.compare_exchange_strong(
                expected, ZPublishState::published, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(_relocated_fields_lock);
            _relocated_remembered_fields_array.clear();
        }
    }
}

namespace MapleRuntime {
void ZForwarding::relocated_remembered_fields_notify_concurrent_scan_of()
    {
        ZPublishState expected = ZPublishState::none;
        if (_relocated_remembered_fields_state.compare_exchange_strong(
                expected, ZPublishState::reject, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }
        if (expected == ZPublishState::published) {
            ZPublishState published = ZPublishState::published;
            if (_relocated_remembered_fields_state.compare_exchange_strong(
                    published, ZPublishState::reject, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lock(_relocated_fields_lock);
                _relocated_remembered_fields_array.clear();
            }
        }
    }
}

namespace MapleRuntime {

// zForwarding.cpp:369-409. Inspect the real table, including the port's
// overflow entries; never reconstruct mappings from object headers.
void ZForwarding::verify() const
{
    CHECK_DETAIL(_ref_count.load(std::memory_order_acquire) != 0, "Invalid forwarding reference count");
    CHECK_DETAIL(_page != nullptr, "Invalid forwarding page");
    std::vector<MAddress> sources;
    for_each_from([&](MAddress from) { sources.push_back(from); });
    std::unordered_set<MAddress> uniqueSources;
    std::unordered_set<MAddress> destinations;
    size_t bytes = 0;
    for (MAddress from : sources) {
        CHECK_DETAIL(from >= start() && from - start() < size(), "Invalid forwarding source");
        CHECK_DETAIL(uniqueSources.insert(from).second, "Duplicate forwarding source");
        const MAddress to = find(from);
        CHECK_DETAIL(to != 0 && destinations.insert(to).second, "Duplicate or null forwarding destination");
        BaseObject* object = reinterpret_cast<BaseObject*>(to);
        bytes += RegionSpace::GetAllocSize(*object);
    }
    _page->verify_live(static_cast<uint32_t>(sources.size()), bytes, in_place());
}

} // namespace MapleRuntime
