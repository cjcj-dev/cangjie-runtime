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

    // Copier admission and the in-flight count share one atomic word so a
    // drain cannot observe "open + zero", return, and then lose a copier that
    // already owns the object lock. The ENTERING state covers the short
    // TryLockObject-success -> count-publication interval. The low 30 bits are
    // the number of admitted copiers.
    enum class CopyAdmissionState : uint8_t {
        OPEN = 0,
        ENTERING = 1,
        SEALED = 2,
    };

    static constexpr int32_t CopyAdmissionOpenWord() { return 0; }
    static constexpr int32_t CopyAdmissionSealedWord()
    {
        return static_cast<int32_t>(uint32_t{ 2 } << 30);
    }

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
                // Try-lock: refuse claimed pages immediately. Waiting here deadlocks
                // when this thread already holds a retain on the same count
                // (TryMutatorRelocate retain + PlanRoute nested RetainScope) while
                // DrainScope inverted n→-n and WaitUntilRef(-1). ZGC's retain_page
                // waits because the caller does not already pin; our mutator path
                // is a try-lock (WCollector.cpp:9976-9977). zForwarding.cpp:95-100.
                g_retainRefusedClaimed.fetch_add(1, std::memory_order_relaxed);
                (void)done;
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

    static void reset_copy_open(std::atomic<int32_t>& copyWord)
    {
        copyWord.store(CopyAdmissionOpenWord(), std::memory_order_release);
    }

    static void reset_copy_sealed(std::atomic<int32_t>& copyWord)
    {
        copyWord.store(CopyAdmissionSealedWord(), std::memory_order_release);
        NotifyAll();
    }

    // First half of copier admission. Called immediately after TryLockObject.
    // ENTERING is visible before any test hook or other work in that interval,
    // so wait_copied must either precede this CAS or wait for its resolution.
    static bool begin_copy(std::atomic<int32_t>& copyWord)
    {
        for (;;) {
            int32_t word = copyWord.load(std::memory_order_acquire);
            switch (copy_admission_state(word)) {
                case CopyAdmissionState::SEALED:
                    return false;
                case CopyAdmissionState::ENTERING:
                    WaitUntilCopyAdmissionSettled(copyWord);
                    continue;
                case CopyAdmissionState::OPEN: {
                    const int32_t entering = PackCopyWord(CopyAdmissionState::ENTERING, copy_count(word));
                    if (copyWord.compare_exchange_weak(
                            word, entering, std::memory_order_acq_rel, std::memory_order_acquire)) {
                        return true;
                    }
                    continue;
                }
            }
        }
    }

    // Second half of copier admission. Existing copiers may finish while this
    // thread owns ENTERING, so publish OPEN + (latest count + 1) with a CAS.
    static void commit_copy(std::atomic<int32_t>& copyWord)
    {
        for (;;) {
            int32_t word = copyWord.load(std::memory_order_acquire);
            CHECK(copy_admission_state(word) == CopyAdmissionState::ENTERING);
            const int32_t count = copy_count(word);
            CHECK(count < static_cast<int32_t>(kCopyCountMask));
            const int32_t admitted = PackCopyWord(CopyAdmissionState::OPEN, count + 1);
            if (copyWord.compare_exchange_weak(
                    word, admitted, std::memory_order_release, std::memory_order_acquire)) {
                NotifyAll();
                return;
            }
        }
    }

    // Convenience for already-locked product entries and focused unit tests.
    // A false answer leaves the count unchanged and requires the caller to
    // roll the object lock back instead of copying.
    static bool note_copy(std::atomic<int32_t>& copyWord)
    {
        if (!begin_copy(copyWord)) {
            return false;
        }
        commit_copy(copyWord);
        return true;
    }

    static void end_copy(std::atomic<int32_t>& copyWord)
    {
        for (;;) {
            int32_t word = copyWord.load(std::memory_order_acquire);
            const CopyAdmissionState state = copy_admission_state(word);
            CHECK(state != CopyAdmissionState::ENTERING);
            const int32_t count = copy_count(word);
            CHECK(count > 0);
            const int32_t ended = PackCopyWord(state, count - 1);
            if (!copyWord.compare_exchange_weak(
                    word, ended, std::memory_order_acq_rel, std::memory_order_acquire)) {
                continue;
            }
            if (count == 1) {
                NotifyAll();
            }
            return;
        }
    }

    // Linearization point for payload retirement. OPEN->SEALED closes future
    // admissions; ENTERING means a copier already owns an object lock and must
    // finish admission before the drain can close the gate. SEALED remains
    // terminal until the next forwarding life explicitly calls reset_copy_open.
    static void wait_copied(std::atomic<int32_t>& copyWord);

    static CopyAdmissionState copy_admission_state(const std::atomic<int32_t>& copyWord)
    {
        return copy_admission_state(copyWord.load(std::memory_order_acquire));
    }

    static int32_t copy_count(const std::atomic<int32_t>& copyWord)
    {
        return copy_count(copyWord.load(std::memory_order_acquire));
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
    static constexpr uint32_t kCopyCountMask = (uint32_t{ 1 } << 30) - 1;
    static constexpr uint32_t kCopyStateShift = 30;

    static CopyAdmissionState copy_admission_state(int32_t word)
    {
        return static_cast<CopyAdmissionState>(static_cast<uint32_t>(word) >> kCopyStateShift);
    }

    static int32_t copy_count(int32_t word)
    {
        return static_cast<int32_t>(static_cast<uint32_t>(word) & kCopyCountMask);
    }

    static int32_t PackCopyWord(CopyAdmissionState state, int32_t count)
    {
        CHECK(count >= 0 && static_cast<uint32_t>(count) <= kCopyCountMask);
        const uint32_t bits = (static_cast<uint32_t>(state) << kCopyStateShift) |
            static_cast<uint32_t>(count);
        return static_cast<int32_t>(bits);
    }

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
    static void WaitUntilCopyAdmissionSettled(std::atomic<int32_t>& copyWord);

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
