// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_FORWARDING_LIFE_H
#define MRT_Z_FORWARDING_LIFE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "Base/Log.h"

namespace MapleRuntime {

// ZForwarding's four-piece lifetime (zForwarding.hpp:66-69, zForwarding.cpp:34-194).
//
//   Atomic<bool>    _claimed     claim()
//   ZConditionLock  _ref_lock    wait / notify on the three-state count
//   Atomic<int32_t> _ref_count   retain_page / release_page / claim invert / detach wait
//   Atomic<bool>    _done        mark_done / is_done
//
// The three-state count is the ABA answer: a late reader is refused, it is never
// handed a reused table. 0 is terminal. <0 is claimed (in-place relocate). >0 is live.
//
// Always on. This is the mechanism that replaced MRT_GCV2_FWDDATA_GRACE and
// MRT_GCV2_MUTRELOC_DRAIN, not another gate.
//
// The three atomics live in RegionInfo::UnitMetadata (they fit the padding after
// routeDestHold). The lock is process-wide: waiters are rare (claim drain, detach,
// a late retain that arrived during claim) and ZGC's notify is already a broadcast.
class ZForwardingLife {
public:
    ZForwardingLife() = delete;

    // zForwarding.inline.hpp:67-70 -- constructed with claimed=false, ref=1, done=false.
    // The construction 1 is the relocating worker's token; it is dropped at retire.
    static void ResetForForwarding(std::atomic<int32_t>& refCount, std::atomic<bool>& claimed,
                                   std::atomic<bool>& done)
    {
        claimed.store(false, std::memory_order_relaxed);
        done.store(false, std::memory_order_relaxed);
        refCount.store(1, std::memory_order_release);
    }

    // Region reuse / never-a-forwarding. 0 is terminal: retain_page refuses.
    // Store 0 then notify: a waiter in WaitUntilDone (n<0 claimed) must observe
    // the idle transition. ZGC never reuses a ZForwarding waiters still sit on;
    // we ResetIdle in place (ExpireKeptPublish / InitRegionInfo), so the
    // predicate is n==0 as well as is_done (zForwarding.cpp:96-100 add_and_wait
    // only watches is_done because the object is not reset under them).
    static void ResetIdle(std::atomic<int32_t>& refCount, std::atomic<bool>& claimed, std::atomic<bool>& done)
    {
        claimed.store(false, std::memory_order_relaxed);
        refCount.store(0, std::memory_order_release);
        done.store(false, std::memory_order_release);
        NotifyAll();
    }

    // zForwarding.cpp:51-53
    static bool claim(std::atomic<bool>& claimed)
    {
        bool expected = false;
        return claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    // zForwarding.cpp:188-194
    static void mark_done(std::atomic<bool>& done)
    {
        done.store(true, std::memory_order_release);
        NotifyAll();
    }

    static bool is_done(const std::atomic<bool>& done) { return done.load(std::memory_order_acquire); }

    // zForwarding.cpp:86-108. queue->add_and_wait is inlined: wait until is_done, then refuse.
    // A late reader therefore never observes a reused from-page.
    static bool retain_page(std::atomic<int32_t>& refCount, const std::atomic<bool>& done)
    {
        for (;;) {
            int32_t n = refCount.load(std::memory_order_acquire);
            if (n == 0) {
                g_retainRefusedReleased.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (n < 0) {
                g_retainRefusedClaimed.fetch_add(1, std::memory_order_relaxed);
                WaitUntilDone(refCount, done);
                return false;
            }
            if (refCount.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
    }

    // zForwarding.cpp:134-169
    static void release_page(std::atomic<int32_t>& refCount)
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

    // zForwarding.cpp:110-131 -- invert n → -n, then wait until -1.
    static void in_place_relocation_claim_page(std::atomic<int32_t>& refCount)
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

    // zForwarding.cpp:171-181 -- block until the count is 0, then the page may be freed.
    static void detach_page(std::atomic<int32_t>& refCount)
    {
        if (refCount.load(std::memory_order_acquire) == 0) {
            return;
        }
        g_detachWaited.fetch_add(1, std::memory_order_relaxed);
        WaitUntilRef(refCount, 0);
    }

    // In-flight copier token, distinct from retain_page (construction starts at 1).
    // TryLockObject success (this thread will copy) → note_copy; every UnlockObject
    // exit (publish FORWARDED or rollback NORMAL) → end_copy. find() hits do not
    // enter (zRelocate.cpp:382-410). Exempt waits wait_copied, never walks the page
    // (VisitAllObjects holes miss LOCKED; REPORT-exemptlife §4 B2.3/B2.4).
    static void note_copy(std::atomic<int32_t>& copyCount)
    {
        copyCount.fetch_add(1, std::memory_order_acq_rel);
    }

    static void end_copy(std::atomic<int32_t>& copyCount)
    {
        const int32_t n = copyCount.fetch_sub(1, std::memory_order_acq_rel);
        CHECK(n > 0);
        if (n == 1) {
            NotifyAll();
        }
    }

    static void wait_copied(std::atomic<int32_t>& copyCount)
    {
        WaitUntilRef(copyCount, 0);
    }

    static uint64_t RetainRefusedReleased()
    {
        return g_retainRefusedReleased.load(std::memory_order_relaxed);
    }
    static uint64_t RetainRefusedClaimed()
    {
        return g_retainRefusedClaimed.load(std::memory_order_relaxed);
    }
    static uint64_t DetachWaited() { return g_detachWaited.load(std::memory_order_relaxed); }

private:
    struct Monitor {
        std::mutex mu;
        std::condition_variable cv;
    };

    static Monitor& Lock()
    {
        static Monitor m;
        return m;
    }

    // Hold the mutex across notify so a waiter that has observed the old count
    // but not yet entered wait cannot miss the signal. Same as ZGC's
    // ZLocker<ZConditionLock> around notify_all (zForwarding.cpp:149,163).
    static void NotifyAll()
    {
        std::lock_guard<std::mutex> guard(Lock().mu);
        Lock().cv.notify_all();
    }

    static void WaitUntilRef(std::atomic<int32_t>& refCount, int32_t expect);

    // zForwarding.cpp:96-100: wait until is_done, then refuse. Also exit on
    // ref==0 (ResetIdle / detach) — ZGC destroys the forwarding instead.
    // Defined in ZForwardingLife.cpp so the waiter can enter a saferegion
    // (mutator cv.wait without it blocks STW: all-futex fifth face).
    static void WaitUntilDone(std::atomic<int32_t>& refCount, const std::atomic<bool>& done);

    static std::atomic<uint64_t> g_retainRefusedReleased;
    static std::atomic<uint64_t> g_retainRefusedClaimed;
    static std::atomic<uint64_t> g_detachWaited;
};

} // namespace MapleRuntime

#endif // MRT_Z_FORWARDING_LIFE_H
