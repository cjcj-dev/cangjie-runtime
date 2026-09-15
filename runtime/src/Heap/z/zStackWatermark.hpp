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

// Per-coroutine phase processing state (zStackWatermark.cpp:175-215).
// Mark and remap have separate completion identities. The phase handshake
// eagerly drains the stack before it can resume; movable stacks retain logical
// frame indices instead of absolute stack addresses.
//   ZGC state/epoch  -> processingPhase, epoch, phase
//   ZGC watermark    -> cursorIndex
//   processing owner -> SELF or GC under MutatorLock
//
// Movable-stack (#7): CJThreadStackAdjust relocates the whole stack (new mmap +
// memmove). Product grow (Mutator::FixExtendedStack) rewrites stack pointers and
// anchorFA. Watermark resume state MUST NOT store absolute SP/FA — only logical
// frame indices — so OnStackGrow does not renumber cursorIndex; it publishes a
// stackGeneration so any in-flight StackFrameCursor (absolute FA cache) is known
// stale and must be rebuilt.
//
class StackWatermark {
public:
    enum Phase : uint32_t {
        WM_NOT_STARTED = 0,
        WM_SCANNING = 1,
        WM_DONE = 2,
    };

    enum class ProcessingPhase : uint32_t { MARK, REMAP };

    enum Owner : uint32_t {
        WM_OWNER_NONE = 0,
        WM_OWNER_SELF = 1,
        WM_OWNER_GC = 2,
    };

    StackWatermark();

    void Reset()
    {
        epoch.store(0, std::memory_order_relaxed);
        processingPhase.store(ProcessingPhase::MARK, std::memory_order_relaxed);
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
        (void)growCount.fetch_add(1, std::memory_order_relaxed);
        // Release so a reader that observes generation N+1 also sees cursor/offset.
        (void)stackGeneration.fetch_add(1, std::memory_order_release);
    }

    // Begin a scan for `scanEpoch`. Exactly one owner may claim.
    // Legal: NOT_STARTED → SCANNING, or DONE of a prior epoch → SCANNING of a new epoch.
    // Illegal: SCANNING → SCANNING (double begin), DONE(complete) same epoch
    // → SCANNING. DONE(incomplete) may be retried by the closing STW.
    bool TryBegin(uint64_t scanEpoch, Owner claimOwner, size_t totalFrames,
                  ProcessingPhase workPhase = ProcessingPhase::MARK)
    {
        CHECK_DETAIL(scanEpoch != 0, "[GCV2][stack-watermark] epoch must not be zero");
        CHECK_DETAIL(claimOwner == WM_OWNER_SELF || claimOwner == WM_OWNER_GC,
                     "[GCV2][stack-watermark] begin requires SELF or GC owner");

        Phase expected = phase.load(std::memory_order_acquire);
        if (expected == WM_SCANNING) {
            return false;
        }
        if (expected == WM_DONE && processingPhase.load(std::memory_order_acquire) == workPhase &&
            epoch.load(std::memory_order_acquire) == scanEpoch &&
            complete.load(std::memory_order_acquire)) {
            return false;
        }

        // Claim owner first (must be NONE).
        Owner none = WM_OWNER_NONE;
        if (!owner.compare_exchange_strong(none, claimOwner, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return false;
        }

        processingPhase.store(workPhase, std::memory_order_relaxed);
        epoch.store(scanEpoch, std::memory_order_relaxed);
        cursorIndex.store(0, std::memory_order_relaxed);
        frameCount.store(totalFrames, std::memory_order_relaxed);
        complete.store(false, std::memory_order_relaxed);
        phase.store(WM_SCANNING, std::memory_order_release);
        return true;
    }

    // Advance after processing frames. index is exclusive end of processed range
    // (same meaning as StackFrameCursor::Cursor after ProcessOne).
    void AdvanceTo(size_t index, Owner)
    {
        cursorIndex.store(index, std::memory_order_release);
    }

    void Finish(Owner claimOwner)
    {
        size_t idx = cursorIndex.load(std::memory_order_relaxed);
        size_t total = frameCount.load(std::memory_order_relaxed);
        if (idx != total) {

            // A partial traversal remains incomplete so the closing pause can retry it.
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
    void FinishIncomplete(Owner)
    {
        owner.store(WM_OWNER_NONE, std::memory_order_relaxed);
        complete.store(false, std::memory_order_relaxed);
        phase.store(WM_DONE, std::memory_order_release);
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
    bool IsDone() const;
    bool IsDone(uint64_t scanEpoch, ProcessingPhase workPhase = ProcessingPhase::MARK) const;
private:

    std::atomic<ProcessingPhase> processingPhase;
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
