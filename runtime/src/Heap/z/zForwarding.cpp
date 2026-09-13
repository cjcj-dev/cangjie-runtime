// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zForwarding.hpp"

#include "Heap/z/zPage.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/Allocator/RegionSpace.h"

#include <cstdio>
#include <cstdlib>
#include <sched.h>

#include "Heap/z/zVerify.hpp"
#include <unordered_set>
#include <vector>

namespace MapleRuntime {

namespace {
thread_local ZForwarding* currentPageWork = nullptr;
}

ZForwardingLife::PageWorkScope::PageWorkScope(ZForwarding* forwarding, bool complete)
    : previous(currentPageWork), forwarding(forwarding), complete(complete)
{
    if (complete) CHECK(forwarding != nullptr && forwarding->claim());
    currentPageWork = forwarding;
}
ZForwardingLife::PageWorkScope::~PageWorkScope()
{
    if (complete) {
        if (forwarding->ref_count().load(std::memory_order_acquire) != 0) forwarding->release_page();
        forwarding->detach_page();
        forwarding->mark_done();
    }
    currentPageWork = previous;
}
ZForwarding* ZForwardingLife::CurrentPageWork() { return currentPageWork; }

void ZForwardingLife::WaitUntilRef(std::atomic<int32_t>& refCount, int32_t expect)
{
    if (refCount.load(std::memory_order_acquire) == expect) {
        return;
    }
    // Yield, do not park on the process-wide cv: a mutator in cv.wait is
    // not in a saferegion and blocks STW (fifth-face all-futex hang).
    // MRT_EnterSaferegion around cv.wait was tried; FormatLog FATAL in a
    // forked gc_unit child then SEGV'd the parent (logger lock). Observe
    // the published word via acquire load instead.
    while (refCount.load(std::memory_order_acquire) != expect) {
        sched_yield();
    }
}

RegionInfo::InPlaceClaimScope::InPlaceClaimScope(RegionInfo* region, ZForwardingLife::Retire site)
    : owner(ForwardingTable::RetainPageOwner(region))
{
    (void)site;
    if (region == nullptr) return;
    if (!owner) {
        return;
    }
    const int32_t before = owner->ref_count().load(std::memory_order_acquire);
    const bool borrowed = ZForwardingLife::CurrentPageWork() == owner.get();
    if (before == 0 || (!borrowed && !owner->claim())) {
        owner->detach_page();
    } else if (before > 0) {
        owner->in_place_relocation_claim_page();
        retiring = true;
    }
}

void ZForwardingLife::WaitPageDone(ZForwarding* forwarding)
{
    if (forwarding == nullptr) {
        return;
    }
    // Legacy page cleanup runs inside the completion owner itself.
    if (CurrentPageWork() == forwarding || forwarding->is_done()) return;
    auto& queue = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().GetRelocationRequestQueue();
    const auto request = queue.Add(ForwardingTable::Owner(forwarding));
    CHECK_DETAIL(request.accepted, "forwarding wait requires a page task");
    (void)queue.Wait(request.request);
}


} // namespace MapleRuntime

namespace MapleRuntime {
bool ZForwarding::claim()
{ return ZForwardingLife::claim(_claimed); }

bool ZForwarding::retain_page()
{
        return ZForwardingLife::retain_page(_ref_count, [this] { ZForwardingLife::WaitPageDone(this); });
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

void ZForwarding::detach_page()
{
        std::unique_lock<std::mutex> lock(_ref_lock);
        _ref_changed.wait(lock, [this] { return _ref_count.load(std::memory_order_acquire) == 0; });
    }

void ZForwarding::mark_done()
{ ZForwardingLife::mark_done(_done); }

bool ZForwarding::is_done() const
{ return ZForwardingLife::is_done(_done); }

void ZForwarding::in_place_relocation_claim_page()
{
        int32_t count = _ref_count.load(std::memory_order_relaxed);
        do {
            CHECK(count > 0);
        } while (!_ref_count.compare_exchange_weak(count, -count, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed));
        std::unique_lock<std::mutex> lock(_ref_lock);
        _ref_changed.wait(lock, [this] { return _ref_count.load(std::memory_order_acquire) == -1; });
    }
}

namespace MapleRuntime {
bool ZForwardingLife::claim(std::atomic<bool>& claimed)
    {
        bool expected = false;
        return claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
}

namespace MapleRuntime {
void ZForwardingLife::mark_done(std::atomic<bool>& done)
    {
        done.store(true, std::memory_order_release);
        NotifyAll();
    }
}

namespace MapleRuntime {
bool ZForwardingLife::is_done(const std::atomic<bool>& done) { return done.load(std::memory_order_acquire); }
}

namespace MapleRuntime {
void ZForwardingLife::release_page(std::atomic<int32_t>& refCount)
    {
        for (;;) {
            int32_t n = refCount.load(std::memory_order_relaxed);
            CHECK(n != 0);
            if (n > 0) {
                if (!refCount.compare_exchange_weak(n, n - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    continue;
                }
                if (n == 1) {
                    NotifyAll();
                }
            } else {
                if (!refCount.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    continue;
                }
                if (n == -2 || n == -1) {
                    NotifyAll();
                }
            }
            return;
        }
    }
}

namespace MapleRuntime {
void ZForwardingLife::in_place_relocation_claim_page(std::atomic<int32_t>& refCount)
    {
        for (;;) {
            int32_t n = refCount.load(std::memory_order_relaxed);
            CHECK(n > 0);
            if (!refCount.compare_exchange_weak(n, -n, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                continue;
            }
            if (n != 1) {
                WaitUntilRef(refCount, -1);
            }
            return;
        }
    }
}

namespace MapleRuntime {
void ZForwardingLife::detach_page(std::atomic<int32_t>& refCount)
    {
        if (refCount.load(std::memory_order_acquire) == 0) {
            return;
        }
        WaitUntilRef(refCount, 0);
    }
}

namespace MapleRuntime {
RegionInfo* ZForwarding::page() const { return _page; }
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
        ZVerify::Object(object, nullptr);
        bytes += RegionSpace::GetAllocSize(*object);
    }
    // The source incarnation's livemap is retained by FromPageView even for
    // in-place relocation, where reusable page metadata already names to-space.
    const FromPageView* from = from_page_snapshot();
    CHECK_DETAIL(from != nullptr && from->liveInfo != nullptr, "Missing forwarding source livemap");
    RegionBitmap* bitmap = _page->GetOwnerMarkBitmap(from->liveInfo);
    CHECK_DETAIL(bitmap != nullptr && sources.size() == bitmap->GetLiveObjects() &&
                 bytes == bitmap->GetLiveBytes(), "Invalid forwarding live objects or bytes");
}

} // namespace MapleRuntime
