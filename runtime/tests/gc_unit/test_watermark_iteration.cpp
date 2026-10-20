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
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "UnwindStack/PrintStackInfo.h"
#include "UnwindStack/StackFrameCursor.h"

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

extern uintptr_t unwindPCForSafepointHandlerStub;

namespace {
const uint32_t* SafepointStubIp()
{
    return reinterpret_cast<const uint32_t*>(&unwindPCForSafepointHandlerStub);
}

void ArmStaleEpoch(uintptr_t& published)
{
    published = ::g_cjStoreGoodMask;
    ::g_cjStoreGoodMask = published | ZPointerRememberedMask;
}
} // namespace

// Recorded frames return before any physical unwind. A null fa/ip on the first
// record must not swallow the rest of the stream.
GC_COMPONENT_TEST(WatermarkIteration, RecordedNullHeadStillAdvances)
{
    static uint32_t secondIp = 0;
    std::vector<FrameInfo> frames;
    frames.push_back(FrameInfo(MachineFrame(nullptr, nullptr), FrameType::NATIVE));
    frames.push_back(FrameInfo(MachineFrame(nullptr, &secondIp), FrameType::NATIVE));
    StackFrameCursor cursor(frames);
    GC_EXPECT_TRUE(!cursor.Done());
    GC_EXPECT_TRUE(cursor.CurrentFrame() != nullptr);
    GC_EXPECT_TRUE(cursor.CurrentFrame()->mFrame.GetFA() == nullptr);
    GC_EXPECT_TRUE(cursor.CurrentFrame()->mFrame.GetIP() == nullptr);
    cursor.Advance();
    GC_EXPECT_TRUE(!cursor.Done());
    GC_EXPECT_TRUE(cursor.CurrentFrame() != nullptr);
    GC_EXPECT_TRUE(cursor.CurrentFrame()->mFrame.GetIP() == &secondIp);
}

// Physical chain: young SAFEPOINT, then the anchor. The anchor's caller slot is
// null. The cursor must observe both frames and stop without loading that slot.
// SAFEPOINT has no stackmap roots (StackFrameCursor::ProcessFrame). Managed
// stackmap coverage is not this test.
GC_COMPONENT_TEST(WatermarkIteration, AnchorStopsBeforeNullCaller)
{
    FrameAddress anchor {};
    anchor.callerFrameAddress = nullptr;
    anchor.returnAddress = nullptr;
    FrameAddress young {};
    young.callerFrameAddress = &anchor;
    young.returnAddress = SafepointStubIp();

    UnwindContext top;
    top.anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    top.frameInfo.mFrame.SetFA(&young);
    top.frameInfo.mFrame.SetIP(SafepointStubIp());
    top.SetUnwindContextStatus(UnwindContextStatus::RELIABLE);

    StackFrameCursor cursor(top);
    int seen = 0;
    bool sawYoung = false;
    bool sawAnchor = false;
    while (!cursor.Done()) {
        const FrameInfo* frame = cursor.CurrentFrame();
        GC_EXPECT_TRUE(frame != nullptr);
        ++seen;
        if (frame->mFrame.GetFA() == &young) { sawYoung = true; }
        if (frame->mFrame.GetFA() == &anchor) { sawAnchor = true; }
        GC_EXPECT_TRUE(frame->GetFrameType() == FrameType::SAFEPOINT);
        cursor.Advance();
    }
    GC_EXPECT_TRUE(seen == 2);
    GC_EXPECT_TRUE(sawYoung);
    GC_EXPECT_TRUE(sawAnchor);

    FrameAddress broken {};
    int iteratorSlot = 0;
    broken.callerFrameAddress = nullptr;
    broken.returnAddress = reinterpret_cast<const uint32_t*>(&iteratorSlot);
    UnwindContext bad;
    bad.anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    bad.frameInfo.mFrame.SetFA(&broken);
    bad.frameInfo.mFrame.SetIP(SafepointStubIp());
    bad.SetUnwindContextStatus(UnwindContextStatus::RELIABLE);
    StackFrameCursor badCursor(bad);
    GC_EXPECT_TRUE(!badCursor.Done());
    GC_EXPECT_TRUE(badCursor.CurrentFrame()->mFrame.GetFA() == &broken);
    badCursor.Advance();
    GC_EXPECT_TRUE(badCursor.Done());
    GC_EXPECT_TRUE(badCursor.CurrentFrame() == nullptr);
}

// Same chain through the watermark consumer. Epoch is forced stale so
// on_safepoint cannot return before start_processing_impl.
GC_COMPONENT_TEST(WatermarkIteration, SafepointWalkStopsOnAnchor)
{
    FrameAddress anchor {};
    anchor.callerFrameAddress = nullptr;
    anchor.returnAddress = nullptr;
    FrameAddress young {};
    young.callerFrameAddress = &anchor;
    young.returnAddress = SafepointStubIp();

    Mutator owner;
    owner.SetManagedContext(true);
    UnwindContext& context = owner.GetUnwindContext();
    context.anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    context.frameInfo.mFrame.SetFA(&young);
    context.frameInfo.mFrame.SetIP(SafepointStubIp());
    context.SetUnwindContextStatus(UnwindContextStatus::RELIABLE);

    uintptr_t published = 0;
    ArmStaleEpoch(published);
    GC_EXPECT_TRUE(!owner.GetStackWatermark().processing_started());
    StackWatermarkSet::on_safepoint(owner);
    const bool started = owner.GetStackWatermark().processing_started();
    const bool finished = owner.GetStackWatermark().IsDone();
    ::g_cjStoreGoodMask = published;
    owner.SetManagedContext(false);
    GC_EXPECT_TRUE(started);
    GC_EXPECT_TRUE(finished);
}

// MutatorBlockInVMPreprocess publishes a live frame only for a cleared managed
// context. RISKY and native contexts keep the bytes they already had.
GC_COMPONENT_TEST(WatermarkIteration, BlockPublishesClearedManagedFrame)
{
    auto release = [] {};
    Mutator cleared;
    cleared.SetManagedContext(true);
    cleared.SetInSaferegion(SAFE_REGION_FALSE);
    cleared.GetUnwindContext().GoIntoManagedCode();
    {
        MutatorBlockInVMPreprocess<decltype(release)> blocked(cleared, release);
        GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetFA() != nullptr);
        GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetIP() != nullptr);
    }

    FrameAddress sentinel {};
    sentinel.callerFrameAddress = &sentinel;
    sentinel.returnAddress = SafepointStubIp();
    Mutator risky;
    risky.SetManagedContext(true);
    risky.SetInSaferegion(SAFE_REGION_FALSE);
    risky.GetUnwindContext().GoIntoNativeCode(SafepointStubIp(), &sentinel);
    {
        MutatorBlockInVMPreprocess<decltype(release)> blocked(risky, release);
        GC_EXPECT_TRUE(risky.GetUnwindContext().GetUnwindContextStatus() == UnwindContextStatus::RISKY);
        GC_EXPECT_TRUE(risky.GetUnwindContext().frameInfo.mFrame.GetFA() == &sentinel);
        GC_EXPECT_TRUE(risky.GetUnwindContext().frameInfo.mFrame.GetIP() == SafepointStubIp());
    }

    Mutator native;
    native.SetManagedContext(false);
    native.SetInSaferegion(SAFE_REGION_FALSE);
    native.GetUnwindContext().GoIntoManagedCode();
    {
        MutatorBlockInVMPreprocess<decltype(release)> blocked(native, release);
        GC_EXPECT_TRUE(native.GetUnwindContext().frameInfo.mFrame.GetFA() == nullptr);
        GC_EXPECT_TRUE(native.GetUnwindContext().frameInfo.mFrame.GetIP() == nullptr);
    }
}
