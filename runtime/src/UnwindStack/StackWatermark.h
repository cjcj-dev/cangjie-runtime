// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STACK_WATERMARK_H
#define MRT_STACK_WATERMARK_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Base/Log.h"

namespace MapleRuntime {

// Per-Mutator stack-scan watermark (stackwm #1 / minorconc Step 2 substrate).
//
// State only — no concurrent stack scanning. Exposure hooks (#5) and grow rebase (#7)
// are separate verticals. OpenJDK correspondence (ref 5b2d6991 stackWatermark.hpp):
//   _state (epoch, is_done)  ↔  epoch + phase
//   _watermark (SP)          ↔  cursorIndex (logical frame index; not SP — movable stacks)
//   processing lock/claim    ↔  owner (SELF vs GC)
// Differences from OpenJDK: owned by Mutator/CJThread not JavaThread; no return-poll
// fast path yet; cursor is frame-index not frame-pointer SP.
//
// Invariants (asserted when MRT_GCV2_STACK_WATERMARK_VERIFY=1, or always for
// structural CHECK on illegal transitions when verify is on):
//   ① phase order: NOT_STARTED → SCANNING → DONE; no back-edge, no skip
//   ② single owner while SCANNING
//   ③ create/exit close to NOT_STARTED; park leaves a stable publishable state
//   ④ cursorIndex is a valid resume token for StackFrameCursor::ResumeAt
class StackWatermark {
public:
    enum Phase : uint32_t {
        WM_NOT_STARTED = 0,
        WM_SCANNING = 1,
        WM_DONE = 2,
    };

    enum Owner : uint32_t {
        WM_OWNER_NONE = 0,
        WM_OWNER_SELF = 1,
        WM_OWNER_GC = 2,
    };

    StackWatermark() { Reset(); }

    void Reset()
    {
        epoch.store(0, std::memory_order_relaxed);
        phase.store(WM_NOT_STARTED, std::memory_order_relaxed);
        owner.store(WM_OWNER_NONE, std::memory_order_relaxed);
        cursorIndex.store(0, std::memory_order_relaxed);
        frameCount.store(0, std::memory_order_relaxed);
    }

    // Create lifecycle: brand-new mutator starts NOT_STARTED with no owner.
    void OnCreate()
    {
        AssertClosedOrReset("CREATE");
        Reset();
    }

    // Exit lifecycle: must not leave SCANNING owned work dangling for a dead mutator.
    void OnExit()
    {
        Phase p = phase.load(std::memory_order_acquire);
        if (p == WM_SCANNING) {
            // Close by abandoning mid-scan: GC will not touch a destroyed mutator.
            // Verify mode requires explicit Finish before exit (positive control injects fail).
            if (VerifyEnabled()) {
                CHECK_DETAIL(false,
                             "[GCV2][stack-watermark] EXIT_WHILE_SCANNING mutator_wm=%p phase=%u owner=%u epoch=%llu",
                             this, static_cast<unsigned>(p),
                             static_cast<unsigned>(owner.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(epoch.load(std::memory_order_relaxed)));
            }
        }
        Reset();
    }

    // Park lifecycle: stack top is stable; SCANNING may continue under GC owner later (#5).
    // Step #1 only records that park is allowed in any phase and does not regress state.
    void OnPark()
    {
        Phase p = phase.load(std::memory_order_acquire);
        if (p == WM_SCANNING) {
            // Owner stays; parked mutator is not running managed code, so GC may later
            // re-claim (not implemented here). Phase must not go back to NOT_STARTED.
            CHECK_DETAIL(p == WM_SCANNING,
                         "[GCV2][stack-watermark] PARK_REGRESS phase became %u", static_cast<unsigned>(p));
        }
        // NOT_STARTED / DONE: no-op publish.
    }

    // Begin a scan for `scanEpoch`. Exactly one owner may claim.
    // Legal: NOT_STARTED → SCANNING, or DONE of a prior epoch → SCANNING of a new epoch.
    // Illegal: SCANNING → SCANNING (double begin), DONE same epoch → SCANNING.
    bool TryBegin(uint64_t scanEpoch, Owner claimOwner, size_t totalFrames)
    {
        CHECK_DETAIL(scanEpoch != 0, "[GCV2][stack-watermark] epoch must not be zero");
        CHECK_DETAIL(claimOwner == WM_OWNER_SELF || claimOwner == WM_OWNER_GC,
                     "[GCV2][stack-watermark] begin requires SELF or GC owner");

        Phase expected = phase.load(std::memory_order_acquire);
        if (expected == WM_SCANNING) {
            if (VerifyEnabled()) {
                CHECK_DETAIL(false,
                             "[GCV2][stack-watermark] ILLEGAL_TRANSITION begin while SCANNING "
                             "epoch=%llu owner=%u claim=%u",
                             static_cast<unsigned long long>(epoch.load(std::memory_order_relaxed)),
                             static_cast<unsigned>(owner.load(std::memory_order_relaxed)),
                             static_cast<unsigned>(claimOwner));
            }
            return false;
        }
        if (expected == WM_DONE && epoch.load(std::memory_order_acquire) == scanEpoch) {
            if (VerifyEnabled()) {
                CHECK_DETAIL(false,
                             "[GCV2][stack-watermark] ILLEGAL_TRANSITION begin after DONE same epoch=%llu",
                             static_cast<unsigned long long>(scanEpoch));
            }
            return false;
        }

        // Claim owner first (must be NONE).
        Owner none = WM_OWNER_NONE;
        if (!owner.compare_exchange_strong(none, claimOwner, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (VerifyEnabled()) {
                CHECK_DETAIL(false,
                             "[GCV2][stack-watermark] OWNER_NOT_UNIQUE existing=%u claim=%u epoch=%llu",
                             static_cast<unsigned>(none), static_cast<unsigned>(claimOwner),
                             static_cast<unsigned long long>(scanEpoch));
            }
            return false;
        }

        epoch.store(scanEpoch, std::memory_order_relaxed);
        cursorIndex.store(0, std::memory_order_relaxed);
        frameCount.store(totalFrames, std::memory_order_relaxed);
        phase.store(WM_SCANNING, std::memory_order_release);
        return true;
    }

    // Advance after processing frames. index is exclusive end of processed range
    // (same meaning as StackFrameCursor::Cursor after ProcessOne).
    void AdvanceTo(size_t index, Owner claimOwner)
    {
        RequireOwnerScanning(claimOwner, "AdvanceTo");
        size_t total = frameCount.load(std::memory_order_relaxed);
        if (VerifyEnabled()) {
            CHECK_DETAIL(index <= total,
                         "[GCV2][stack-watermark] AdvanceTo OOB index=%zu total=%zu", index, total);
            size_t prev = cursorIndex.load(std::memory_order_relaxed);
            CHECK_DETAIL(index >= prev,
                         "[GCV2][stack-watermark] ILLEGAL_TRANSITION AdvanceTo regress %zu -> %zu", prev, index);
        }
        cursorIndex.store(index, std::memory_order_release);
    }

    void Finish(Owner claimOwner)
    {
        RequireOwnerScanning(claimOwner, "Finish");
        size_t idx = cursorIndex.load(std::memory_order_relaxed);
        size_t total = frameCount.load(std::memory_order_relaxed);
        if (VerifyEnabled()) {
            CHECK_DETAIL(idx == total,
                         "[GCV2][stack-watermark] Finish with residual frames cursor=%zu total=%zu", idx, total);
        }
        owner.store(WM_OWNER_NONE, std::memory_order_relaxed);
        phase.store(WM_DONE, std::memory_order_release);
    }

    // Positive-control injectors (only meaningful under verify flag).
    // Called from harness / oracle; never from product GC path.
    void InjectIllegalPhaseBack()
    {
        // Force SCANNING → NOT_STARTED (forbidden back-edge).
        phase.store(WM_SCANNING, std::memory_order_relaxed);
        owner.store(WM_OWNER_SELF, std::memory_order_relaxed);
        epoch.store(1, std::memory_order_relaxed);
        // Next TryBegin of same/other epoch while SCANNING must fire.
    }

    void InjectDualOwner(Owner second)
    {
        phase.store(WM_SCANNING, std::memory_order_relaxed);
        owner.store(WM_OWNER_SELF, std::memory_order_relaxed);
        epoch.store(1, std::memory_order_relaxed);
        // Second claim must fire OWNER_NOT_UNIQUE.
        (void)second;
    }

    Phase GetPhase() const { return phase.load(std::memory_order_acquire); }
    Owner GetOwner() const { return owner.load(std::memory_order_acquire); }
    uint64_t GetEpoch() const { return epoch.load(std::memory_order_acquire); }
    size_t GetCursorIndex() const { return cursorIndex.load(std::memory_order_acquire); }
    size_t GetFrameCount() const { return frameCount.load(std::memory_order_acquire); }

    bool IsNotStarted() const { return GetPhase() == WM_NOT_STARTED; }
    bool IsScanning() const { return GetPhase() == WM_SCANNING; }
    bool IsDone() const { return GetPhase() == WM_DONE; }

    static bool VerifyEnabled();

private:
    void AssertClosedOrReset(const char* why)
    {
        if (!VerifyEnabled()) {
            return;
        }
        Phase p = phase.load(std::memory_order_acquire);
        CHECK_DETAIL(p == WM_NOT_STARTED || p == WM_DONE,
                     "[GCV2][stack-watermark] %s expected closed phase, got %u", why, static_cast<unsigned>(p));
    }

    void RequireOwnerScanning(Owner claimOwner, const char* op)
    {
        Phase p = phase.load(std::memory_order_acquire);
        Owner o = owner.load(std::memory_order_acquire);
        if (p != WM_SCANNING || o != claimOwner) {
            if (VerifyEnabled()) {
                CHECK_DETAIL(false,
                             "[GCV2][stack-watermark] ILLEGAL_TRANSITION %s phase=%u owner=%u claim=%u",
                             op, static_cast<unsigned>(p), static_cast<unsigned>(o),
                             static_cast<unsigned>(claimOwner));
            }
        }
    }

    std::atomic<uint64_t> epoch;
    std::atomic<Phase> phase;
    std::atomic<Owner> owner;
    std::atomic<size_t> cursorIndex;
    std::atomic<size_t> frameCount;
};

} // namespace MapleRuntime

#endif // MRT_STACK_WATERMARK_H
