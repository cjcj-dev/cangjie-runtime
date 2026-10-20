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

namespace {
alignas(4) uint32_t nativeSlot = 0;

const uint32_t* NativeIp()
{
    return &nativeSlot;
}

void ArmStaleEpoch(uintptr_t& published)
{
    published = ::g_cjStoreGoodMask;
    ::g_cjStoreGoodMask = published | ZPointerRememberedMask;
}

class BoundMutator {
public:
    explicit BoundMutator(Mutator& owner) : saved(ThreadLocal::GetMutator())
    {
        ThreadLocal::SetMutator(&owner);
    }
    ~BoundMutator() { ThreadLocal::SetMutator(saved); }
    BoundMutator(const BoundMutator&) = delete;
    BoundMutator& operator=(const BoundMutator&) = delete;
private:
    Mutator* saved;
};

__attribute__((noinline)) FrameAddress* PublishFrom(Mutator& owner)
{
    auto release = [] {};
    FrameAddress* seen = nullptr;
    {
        MutatorBlockInVMPreprocess<decltype(release)> blocked(owner, release);
        seen = owner.GetUnwindContext().frameInfo.mFrame.GetFA();
    }
    return seen;
}

__attribute__((noinline)) FrameAddress* PublishFromA(Mutator& owner) { return PublishFrom(owner); }
__attribute__((noinline)) FrameAddress* PublishFromB(Mutator& owner) { return PublishFrom(owner); }
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

// Physical chain: young native frame, then the anchor. The anchor's caller is a
// real frame, so the null-caller guard would install it. The oldest-frame stop
// must not. Native frames have no stackmap roots. Managed stackmap coverage is
// not this test.
GC_COMPONENT_TEST(WatermarkIteration, AnchorStopsBeforeNullCaller)
{
    FrameAddress past {};
    past.callerFrameAddress = nullptr;
    past.returnAddress = NativeIp();
    FrameAddress anchor {};
    anchor.callerFrameAddress = &past;
    anchor.returnAddress = NativeIp();
    FrameAddress young {};
    young.callerFrameAddress = &anchor;
    young.returnAddress = NativeIp();

    UnwindContext top;
    top.anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    top.frameInfo.mFrame.SetFA(&young);
    top.frameInfo.mFrame.SetIP(NativeIp());
    top.SetUnwindContextStatus(UnwindContextStatus::RELIABLE);

    StackFrameCursor cursor(top);
    int seen = 0;
    bool sawYoung = false;
    bool sawAnchor = false;
    bool sawPast = false;
    while (!cursor.Done()) {
        const FrameInfo* frame = cursor.CurrentFrame();
        GC_EXPECT_TRUE(frame != nullptr);
        ++seen;
        if (frame->mFrame.GetFA() == &young) { sawYoung = true; }
        if (frame->mFrame.GetFA() == &anchor) { sawAnchor = true; }
        if (frame->mFrame.GetFA() == &past) { sawPast = true; }
        GC_EXPECT_TRUE(frame->GetFrameType() == FrameType::NATIVE);
        cursor.Advance();
    }
    GC_EXPECT_TRUE(seen == 2);
    GC_EXPECT_TRUE(sawYoung);
    GC_EXPECT_TRUE(sawAnchor);
    GC_EXPECT_TRUE(!sawPast);
    GC_EXPECT_TRUE(!cursor.ChainBroken());

    FrameAddress broken {};
    broken.callerFrameAddress = nullptr;
    broken.returnAddress = NativeIp();
    UnwindContext bad;
    bad.anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    bad.frameInfo.mFrame.SetFA(&broken);
    bad.frameInfo.mFrame.SetIP(NativeIp());
    bad.SetUnwindContextStatus(UnwindContextStatus::RELIABLE);
    StackFrameCursor badCursor(bad);
    GC_EXPECT_TRUE(!badCursor.Done());
    GC_EXPECT_TRUE(badCursor.CurrentFrame()->mFrame.GetFA() == &broken);
    badCursor.Advance();
    GC_EXPECT_TRUE(badCursor.Done());
    GC_EXPECT_TRUE(badCursor.ChainBroken());
    GC_EXPECT_TRUE(badCursor.CurrentFrame() == nullptr);
}

// Same chain through the watermark consumer. Epoch is forced stale so
// on_safepoint cannot return before start_processing_impl.
GC_COMPONENT_TEST(WatermarkIteration, SafepointWalkStopsOnAnchor)
{
    FrameAddress past {};
    past.callerFrameAddress = nullptr;
    past.returnAddress = NativeIp();
    FrameAddress anchor {};
    anchor.callerFrameAddress = &past;
    anchor.returnAddress = NativeIp();
    FrameAddress young {};
    young.callerFrameAddress = &anchor;
    young.returnAddress = NativeIp();

    Mutator owner;
    BoundMutator bound(owner);
    owner.SetManagedContext(true);
    UnwindContext& context = owner.GetUnwindContext();
    context.anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    context.frameInfo.mFrame.SetFA(&young);
    context.frameInfo.mFrame.SetIP(NativeIp());
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

// A walk that starts and stops before the anchor must not return to the caller.
// An empty context is not that walk: javaFrameAnchor.hpp:77, stackWatermark.cpp:219.
GC_COMPONENT_TEST(WatermarkIteration, BrokenChainDoesNotCompleteEpoch)
{
    FrameAddress anchor {};
    anchor.callerFrameAddress = nullptr;
    anchor.returnAddress = NativeIp();
    FrameAddress broken {};
    broken.callerFrameAddress = nullptr;
    broken.returnAddress = NativeIp();

    Mutator owner;
    BoundMutator bound(owner);
    owner.SetManagedContext(true);
    UnwindContext& context = owner.GetUnwindContext();
    context.anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    context.frameInfo.mFrame.SetFA(&broken);
    context.frameInfo.mFrame.SetIP(NativeIp());
    context.SetUnwindContextStatus(UnwindContextStatus::RELIABLE);

    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        uintptr_t childPublished = 0;
        ArmStaleEpoch(childPublished);
        StackWatermarkSet::on_safepoint(owner);
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
        transcript.find("stack watermark stopped before the anchor") != std::string::npos;
    GC_EXPECT_TRUE(rejected);

    FrameAddress young {};
    young.callerFrameAddress = &anchor;
    young.returnAddress = NativeIp();
    Mutator legal;
    BoundMutator legalBound(legal);
    legal.SetManagedContext(true);
    UnwindContext& legalContext = legal.GetUnwindContext();
    legalContext.anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    legalContext.frameInfo.mFrame.SetFA(&young);
    legalContext.frameInfo.mFrame.SetIP(NativeIp());
    legalContext.SetUnwindContextStatus(UnwindContextStatus::RELIABLE);
    uintptr_t published = 0;
    ArmStaleEpoch(published);
    StackWatermarkSet::on_safepoint(legal);
    const bool legalDone = legal.GetStackWatermark().IsDone();
    ::g_cjStoreGoodMask = published;
    GC_EXPECT_TRUE(legalDone);

    Mutator cleared;
    BoundMutator clearedBound(cleared);
    cleared.SetManagedContext(true);
    cleared.GetUnwindContext().GoIntoManagedCode();
    cleared.GetUnwindContext().anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    const uint32_t packedBefore = cleared.GetStackWatermark().PackedState();
    ArmStaleEpoch(published);
    StackWatermarkSet::on_safepoint(cleared);
    const bool unpublishedStarted = cleared.GetStackWatermark().processing_started();
    const bool unpublishedDone = cleared.GetStackWatermark().IsDone(StackWatermark::epoch_id());
    const bool unpublishedUntouched = cleared.GetStackWatermark().PackedState() == packedBefore;
    ::g_cjStoreGoodMask = published;
    cleared.SetManagedContext(false);
    legal.SetManagedContext(false);
    owner.SetManagedContext(false);
    GC_EXPECT_TRUE(!unpublishedStarted);
    GC_EXPECT_TRUE(!unpublishedDone);
    GC_EXPECT_TRUE(unpublishedUntouched);
}

// MutatorBlockInVMPreprocess publishes the live caller for every managed
// non-RISKY entry, including one that already holds a frame, and restores the
// previous context after the walk. RISKY and native contexts are not replaced.
GC_COMPONENT_TEST(WatermarkIteration, BlockPublishesClearedManagedFrame)
{
    auto release = [] {};
    Mutator cleared;
    BoundMutator bound(cleared);
    cleared.SetManagedContext(true);
    cleared.SetInSaferegion(SAFE_REGION_FALSE);
    cleared.GetUnwindContext().GoIntoManagedCode();
    {
        MutatorBlockInVMPreprocess<decltype(release)> blocked(cleared, release);
        GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetFA() != nullptr);
        GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetIP() != nullptr);
    }
    GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetFA() == nullptr);
    GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetIP() == nullptr);

    FrameAddress sentinel {};
    sentinel.callerFrameAddress = &sentinel;
    sentinel.returnAddress = NativeIp();
    cleared.GetUnwindContext().frameInfo.mFrame.SetFA(&sentinel);
    cleared.GetUnwindContext().frameInfo.mFrame.SetIP(NativeIp());
    cleared.GetUnwindContext().SetUnwindContextStatus(UnwindContextStatus::RELIABLE);
    {
        MutatorBlockInVMPreprocess<decltype(release)> blocked(cleared, release);
        GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetFA() != &sentinel);
        GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetIP() != nullptr);
    }
    GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetFA() == &sentinel);
    GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetIP() == NativeIp());

    FrameAddress moved {};
    moved.callerFrameAddress = &sentinel;
    moved.returnAddress = NativeIp();
    cleared.GetUnwindContext().anchorFA = reinterpret_cast<uint32_t*>(&sentinel);
    {
        MutatorBlockInVMPreprocess<decltype(release)> blocked(cleared, release);
        cleared.GetUnwindContext().anchorFA = reinterpret_cast<uint32_t*>(&moved);
        GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetFA() != &sentinel);
    }
    GC_EXPECT_TRUE(cleared.GetUnwindContext().anchorFA == reinterpret_cast<uint32_t*>(&moved));
    GC_EXPECT_TRUE(cleared.GetUnwindContext().frameInfo.mFrame.GetFA() == &sentinel);

    Mutator nested;
    BoundMutator nestedBound(nested);
    nested.SetManagedContext(true);
    nested.SetInSaferegion(SAFE_REGION_TRUE);
    nested.GetUnwindContext().frameInfo.mFrame.SetFA(&sentinel);
    nested.GetUnwindContext().frameInfo.mFrame.SetIP(NativeIp());
    nested.GetUnwindContext().SetUnwindContextStatus(UnwindContextStatus::RELIABLE);
    {
        MutatorBlockInVMPreprocess<decltype(release)> blocked(nested, release);
        GC_EXPECT_TRUE(nested.InSaferegion());
        GC_EXPECT_TRUE(nested.GetUnwindContext().frameInfo.mFrame.GetFA() == &sentinel);
        GC_EXPECT_TRUE(nested.GetUnwindContext().frameInfo.mFrame.GetIP() == NativeIp());
    }
    GC_EXPECT_TRUE(nested.InSaferegion());
    GC_EXPECT_TRUE(nested.GetUnwindContext().frameInfo.mFrame.GetFA() == &sentinel);

    Mutator risky;
    BoundMutator riskyBound(risky);
    risky.SetManagedContext(true);
    risky.SetInSaferegion(SAFE_REGION_FALSE);
    risky.GetUnwindContext().GoIntoNativeCode(NativeIp(), &sentinel);
    {
        MutatorBlockInVMPreprocess<decltype(release)> blocked(risky, release);
        GC_EXPECT_TRUE(risky.GetUnwindContext().GetUnwindContextStatus() == UnwindContextStatus::RISKY);
        GC_EXPECT_TRUE(risky.GetUnwindContext().frameInfo.mFrame.GetFA() == &sentinel);
        GC_EXPECT_TRUE(risky.GetUnwindContext().frameInfo.mFrame.GetIP() == NativeIp());
    }
    GC_EXPECT_TRUE(risky.GetUnwindContext().frameInfo.mFrame.GetFA() == &sentinel);

    Mutator native;
    BoundMutator nativeBound(native);
    native.SetManagedContext(false);
    native.SetInSaferegion(SAFE_REGION_FALSE);
    native.GetUnwindContext().GoIntoManagedCode();
    {
        MutatorBlockInVMPreprocess<decltype(release)> blocked(native, release);
        GC_EXPECT_TRUE(native.GetUnwindContext().frameInfo.mFrame.GetFA() == nullptr);
        GC_EXPECT_TRUE(native.GetUnwindContext().frameInfo.mFrame.GetIP() == nullptr);
    }
}

// The capture from one call must not still be the context after that call
// returns, and the next block must publish its own caller.
GC_COMPONENT_TEST(WatermarkIteration, BlockDropsCaptureAfterReturn)
{
    Mutator owner;
    BoundMutator bound(owner);
    owner.SetManagedContext(true);
    owner.SetInSaferegion(SAFE_REGION_FALSE);
    owner.GetUnwindContext().GoIntoManagedCode();
    FrameAddress* first = PublishFromA(owner);
    GC_EXPECT_TRUE(first != nullptr);
    GC_EXPECT_TRUE(owner.GetUnwindContext().frameInfo.mFrame.GetFA() == nullptr);
    GC_EXPECT_TRUE(owner.GetUnwindContext().frameInfo.mFrame.GetIP() == nullptr);
    FrameAddress* second = PublishFromB(owner);
    GC_EXPECT_TRUE(second != nullptr);
    GC_EXPECT_TRUE(owner.GetUnwindContext().frameInfo.mFrame.GetFA() == nullptr);
    GC_EXPECT_TRUE(owner.GetUnwindContext().frameInfo.mFrame.GetIP() == nullptr);
}

// Exactly one of FA/IP is not a published frame and must not complete the epoch.
// Both null stays unpublished: the epoch is not started and this call does not abort.
GC_COMPONENT_TEST(WatermarkIteration, SingleEmptyTopIsRejected)
{
    FrameAddress anchor {};
    anchor.callerFrameAddress = nullptr;
    anchor.returnAddress = NativeIp();
    Mutator owner;
    BoundMutator bound(owner);
    owner.SetManagedContext(true);
    UnwindContext& context = owner.GetUnwindContext();
    context.anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    context.frameInfo.mFrame.SetFA(nullptr);
    context.frameInfo.mFrame.SetIP(NativeIp());
    context.SetUnwindContextStatus(UnwindContextStatus::RELIABLE);

    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        uintptr_t childPublished = 0;
        ArmStaleEpoch(childPublished);
        StackWatermarkSet::on_safepoint(owner);
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
        transcript.find("unwind top is missing only the frame address or only the instruction") != std::string::npos &&
        transcript.find("stack watermark stopped before the anchor") == std::string::npos;
    GC_EXPECT_TRUE(rejected);

    Mutator cleared;
    BoundMutator clearedBound(cleared);
    cleared.SetManagedContext(true);
    cleared.GetUnwindContext().GoIntoManagedCode();
    cleared.GetUnwindContext().anchorFA = reinterpret_cast<uint32_t*>(&anchor);
    uintptr_t published = 0;
    const uint32_t packedBefore = cleared.GetStackWatermark().PackedState();
    ArmStaleEpoch(published);
    StackWatermarkSet::on_safepoint(cleared);
    const bool stayed = !cleared.GetStackWatermark().processing_started() &&
        !cleared.GetStackWatermark().IsDone(StackWatermark::epoch_id()) &&
        cleared.GetStackWatermark().PackedState() == packedBefore;
    ::g_cjStoreGoodMask = published;
    cleared.SetManagedContext(false);
    owner.SetManagedContext(false);
    GC_EXPECT_TRUE(stayed);
}

// EnterSaferegion(false) must publish a managed unpublished top. It must not
// replace a RISKY native frame, a native context, or an already walkable top.
GC_COMPONENT_TEST(WatermarkIteration, EnterPublishesUnpublishedManagedTop)
{
    auto staysNull = [](Mutator& mutator) {
        return mutator.GetUnwindContext().frameInfo.mFrame.GetFA() == nullptr &&
            mutator.GetUnwindContext().frameInfo.mFrame.GetIP() == nullptr;
    };
    Mutator cleared;
    BoundMutator bound(cleared);
    cleared.SetManagedContext(true);
    cleared.SetInSaferegion(SAFE_REGION_FALSE);
    cleared.GetUnwindContext().GoIntoManagedCode();
    const bool entered = cleared.EnterSaferegion(false);
    const bool published = cleared.InSaferegion() &&
        cleared.GetUnwindContext().frameInfo.mFrame.GetFA() != nullptr &&
        cleared.GetUnwindContext().frameInfo.mFrame.GetIP() != nullptr;
    GC_EXPECT_TRUE(entered);
    GC_EXPECT_TRUE(published);

    FrameAddress sentinel {};
    sentinel.callerFrameAddress = &sentinel;
    sentinel.returnAddress = NativeIp();
    Mutator risky;
    BoundMutator riskyBound(risky);
    risky.SetManagedContext(true);
    risky.SetInSaferegion(SAFE_REGION_FALSE);
    risky.GetUnwindContext().GoIntoNativeCode(NativeIp(), &sentinel);
    const bool riskyEntered = risky.EnterSaferegion(false);
    GC_EXPECT_TRUE(riskyEntered);
    GC_EXPECT_TRUE(risky.GetUnwindContext().GetUnwindContextStatus() == UnwindContextStatus::RISKY);
    GC_EXPECT_TRUE(risky.GetUnwindContext().frameInfo.mFrame.GetFA() == &sentinel);
    GC_EXPECT_TRUE(risky.GetUnwindContext().frameInfo.mFrame.GetIP() == NativeIp());

    Mutator native;
    BoundMutator nativeBound(native);
    native.SetManagedContext(false);
    native.SetInSaferegion(SAFE_REGION_FALSE);
    native.GetUnwindContext().GoIntoManagedCode();
    const bool nativeEntered = native.EnterSaferegion(false);
    GC_EXPECT_TRUE(nativeEntered);
    GC_EXPECT_TRUE(staysNull(native));

    Mutator kept;
    BoundMutator keptBound(kept);
    kept.SetManagedContext(true);
    kept.SetInSaferegion(SAFE_REGION_FALSE);
    kept.GetUnwindContext().frameInfo.mFrame.SetFA(&sentinel);
    kept.GetUnwindContext().frameInfo.mFrame.SetIP(NativeIp());
    kept.GetUnwindContext().SetUnwindContextStatus(UnwindContextStatus::RELIABLE);
    const bool keptEntered = kept.EnterSaferegion(false);
    GC_EXPECT_TRUE(keptEntered);
    GC_EXPECT_TRUE(kept.GetUnwindContext().frameInfo.mFrame.GetFA() == &sentinel);
    GC_EXPECT_TRUE(kept.GetUnwindContext().frameInfo.mFrame.GetIP() == NativeIp());
}
