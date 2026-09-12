// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/ZForwardingLife.h"

#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/ZForwarding.h"

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

std::atomic<uint64_t> ZForwardingLife::g_retainRefusedReleased{ 0 };
std::atomic<uint64_t> ZForwardingLife::g_retainRefusedClaimed{ 0 };
std::atomic<uint64_t> ZForwardingLife::g_detachWaited{ 0 };

namespace {
struct DumpOnce {
    DumpOnce()
    {
        std::atexit([]() {
            std::fprintf(stderr,
                         "[GCV2][zlife] atexit refuse_released=%llu refuse_claimed=%llu detach_waited=%llu\n",
                         static_cast<unsigned long long>(ZForwardingLife::RetainRefusedReleased()),
                         static_cast<unsigned long long>(ZForwardingLife::RetainRefusedClaimed()),
                         static_cast<unsigned long long>(ZForwardingLife::DetachWaited()));
            std::fflush(stderr);
        });
    }
};
const DumpOnce g_dumpOnce;
} // namespace

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
    // zForwarding.cpp:110-130: the page worker never waits for its own done.
    if (CurrentPageWork() == forwarding) {
        return;
    }
    WaitUntilDone(forwarding->ref_count(), forwarding->done());
}

void ZForwardingLife::WaitUntilDone(std::atomic<int32_t>& refCount, const std::atomic<bool>& done)
{
    // zForwarding.cpp:96-100 add_and_wait: wait until is_done. Also treat
    // ref==0 as terminal — ResetIdle / InitRegionInfo reuse the same
    // ZForwardingLife words in place (ZGC destroys the forwarding).
    if (done.load(std::memory_order_acquire) || refCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    while (!done.load(std::memory_order_acquire) && refCount.load(std::memory_order_acquire) != 0) {
        sched_yield();
    }
}

} // namespace MapleRuntime
