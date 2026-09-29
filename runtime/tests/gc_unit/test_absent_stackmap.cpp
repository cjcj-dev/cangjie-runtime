// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// A compiled frame may publish funcdesc.stackMap offset 0. HotSpot frame.cpp:998
// skips root processing when oop_map() is null and keeps walking. The product
// entry is StackFrameStream::Start -> CheckRegisterRoots (StackInfo.cpp).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Loader/ElfUnloadQuiescence.h"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "UnwindStack/StackInfo.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct Image {
    int32_t descriptorOffset;
    uint32_t pc[8];
    int32_t stackMapOffset;
    uint32_t rest[6];
    uint8_t bits[256];
};

struct Bits {
    uint8_t* data;
    unsigned pos = 0;
    void Put(uint64_t value, unsigned width)
    {
        for (unsigned i = 0; i < width; ++i, ++pos) {
            data[pos / 8] |= static_cast<uint8_t>(((value >> i) & 1) << (pos % 8));
        }
    }
    void Var(uint32_t value)
    {
        if (value <= 11) { Put(value, 4); } else { Put(12, 4); Put(value, 8); }
    }
};

void Link(Image& image, bool withMap, uint64_t mask)
{
    std::memset(&image, 0, sizeof(image));
    image.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&image.stackMapOffset) -
        reinterpret_cast<char*>(&image.descriptorOffset));
    if (withMap) {
        image.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(image.bits) -
            reinterpret_cast<char*>(&image.stackMapOffset));
        Bits bits {image.bits};
        bits.Var(0); bits.Var(0); bits.Var(0);
        bits.Var(2); bits.Var(1); bits.Var(1); bits.Var(1); bits.Var(1); bits.Var(0);
        bits.Put(0, 32); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1);
        bits.Put(16, 32); bits.Put(1, 1); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1);
        bits.Var(1); bits.Var(33); bits.Put(mask, 33);
        bits.Var(0); bits.Var(0); bits.Var(0);
        bits.Var(0); bits.Var(0);
        bits.Var(0);
    }
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(image.pc));
}

FrameInfo Managed(Image& image)
{
    FrameInfo frame(image.pc);
    frame.SetFrameType(FrameType::MANAGED);
    frame.mFrame.SetIP(image.pc + 4);
    frame.mFrame.SetFA(reinterpret_cast<FrameAddress*>(image.pc));
    return frame;
}

} // namespace

// frame.cpp:998. Absent map must not be decoded; the walk continues to the next frame.
// The walk itself runs in a child so a decode of a null table is observed as the
// target invariant (the child must finish and report both frames) instead of
// killing the runner before any assertion runs.
GC_TEST(AbsentStackMap, NullOffsetWalkContinues)
{
#if defined(__linux__)
    static Image absent;
    static Image present;
    Link(absent, false, 0);
    Link(present, true, 0);
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        std::vector<FrameInfo> frames;
        frames.push_back(Managed(absent));
        frames.push_back(Managed(present));
        StackFrameStream stream(frames);
        stream.Start();
        const bool first = !stream.IsDone();
        stream.Next();
        const bool second = !stream.IsDone();
        stream.Next();
        const bool finished = stream.IsDone();
        std::fprintf(stderr, "ABSENT_MAP_WALK first=%d second=%d finished=%d\n", first, second, finished);
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char bytes[1024];
    ssize_t count;
    while ((count = read(output[0], bytes, sizeof(bytes))) > 0) { transcript.append(bytes, static_cast<size_t>(count)); }
    close(output[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool walked = transcript.find("ABSENT_MAP_WALK first=1 second=1 finished=1") != std::string::npos;
    std::fprintf(stderr, "ABSENT_MAP_TARGET executed=1 walked=%d exited=%d signaled=%d sig=%d status=%d\n%s",
        walked, WIFEXITED(status), WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0, status,
        transcript.c_str());
    GC_EXPECT_TRUE(walked);
    GC_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#else
    std::fprintf(stderr, "ABSENT_MAP_TARGET executed=0 reason=not-linux\n");
    GC_EXPECT_TRUE(true);
#endif
}

// Same entry, non-zero stack map: CheckRegisterRoots still reaches the register-root fatal.
GC_TEST(AbsentStackMap, PresentMapStillReachesRegisterRoot)
{
#if defined(__linux__)
    static Image present;
    Link(present, true, 1);
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        std::vector<FrameInfo> frames;
        frames.push_back(Managed(present));
        StackFrameStream stream(frames);
        stream.Start();
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char bytes[1024];
    ssize_t count;
    while ((count = read(output[0], bytes, sizeof(bytes))) > 0) { transcript.append(bytes, static_cast<size_t>(count)); }
    close(output[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    const bool message = transcript.find("GC register root") != std::string::npos;
    std::fprintf(stderr, "PRESENT_MAP_TARGET executed=1 aborted=%d message=%d status=%d\n%s", aborted, message, status,
        transcript.c_str());
    GC_EXPECT_TRUE(aborted);
    GC_EXPECT_TRUE(message);
#endif
}

#if defined(__linux__) && defined(__x86_64__)
// The two cases above drive StackFrameStream through its recordedFrames
// constructor, so they never exercise the real phase entrance. This one enters
// where the runtime enters: StackWatermarkSet::on_safepoint (zStackWatermark.cpp:452,
// registered in PHASE_ENTRIES.txt:199) over the mutator's real unwind context.
// Start() then runs the product's own AnalyseAndSetFrameType, which classifies a
// frame MANAGED only because its IP is a linked managed address
// (StackInfo.cpp:150-153), and CheckRegisterRoots reads that frame's funcdesc.
namespace {

// A real funcdesc: MFuncDesc::GetFuncDesc reads *(int32*)(pc - 4)
// (MFuncdesc.inline.h:60-70) and GetStackMap reads DataRefOffset32::refOffset
// (Dataref.h:24-33). refOffset 0 is exactly the "valid funcdesc, stackMap
// relative offset 0" the issue reports; nothing here is a decoder stub.
struct Desc {
    int32_t descriptorOffset;
    uint32_t pc[8];
    int32_t stackMapOffset;
    uint32_t rest[6];
    uint32_t returnPollFlag;
    uint8_t bits[256];
};

constexpr unsigned kAbsentFrames = 7;
constexpr unsigned kFrames = 14;
constexpr unsigned kFpSlot = 56;   // [fa - 1] holds start_pc + 9 (GetFuncStartPC)
constexpr unsigned kLinkSlot = 56; // [fa] holds the caller frame address
constexpr unsigned kRetSlot = 57;  // [fa + 1] holds the return address
constexpr unsigned kR13Slot = 53;  // fp - 24, the slot the present map names

void InitDesc(Desc& desc, bool withMap, bool barrier)
{
    std::memset(&desc, 0, sizeof(desc));
    desc.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&desc.stackMapOffset) -
        reinterpret_cast<char*>(&desc.descriptorOffset));
    if (withMap) {
        desc.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(desc.bits) -
            reinterpret_cast<char*>(&desc.stackMapOffset));
        Bits bits {desc.bits};
        bits.Var(0); bits.Var(0); bits.Var(0);
        bits.Var(2); bits.Var(1); bits.Var(1); bits.Var(1); bits.Var(1); bits.Var(0);
        bits.Put(0, 32); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1);
        bits.Put(16, 32); bits.Put(1, 1); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1);
        bits.Var(1); bits.Var(33); bits.Put(0, 33);
        bits.Var(0); bits.Var(0); bits.Var(0);
        bits.Var(0); bits.Var(0);
        bits.Var(0);
    }
    // A return-poll flag is the product's has_barrier property
    // (zStackWatermark.cpp:121-129); set it so the absent frames are the
    // barrier frames the issue's "volatile store then gc-leaf call" shape has.
    desc.returnPollFlag = barrier ? 1u : 0u;
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(desc.pc));
}

} // namespace

// Real safepoint entrance. The mutator's unwind context holds a real frame
// chain: frames 1..7 carry a funcdesc with stackMap relative offset 0, frames
// 8..13 carry a present map whose R13 prologue slot names a younger stack slot.
// The walk must cross the absent frames and still process frame 8's root.
GC_TEST(AbsentStackMap, SafepointWalkCrossesAbsentMapFrame)
{
    static Desc absent;
    static Desc present;
    InitDesc(absent, false, true);
    InitDesc(present, true, false);
    alignas(16) static uintptr_t frames[kFrames][64];

    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        // A younger frame the present map's root points at.
        uintptr_t target = reinterpret_cast<uintptr_t>(&frames[kAbsentFrames + 1][54]);
        for (unsigned i = 1; i < kFrames; ++i) {
            const Desc& desc = i <= kAbsentFrames ? absent : present;
            frames[i][kFpSlot - 1] = reinterpret_cast<uintptr_t>(desc.pc) + 9;
            frames[i][kLinkSlot] = i + 1 < kFrames ? reinterpret_cast<uintptr_t>(&frames[i + 1][kLinkSlot]) : 0;
            frames[i][kRetSlot] = reinterpret_cast<uintptr_t>(desc.pc) + 4;
        }
        // The first present frame publishes target through its R13 slot.
        frames[kAbsentFrames + 1][kR13Slot] = target;

        Mutator owner;
        owner.SetManagedContext(true);
        ThreadLocal::GetThreadLocalData()->SetMutator(&owner);
        uint32_t savedEpoch = *ZPointerStoreGoodMaskLowOrderBitsAddr;
        *ZPointerStoreGoodMaskLowOrderBitsAddr = savedEpoch == 7 ? 8 : 7;
        auto& context = owner.GetUnwindContext();
        context.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(&frames[1][kLinkSlot]));
        context.frameInfo.mFrame.SetIP(absent.pc);
        context.frameInfo.mFrame.SetSP(reinterpret_cast<uintptr_t>(frames));
        context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
        owner.SetStackTopAddr(reinterpret_cast<uintptr_t>(frames));
        owner.SetStackSize(sizeof(frames));

        StackWatermarkSet::on_safepoint(owner);
        const uintptr_t frontier = owner.GetStackWatermark().last_processed_raw();
        const bool bounded = !owner.GetStackWatermark().IsDone();
        *ZPointerStoreGoodMaskLowOrderBitsAddr = savedEpoch;
        std::fprintf(stderr, "ABSENT_SAFEPOINT_TARGET target=%p frontier=%p crossed=%d bounded=%d\n",
            reinterpret_cast<void*>(target), reinterpret_cast<void*>(frontier),
            static_cast<int>(frontier > target), static_cast<int>(bounded));
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char bytes[1024];
    ssize_t count;
    while ((count = read(output[0], bytes, sizeof(bytes))) > 0) { transcript.append(bytes, static_cast<size_t>(count)); }
    close(output[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool crossed = transcript.find("crossed=1") != std::string::npos;
    const bool finished = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::fprintf(stderr, "ABSENT_SAFEPOINT_RESULT executed=1 crossed=%d finished=%d signaled=%d sig=%d status=%d\n%s",
        crossed, finished, WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0, status,
        transcript.c_str());
    // The target invariant: the walk crossed the absent-map frames and reached
    // the present frame behind them. A decode of the null table takes the child
    // down before this line, which is what the cut arm shows.
    GC_EXPECT_TRUE(crossed);
    GC_EXPECT_TRUE(finished);
}
#endif
