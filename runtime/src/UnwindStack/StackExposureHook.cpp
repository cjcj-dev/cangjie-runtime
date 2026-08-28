// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "UnwindStack/StackExposureHook.h"

#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Mutator/Mutator.h"
#include "StackMap/StackMapTypeDef.h"
#include "UnwindStack/StackFrameCursor.h"

namespace MapleRuntime {
namespace {
std::atomic<size_t> g_fireCount{ 0 };
std::atomic<size_t> g_advanceCount{ 0 };
std::atomic<size_t> g_processOneCount{ 0 };
std::atomic<size_t> g_crossWithoutProcess{ 0 };
std::atomic<size_t> g_stwInHook{ 0 };

// Nesting depth of hook slow path — StopTheWorld probes this.
thread_local int g_hookDepth = 0;
thread_local int g_processRootsDepth = 0;
thread_local RegSlotsMap g_iterationRegs;

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}
} // namespace

bool StackExposureHook::ProductEnabled()
{
    // Correctness hook: product wiring is unconditional.  Verification is
    // separately controlled by MRT_GCV2_STACK_EXPOSURE_VERIFY.
    return true;
}

bool StackExposureHook::VerifyEnabled()
{
    static const bool on = false /* pinned:MRT_GCV2_STACK_EXPOSURE_VERIFY */;
    return on;
}

bool StackExposureHook::NeedsProcess(const StackWatermark& wm, size_t exposingFrameIndex)
{
    // DONE / NOT_STARTED: nothing to protect for the current (or absent) epoch.
    if (!wm.IsScanning()) {
        return false;
    }
    // cursorIndex is exclusive end of processed range (StackWatermark.h).
    // Frame F is processed iff F < cursorIndex. Exposing F needs process when F >= cursor.
    return exposingFrameIndex >= wm.GetCursorIndex();
}

size_t StackExposureHook::NoopProcess(StackWatermark& /*wm*/, size_t /*needUpToExclusive*/)
{
    // Positive control for assertion ②: hook fires but watermark does not advance.
    return 0; // signal "did not advance" — RunSlowPath will not call AdvanceTo
}

size_t StackExposureHook::AdvanceOnlyProcess(StackWatermark& wm, size_t needUpToExclusive)
{
    size_t total = wm.GetFrameCount();
    size_t target = needUpToExclusive;
    if (target > total) {
        target = total;
    }
    size_t prev = wm.GetCursorIndex();
    if (target < prev) {
        target = prev;
    }
    // Owner must already be SELF or GC; harness uses SELF.
    StackWatermark::Owner o = wm.GetOwner();
    if (o == StackWatermark::WM_OWNER_NONE) {
        return prev;
    }
    wm.AdvanceTo(target, o);
    return target;
}

size_t StackExposureHook::ProcessFrameRoots(StackWatermark& wm, size_t needUpToExclusive)
{
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr || !mutator->IsManagedContext() || g_processRootsDepth > 0) {
        return wm.GetCursorIndex();
    }
    ++g_processRootsDepth;
    StackFrameCursor cursor(mutator->GetUnwindContext());
    size_t start = wm.GetCursorIndex();
    if (!cursor.ResumeAt(start, *mutator)) {
        --g_processRootsDepth;
        return start;
    }
    RootVisitor visitor = [](ObjectRef&) {};
    DerivedPtrVisitor derived = [](BasePtrType, DerivedSlot&) {};
    size_t target = needUpToExclusive;
    if (target > cursor.FrameCount()) {
        target = cursor.FrameCount();
    }
    while (cursor.Cursor() < target && !cursor.Done()) {
        (void)cursor.ProcessOne(visitor, *mutator, &derived);
        g_processOneCount.fetch_add(1, std::memory_order_relaxed);
    }
    StackWatermark::Owner o = wm.GetOwner();
    if (o != StackWatermark::WM_OWNER_NONE) {
        wm.AdvanceTo(cursor.Cursor(), o);
    }
    --g_processRootsDepth;
    return cursor.Cursor();
}

bool StackExposureHook::RunSlowPath(StackWatermark& wm, size_t exposingFrameIndex, const ProcessFn& processFn,
                                    const char* site)
{
    if (!NeedsProcess(wm, exposingFrameIndex)) {
        return false;
    }

    g_fireCount.fetch_add(1, std::memory_order_relaxed);
    ++g_hookDepth;

    // Exclusive end that covers exposingFrameIndex.
    size_t needUpTo = exposingFrameIndex + 1;
    size_t before = wm.GetCursorIndex();
    size_t after = processFn(wm, needUpTo);

    if (after > before) {
        // processFn may have Advanced already (AdvanceOnlyProcess) or returned a target.
        if (wm.GetCursorIndex() < after) {
            StackWatermark::Owner o = wm.GetOwner();
            if (o != StackWatermark::WM_OWNER_NONE) {
                wm.AdvanceTo(after, o);
            }
        }
        g_advanceCount.fetch_add(1, std::memory_order_relaxed);
    }

    if (VerifyEnabled()) {
        // After a real process, frame must be covered (or process was intentionally empty).
        if (after > before && NeedsProcess(wm, exposingFrameIndex)) {
            CHECK_DETAIL(false,
                         "[GCV2][stack-exposure] process left frame uncovered site=%s frame=%zu cursor=%zu",
                         site, exposingFrameIndex, wm.GetCursorIndex());
        }
    }

    --g_hookDepth;
    return true;
}

bool StackExposureHook::OnBeforeUnwind(StackWatermark& wm, size_t exposingFrameIndex, const ProcessFn& processFn)
{
    // Product wiring is unconditional; callers provide the processing policy
    // (the product overload binds it to the mutator watermark).
    return RunSlowPath(wm, exposingFrameIndex, processFn, "before_unwind");
}

bool StackExposureHook::OnAfterUnwind(StackWatermark& wm, size_t topFrameIndex, const ProcessFn& processFn)
{
    return RunSlowPath(wm, topFrameIndex, processFn, "after_unwind");
}

bool StackExposureHook::OnBeforeUnwind(Mutator& mutator, size_t exposingFrameIndex)
{
    StackWatermark& wm = mutator.GetStackWatermark();
    if (!wm.IsScanning() || !mutator.IsManagedContext()) {
        return false;
    }
    return OnBeforeUnwind(wm, exposingFrameIndex, ProcessFn(ProcessFrameRoots));
}

bool StackExposureHook::OnAfterUnwind(Mutator& mutator, size_t topFrameIndex)
{
    StackWatermark& wm = mutator.GetStackWatermark();
    if (!wm.IsScanning() || !mutator.IsManagedContext()) {
        return false;
    }
    return OnAfterUnwind(wm, topFrameIndex, ProcessFn(ProcessFrameRoots));
}

bool StackExposureHook::OnIteration(Mutator& mutator, size_t exposingFrameIndex)
{
    return OnBeforeUnwind(mutator, exposingFrameIndex);
}

bool StackExposureHook::OnIteration(Mutator& mutator, size_t exposingFrameIndex, const FrameInfo& frame)
{
    StackWatermark& wm = mutator.GetStackWatermark();
    if (!wm.IsScanning() || !mutator.IsManagedContext()) {
        return false;
    }
    if (!NeedsProcess(wm, exposingFrameIndex)) {
        return false;
    }
    g_fireCount.fetch_add(1, std::memory_order_relaxed);
    ++g_hookDepth;
    if (exposingFrameIndex == 0) {
        g_iterationRegs = RegSlotsMap();
    }
    RootVisitor visitor = [](ObjectRef&) {};
    DerivedPtrVisitor derived = [](BasePtrType, DerivedSlot&) {};
    StackFrameCursor::ProcessFrame(frame, g_iterationRegs, visitor, mutator, &derived);
    g_processOneCount.fetch_add(1, std::memory_order_relaxed);
    StackWatermark::Owner o = wm.GetOwner();
    size_t before = wm.GetCursorIndex();
    size_t after = exposingFrameIndex + 1;
    if (o != StackWatermark::WM_OWNER_NONE && after > before) {
        wm.AdvanceTo(after, o);
        g_advanceCount.fetch_add(1, std::memory_order_relaxed);
    }
    --g_hookDepth;
    return true;
}

bool StackExposureHook::ObserveCrossWithoutProcess(const StackWatermark& wm, size_t exposingFrameIndex)
{
    if (!NeedsProcess(wm, exposingFrameIndex)) {
        return false;
    }
    g_crossWithoutProcess.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void StackExposureHook::NoteStopTheWorldFromHook()
{
    if (g_hookDepth > 0) {
        g_stwInHook.fetch_add(1, std::memory_order_relaxed);
    }
}

size_t StackExposureHook::FireCount() { return g_fireCount.load(std::memory_order_relaxed); }
size_t StackExposureHook::AdvanceCount() { return g_advanceCount.load(std::memory_order_relaxed); }
size_t StackExposureHook::ProcessOneCount() { return g_processOneCount.load(std::memory_order_relaxed); }
size_t StackExposureHook::CrossWithoutProcessCount()
{
    return g_crossWithoutProcess.load(std::memory_order_relaxed);
}
size_t StackExposureHook::StopTheWorldCallsInHook() { return g_stwInHook.load(std::memory_order_relaxed); }

void StackExposureHook::ResetStats()
{
    g_fireCount.store(0, std::memory_order_relaxed);
    g_advanceCount.store(0, std::memory_order_relaxed);
    g_processOneCount.store(0, std::memory_order_relaxed);
    g_crossWithoutProcess.store(0, std::memory_order_relaxed);
    g_stwInHook.store(0, std::memory_order_relaxed);
}

} // namespace MapleRuntime
