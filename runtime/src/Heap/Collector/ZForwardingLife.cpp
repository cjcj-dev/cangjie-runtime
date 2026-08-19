// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/ZForwardingLife.h"

#include <cstdio>
#include <cstdlib>

#include "Common/ScopedObjectAccess.h"

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
    // Mutator must be in a saferegion: cv.wait is a handshake point.
    // Without it STW waits for this thread while this thread waits for a
    // publisher that only runs after STW (fifth-face all-futex hang).
    ScopedEnterSaferegion enterSaferegion(true);
    std::unique_lock<std::mutex> guard(Lock().mu);
    while (refCount.load(std::memory_order_acquire) != expect) {
        Lock().cv.wait(guard);
    }
}

void ZForwardingLife::WaitUntilDone(std::atomic<int32_t>& refCount, const std::atomic<bool>& done)
{
    // zForwarding.cpp:96-100 add_and_wait: wait until is_done. Also treat
    // ref==0 as terminal — ResetIdle / InitRegionInfo reuse the same
    // ZForwardingLife words in place (ZGC destroys the forwarding).
    if (done.load(std::memory_order_acquire) || refCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    ScopedEnterSaferegion enterSaferegion(true);
    std::unique_lock<std::mutex> guard(Lock().mu);
    while (!done.load(std::memory_order_acquire) && refCount.load(std::memory_order_acquire) != 0) {
        Lock().cv.wait(guard);
    }
}

} // namespace MapleRuntime
