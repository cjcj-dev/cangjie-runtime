// Copyright (c) Huawei Technologies Co. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// ZGC stackWatermark.inline.hpp:70-71,127-131: on_iteration assumes processing
// has already been started by on_safepoint (stackWatermark.cpp:311-318) or by
// the STW safepoint / handshake paths (stackWatermarkSet.cpp:121-130,163-170).
// A diagnostic stack walk is not one of those entries, so it must not advance
// the owner's epoch. The product decision point is
// StackInfo::ProcessOnIteration (runtime/src/UnwindStack/StackInfo.cpp), which
// used to call StackWatermarkSet::start_processing unconditionally.
#include <cstdio>
#include <thread>

#include "b09_runtime_fixture.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "UnwindStack/PrintStackInfo.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
// The traversal under test is the product StackInfo::ProcessOnIteration; the
// driver only exposes the protected entry the product's own FillInStackTrace
// calls, so no re-implementation of the mechanism lives in the test.
class IterationDriver : public PrintStackInfo {
public:
    void WalkOneFrame(Mutator& owner, const FrameInfo& frame)
    {
        SetProcessingOwner(&owner);
        ProcessOnIteration(frame);
    }
};
} // namespace

GC_OTHER_VM_TEST(WatermarkIteration, DiagnosticWalkLeavesEpochUnstarted)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    bool precondition = false;
    bool targetHeld = false;
    bool controlStarted = false;
    std::thread thread([&] {
        auto& manager = MutatorManager::Instance();
        auto* owner = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        auto& watermark = owner->GetStackWatermark();
        // A real phase flip republishes the store-good colour before
        // safepoint_synchronize_begin (stackWatermarkSet.cpp:163-170) starts
        // processing, which leaves the owner registered on the previous epoch.
        const uintptr_t published = ::g_cjStoreGoodMask;
        ::g_cjStoreGoodMask = published | ZPointerRememberedMask;
        const bool stale = !watermark.processing_started();
        const uint32_t stateBefore = watermark.PackedState();
        const uintptr_t markBefore = watermark.watermark();
        const uintptr_t installedBefore = owner->GetGCData().storeGoodMask;

        FrameInfo frame { MachineFrame(nullptr, nullptr), FrameType::MANAGED };
        IterationDriver driver;
        driver.WalkOneFrame(*owner, frame);

        // Target invariant: the walk neither started processing nor published
        // the phase's masks on the owner.
        const bool stillStale = !watermark.processing_started();
        const bool stateSame = watermark.PackedState() == stateBefore;
        const bool noPhaseEffect = watermark.watermark() == markBefore &&
            owner->GetGCData().storeGoodMask == installedBefore;
        std::fprintf(stderr,
            "WM_ITERATION_TARGET executed=1 stale=%d still_stale=%d state_same=%d no_phase_effect=%d "
            "epoch_before=%u epoch_after=%u\n",
            stale, stillStale, stateSame, noPhaseEffect, StackWatermark::UnpackEpoch(stateBefore),
            watermark.GetEpoch());
        precondition = stale;
        targetHeld = stillStale && stateSame && noPhaseEffect;

        // Positive control: the legal safepoint entry does start processing on
        // the same owner, and the epoch it publishes is the current one, so the
        // state read by the target assertion is not a constant.
        StackWatermarkSet::on_safepoint(*owner);
        const bool started = watermark.processing_started();
        const bool advanced = watermark.GetEpoch() != StackWatermark::UnpackEpoch(stateBefore);
        std::fprintf(stderr, "WM_ITERATION_CONTROL executed=1 started=%d epoch_advanced=%d epoch_now=%u\n", started,
            advanced, watermark.GetEpoch());
        controlStarted = started && advanced;

        ::g_cjStoreGoodMask = published;
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    thread.join();
    GC_EXPECT_TRUE(precondition);
    GC_EXPECT_TRUE(targetHeld);
    GC_EXPECT_TRUE(controlStarted);
}
