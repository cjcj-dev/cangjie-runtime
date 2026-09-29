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

#include "Loader/ElfUnloadQuiescence.h"
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
GC_TEST(AbsentStackMap, NullOffsetWalkContinues)
{
    static Image absent;
    static Image present;
    Link(absent, false, 0);
    Link(present, true, 0);
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
    std::fprintf(stderr, "ABSENT_MAP_TARGET executed=1 first=%d second=%d finished=%d\n", first, second, finished);
    GC_EXPECT_TRUE(first);
    GC_EXPECT_TRUE(second);
    GC_EXPECT_TRUE(finished);
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
