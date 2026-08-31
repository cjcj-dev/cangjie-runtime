// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/ZForwardingLife.h"

#include "Heap/Allocator/RegionInfo.h"

#include <cstdio>
#include <cstdlib>
#include <sched.h>

namespace MapleRuntime {

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

void ZForwardingLife::WaitUntilCopyAdmissionSettled(std::atomic<int32_t>& copyWord)
{
    while (copy_admission_state(copyWord) == CopyAdmissionState::ENTERING) {
        sched_yield();
    }
}

void ZForwardingLife::wait_copied(std::atomic<int32_t>& copyWord)
{
    for (;;) {
        int32_t word = copyWord.load(std::memory_order_acquire);
        const CopyAdmissionState state = copy_admission_state(word);
        if (state == CopyAdmissionState::ENTERING) {
            WaitUntilCopyAdmissionSettled(copyWord);
            continue;
        }
        if (state == CopyAdmissionState::OPEN) {
            const int32_t sealed = PackCopyWord(CopyAdmissionState::SEALED, copy_count(word));
            if (!copyWord.compare_exchange_weak(
                    word, sealed, std::memory_order_acq_rel, std::memory_order_acquire)) {
                continue;
            }
        }
        WaitUntilRef(copyWord, CopyAdmissionSealedWord());
        return;
    }
}

RegionInfo::DrainScope::DrainScope(RegionInfo* region, MutatorRelocate::Retire site) : region(nullptr)
{
    if (region == nullptr) {
        return;
    }
    uint64_t start = TimeUtil::NanoSeconds();
    // copyInflight is the copier token (zForwarding.cpp:86-108 / lockdrain).
    // detach_page does not observe it. ClearUnits must not race a LOCKED copy.
    ZForwardingLife::wait_copied(region->metadata.copyInflight);
    int32_t before = region->metadata.fwdRefCount.load(std::memory_order_acquire);
    // zForwarding.cpp:171-181 detach_page: wait until 0. A count that is
    // already 0 is a successful wait, not a license to skip the drain —
    // LEAD-NOTE 0820 21:1x: TakeRegion garbage_reuse ClearUnits ran with
    // no wait when only naked mutator refs remained.
    if (before == 0) {
        ZForwardingLife::detach_page(region->metadata.fwdRefCount);
        uint64_t spun = TimeUtil::NanoSeconds() - start;
        MutatorRelocate::NoteDrain(site, spun, false);
        return;
    }
    if (!region->ClaimForwarding()) {
        // Another retire already claimed. Wait until that one published 0.
        ZForwardingLife::detach_page(region->metadata.fwdRefCount);
        uint64_t spun = TimeUtil::NanoSeconds() - start;
        MutatorRelocate::NoteDrain(site, spun, true);
        return;
    }
    this->region = region;
    if (before > 0) {
        ZForwardingLife::in_place_relocation_claim_page(region->metadata.fwdRefCount);
    }
    uint64_t spun = TimeUtil::NanoSeconds() - start;
    MutatorRelocate::NoteDrain(site, spun, before != 1);
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
