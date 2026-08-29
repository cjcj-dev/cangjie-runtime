// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STACK_EXPOSURE_HOOK_H
#define MRT_STACK_EXPOSURE_HOOK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "UnwindStack/StackWatermark.h"

namespace MapleRuntime {

// Frame exposure hooks (stackwm #5).
//
// OpenJDK correspondence (ref 5b2d6991 stackWatermark.inline.hpp):
//   before_unwind / ensure_safe  ↔  OnBeforeUnwind / NeedsProcess
//   after_unwind                 ↔  OnAfterUnwind
//   process_one on cross         ↔  ProcessFn advances watermark past exposed frame
//
// Differences from OpenJDK (must not copy-paste):
//   - boundary is logical frame index (cursorIndex), not SP/FP
//   - owner is Mutator/CJThread, not JavaThread
//   - product codegen has entry/backedge safepoint polls only
//     (PlaceSafepoints: no return statepoints) ⇒ return-every-frame is not free;
//     this API is the runtime half; compiler return-poll is a later vertical
//   - process work is supplied by caller (cursor ProcessOne or harness stub);
//     this unit does not itself walk a managed stack
//
// Invariant (same as OpenJDK is_frame_safe shape, adapted to indices):
//   A frame at index F may be exposed only if watermark.cursorIndex > F
//   (exclusive end of processed range covers F), or phase is DONE / not active.
//
// Default product path: hooks are no-ops unless MRT_GCV2_STACK_EXPOSURE_HOOK=1.
// Structural CHECKs / counters need MRT_GCV2_STACK_EXPOSURE_VERIFY=1.
class StackExposureHook {
public:
    // processFn(wm, needUpToExclusive) must process frames so that after return
    // wm.cursorIndex >= needUpToExclusive. It must not call StopTheWorld.
    // Returns the new exclusive cursor index actually reached.
    using ProcessFn = std::function<size_t(StackWatermark& wm, size_t needUpToExclusive)>;

    // True when an exposure of `exposingFrameIndex` would cross the watermark
    // (frame not yet covered by cursorIndex while SCANNING).
    static bool NeedsProcess(const StackWatermark& wm, size_t exposingFrameIndex);

    // OpenJDK before_unwind: about to expose the caller of the returning frame.
    // If NeedsProcess, runs processFn and AdvanceTo; records counters under verify.
    // Returns true if the slow path ran (hook "fired").
    static bool OnBeforeUnwind(StackWatermark& wm, size_t exposingFrameIndex, const ProcessFn& processFn);

    // OpenJDK after_unwind: top frame just became current; ensure it is safe.
    static bool OnAfterUnwind(StackWatermark& wm, size_t topFrameIndex, const ProcessFn& processFn);

    // Empty process: records fire but does not advance watermark (positive control for ②).
    static size_t NoopProcess(StackWatermark& wm, size_t needUpToExclusive);

    // Default process for structural tests: AdvanceTo(needUpToExclusive) under SELF owner.
    static size_t AdvanceOnlyProcess(StackWatermark& wm, size_t needUpToExclusive);

    // Gates (default off).
    static bool ProductEnabled(); // MRT_GCV2_STACK_EXPOSURE_HOOK=1
    static bool VerifyEnabled();  // MRT_GCV2_STACK_EXPOSURE_VERIFY=1

    // Counters (verify / harness).
    static size_t FireCount();
    static size_t AdvanceCount();
    static size_t CrossWithoutProcessCount(); // observed cross with process disabled/empty
    static size_t StopTheWorldCallsInHook();
    static void ResetStats();

    // Called from product paths that must not STW; increments if someone nested STW.
    static void NoteStopTheWorldFromHook();

    // Simulate "return to unscanned frame" observation without processing (positive for ①).
    // Only legal under verify; increments CrossWithoutProcessCount when NeedsProcess.
    static bool ObserveCrossWithoutProcess(const StackWatermark& wm, size_t exposingFrameIndex);

private:
    static bool RunSlowPath(StackWatermark& wm, size_t exposingFrameIndex, const ProcessFn& processFn,
                            const char* site);
};

} // namespace MapleRuntime

#endif // MRT_STACK_EXPOSURE_HOOK_H
