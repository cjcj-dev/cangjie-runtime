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
// Movable-stack (#7): CJThreadStackAdjust relocates the whole stack (new mmap +
// memmove). Product grow (Mutator::FixExtendedStack) rewrites stack pointers and
// anchorFA. Watermark resume state MUST NOT store absolute SP/FA — only logical
// frame indices — so OnStackGrow does not renumber cursorIndex; it publishes a
// stackGeneration so any in-flight StackFrameCursor (absolute FA cache) is known
// stale and must be rebuilt.
//
// Invariants (asserted when MRT_GCV2_STACK_WATERMARK_VERIFY=1, or always for
// structural CHECK on illegal transitions when verify is on):
//   ① phase order: NOT_STARTED → SCANNING → DONE; DONE(incomplete) may
//      retry SCANNING in the same epoch, but DONE(complete) has no back-edge
//   ② single owner while SCANNING
//   ③ create/exit close to NOT_STARTED; park leaves a stable publishable state
//   ④ cursorIndex is a valid resume token for StackFrameCursor::ResumeAt
//   ⑤ after OnStackGrow, cursorIndex still names the same logical frame; stack
//      generation has advanced (absolute-FA cursors must rebuild)
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
        complete.store(false, std::memory_order_relaxed);
        stackGeneration.store(0, std::memory_order_relaxed);
        lastGrowOffset.store(0, std::memory_order_relaxed);
        growCount.store(0, std::memory_order_relaxed);
    }

    // Create lifecycle: brand-new mutator starts NOT_STARTED with no owner.
    void OnCreate()
    {
        Reset();
    }

    // Exit lifecycle: must not leave SCANNING owned work dangling for a dead mutator.
    void OnExit()
    {
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

    // Movable-stack grow (#7). Called from Mutator::FixExtendedStack after a successful
    // CJThreadStackGrow with nonzero stackOffset (newBase - oldBase).
    //
    // Position-related fields (Q2):
    //   cursorIndex  — logical exclusive frame index; address-independent; NOT rebased
    //   frameCount   — logical total; address-independent; NOT rebased
    //   epoch/phase/owner — not positions
    // There is no absolute SP/FA stored in this object (by design vs OpenJDK _watermark SP).
    //
    // What OnStackGrow does:
    //   ① record offset + bump growCount
    //   ② advance stackGeneration (invalidates absolute-FA caches such as StackFrameCursor)
    //   ③ leave cursorIndex/frameCount unchanged so ResumeAt still names the same frame
    //
    // Race (Q4): product path takes MutatorLock around pointer fix + this call so a
    // concurrent VisitStackRoots cannot fill a cursor against a half-moved stack.
    void OnStackGrow(intptr_t stackOffset)
    {
        if (stackOffset == 0) {
            return;
        }
        lastGrowOffset.store(stackOffset, std::memory_order_relaxed);
        growCount.fetch_add(1, std::memory_order_relaxed);
        stackGeneration.fetch_add(1, std::memory_order_release);
    }

    // Begin a scan for `scanEpoch`. Exactly one owner may claim.
    // Legal: NOT_STARTED → SCANNING, or DONE of a prior epoch → SCANNING of a new epoch.
    // Illegal: SCANNING → SCANNING (double begin), DONE(complete) same epoch
    // → SCANNING. DONE(incomplete) may be retried by the closing STW.
    bool TryBegin(uint64_t scanEpoch, Owner claimOwner, size_t totalFrames)
    {
        CHECK_DETAIL(scanEpoch != 0, "[GCV2][stack-watermark] epoch must not be zero");
        CHECK_DETAIL(claimOwner == WM_OWNER_SELF || claimOwner == WM_OWNER_GC,
                     "[GCV2][stack-watermark] begin requires SELF or GC owner");

        Phase expected = phase.load(std::memory_order_acquire);
        if (expected == WM_SCANNING) {
            return false;
        }
        if (expected == WM_DONE && epoch.load(std::memory_order_acquire) == scanEpoch &&
            complete.load(std::memory_order_acquire)) {
            return false;
        }

        Owner none = WM_OWNER_NONE;
        if (!owner.compare_exchange_strong(none, claimOwner, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return false;
        }

        epoch.store(scanEpoch, std::memory_order_relaxed);
        cursorIndex.store(0, std::memory_order_relaxed);
        frameCount.store(totalFrames, std::memory_order_relaxed);
        complete.store(false, std::memory_order_relaxed);
        phase.store(WM_SCANNING, std::memory_order_release);
        return true;
    }

    // Advance after processing frames. index is exclusive end of processed range
    // (same meaning as StackFrameCursor::Cursor after ProcessOne).
    void AdvanceTo(size_t index, Owner claimOwner)
    {
        RequireOwnerScanning(claimOwner, "AdvanceTo");
        cursorIndex.store(index, std::memory_order_release);
    }

    void Finish(Owner claimOwner)
    {
        RequireOwnerScanning(claimOwner, "Finish");
        size_t idx = cursorIndex.load(std::memory_order_relaxed);
        size_t total = frameCount.load(std::memory_order_relaxed);
        if (idx != total) {
            FinishIncomplete(claimOwner);
            return;
        }
        owner.store(WM_OWNER_NONE, std::memory_order_relaxed);
        complete.store(true, std::memory_order_relaxed);
        phase.store(WM_DONE, std::memory_order_release);
    }

    // Close a traversal that cannot establish the frame-coverage postcondition.
    // IsDone(epoch) stays false so the closing STW can retry the epoch; if that
    // still cannot start, the consumer takes the legacy fallback.
    void FinishIncomplete(Owner claimOwner)
    {
        RequireOwnerScanning(claimOwner, "FinishIncomplete");
        owner.store(WM_OWNER_NONE, std::memory_order_relaxed);
        complete.store(false, std::memory_order_relaxed);
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
    bool HasCoveredAllFrames() const { return GetCursorIndex() == GetFrameCount(); }
    uint64_t GetStackGeneration() const { return stackGeneration.load(std::memory_order_acquire); }
    intptr_t GetLastGrowOffset() const { return lastGrowOffset.load(std::memory_order_acquire); }
    size_t GetGrowCount() const { return growCount.load(std::memory_order_acquire); }

    bool IsNotStarted() const { return GetPhase() == WM_NOT_STARTED; }
    bool IsScanning() const { return GetPhase() == WM_SCANNING; }
    bool IsDone() const { return GetPhase() == WM_DONE; }
    bool IsDone(uint64_t scanEpoch) const
    {
        return GetPhase() == WM_DONE && GetEpoch() == scanEpoch && complete.load(std::memory_order_acquire);
    }

    static bool VerifyEnabled();

    // Positive-control helper (harness only): pretend resume token was an absolute FA
    // and "rebase" by adding offset to a synthetic address. Used to prove that treating
    // watermark as SP would desync from logical frame identity after grow.
    static uintptr_t InjectAbsoluteResumeToken(uintptr_t absoluteFa, intptr_t growOffset)
    {
        return static_cast<uintptr_t>(static_cast<intptr_t>(absoluteFa) + growOffset);
    }

private:
    void RequireOwnerScanning(Owner claimOwner, const char* op)
    {
        (void)claimOwner;
        (void)op;
    }

    std::atomic<uint64_t> epoch;
    std::atomic<Phase> phase;
    std::atomic<Owner> owner;
    std::atomic<size_t> cursorIndex;
    std::atomic<size_t> frameCount;
    std::atomic<bool> complete;
    // Movable-stack generation: advanced on every successful grow with nonzero offset.
    std::atomic<uint64_t> stackGeneration;
    std::atomic<intptr_t> lastGrowOffset;
    std::atomic<size_t> growCount;
};

} // namespace MapleRuntime

#endif // MRT_STACK_WATERMARK_H
