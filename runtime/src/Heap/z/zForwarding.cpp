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
