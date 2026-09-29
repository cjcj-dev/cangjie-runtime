// Copyright (c) Huawei Technologies Co. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// ZGC stackWatermark.inline.hpp:70-71,127-131: on_iteration assumes processing
// has already been started by on_safepoint (stackWatermark.cpp:311-318) or by
// the STW safepoint / handshake paths (stackWatermarkSet.cpp:121-130,163-170).
// A diagnostic stack walk is not one of those entries; exposing a frame
// without started processing must assert (stackWatermark.inline.hpp:71). The product decision point is
// StackInfo::ProcessOnIteration (runtime/src/UnwindStack/StackInfo.cpp), which
// used to call StackWatermarkSet::start_processing unconditionally.
#include <cstdio>
#include <csignal>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

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
    using PrintStackInfo::PrintStackInfo;
    void WalkOneFrame(Mutator& owner, const FrameInfo& frame)
    {
        SetProcessingOwner(&owner);
        ProcessOnIteration(frame);
    }
};
} // namespace

GC_COMPONENT_TEST(WatermarkIteration, DiagnosticWalkRejectsUnstartedEpoch)
{
    // This death test owns its environment. Fork before creating any worker
    // threads; inheriting a live runtime's mutexes cannot test an epoch guard.
    Mutator ownerStorage;
    ownerStorage.SetManagedContext(false);
    Mutator* owner = &ownerStorage;
    auto& watermark = owner->GetStackWatermark();
    bool precondition = false;
    bool targetHeld = false;
    bool controlStarted = false;
    // A real phase flip republishes the store-good colour before
    // safepoint_synchronize_begin (stackWatermarkSet.cpp:163-170) starts
    // processing, which leaves the owner registered on the previous epoch.
    const uintptr_t published = ::g_cjStoreGoodMask;
    ::g_cjStoreGoodMask = published | ZPointerRememberedMask;
    const bool stale = !watermark.processing_started();
    const uint32_t stateBefore = watermark.PackedState();

    FrameInfo frame { MachineFrame(nullptr, nullptr), FrameType::MANAGED };
    IterationDriver driver(&owner->GetUnwindContext());
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        driver.WalkOneFrame(*owner, frame);
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char bytes[512];
    ssize_t count;
    while ((count = read(output[0], bytes, sizeof(bytes))) > 0) { transcript.append(bytes, count); }
    close(output[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool rejected = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
        transcript.find("Processing should already have started") != std::string::npos;
    std::fprintf(stderr, "WM_ITERATION_TARGET executed=1 stale=%d rejected=%d status=%d\n%s",
                 stale, rejected, status, transcript.c_str());
    precondition = stale;
    targetHeld = rejected;

    // Positive control: the legal safepoint entry does start processing on
    // the same owner, and the epoch it publishes is the current one, so the
    // state read by the target assertion is not a constant.
    StackWatermarkSet::on_safepoint(*owner);
    const bool started = watermark.processing_started();
    const bool advanced = watermark.GetEpoch() != StackWatermark::UnpackEpoch(stateBefore);
    std::fprintf(stderr, "WM_ITERATION_CONTROL executed=1 started=%d epoch_advanced=%d epoch_now=%llu\n", started,
        advanced, static_cast<unsigned long long>(watermark.GetEpoch()));
    controlStarted = started && advanced;

    ::g_cjStoreGoodMask = published;
    GC_EXPECT_TRUE(precondition);
    GC_EXPECT_TRUE(targetHeld);
    GC_EXPECT_TRUE(controlStarted);
}
