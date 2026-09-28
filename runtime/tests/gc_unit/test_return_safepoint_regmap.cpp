// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// A return-safepoint stub records save-area locations for its sender
// (frame_x86.inline.hpp:455-460) and does not license register GC roots
// on the ordinary call that follows (frame_x86.inline.hpp:464).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "CangjieRuntime.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "Mutator/Mutator.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/GcStackInfo.h"
#include "UnwindStack/StackFrameCursor.h"
#include "gc_unittest.hpp"

#if defined(__linux__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__linux__) && defined(__x86_64__)
#define MRT_TEST_RETURN_SAFEPOINT_REGMAP 1
#endif

#if defined(MRT_TEST_RETURN_SAFEPOINT_REGMAP)

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct Descriptor {
    int32_t descriptorOffset;
    uint32_t pc[4];
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
            data[pos / 8] |= ((value >> i) & 1) << (pos % 8);
        }
    }
    void Var(uint32_t value)
    {
        if (value <= 11) { Put(value, 4); }
        else { Put(12, 4); Put(value, 8); }
    }
};

void InitDescriptor(Descriptor& desc, uint64_t mask)
{
    std::memset(&desc, 0, sizeof(desc));
    desc.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&desc.stackMapOffset) -
        reinterpret_cast<char*>(&desc.descriptorOffset));
    desc.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(desc.bits) -
        reinterpret_cast<char*>(&desc.stackMapOffset));
    Bits bits {desc.bits};
    bits.Var(0); bits.Var(0); bits.Var(0);
    bits.Var(2); bits.Var(1); bits.Var(1); bits.Var(1); bits.Var(1); bits.Var(0);
    bits.Put(0, 32); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1);
    bits.Put(16, 32); bits.Put(1, 1); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1);
    bits.Var(1); bits.Var(33); bits.Put(mask, 33);
    bits.Var(0); bits.Var(0); bits.Var(0);
    bits.Var(0); bits.Var(0);
    bits.Var(0);
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(desc.pc));
}

constexpr unsigned savedGprs[] = {0, 3, 2, 1, 5, 4, 7, 8, 9, 10, 11, 12, 13, 14, 15};

uintptr_t* StubSlot(uintptr_t* fp, unsigned reg)
{
    for (unsigned i = 0; i < 15; ++i) {
        if (savedGprs[i] == reg) { return fp - 1 - i; }
    }
    return nullptr;
}

void PlantReturnFrame(GCStackInfo& stack, uintptr_t* fp, const uint32_t* start)
{
    MachineFrame stub;
    stub.SetFA(reinterpret_cast<FrameAddress*>(fp));
    stack.GetStack().emplace_back(stub, FrameType::RETURN_SAFEPOINT);
    FrameInfo managed(start);
    managed.SetFrameType(FrameType::MANAGED);
    managed.mFrame.SetFA(reinterpret_cast<FrameAddress*>(fp + 64));
    managed.mFrame.SetIP(start + 4);
    stack.GetStack().push_back(managed);
}

struct GrowOff {
    StackGrowConfig old = CangjieRuntime::stackGrowConfig;
    GrowOff() { CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_OFF; }
    ~GrowOff() { CangjieRuntime::stackGrowConfig = old; }
};

} // namespace

GC_TEST(ReturnSafepointRegMap, SenderKeepsStubSlot)
{
    GrowOff grow;
    static Descriptor desc;
    InitDescriptor(desc, 0);
    alignas(16) uintptr_t storage[128] {};
    uintptr_t* fp = &storage[56];
    const uintptr_t sentinel = 0x10000 + R12 * 16;
    *StubSlot(fp, R12) = sentinel;
    UnwindContext context{};
    GCStackInfo stack(&context);
    PlantReturnFrame(stack, fp, desc.pc);
    RootVisitor ignore = [](RootSlot&) {};
    Mutator mutator;
    stack.VisitStackRoots(ignore, mutator);
    const RegSlotsMap& map = stack.RegisterMap();
    const uintptr_t slot = reinterpret_cast<uintptr_t>(map.addrMap[R12]);
    const uintptr_t expected = reinterpret_cast<uintptr_t>(StubSlot(fp, R12));
    std::fprintf(stderr, "SENDER_SLOT_TARGET slot=%p expected=%p value=%p flag=%d\n",
        reinterpret_cast<void*>(slot), reinterpret_cast<void*>(expected),
        slot == 0 ? nullptr : reinterpret_cast<void*>(*reinterpret_cast<uintptr_t*>(slot)),
        map.allRegistersSaved ? 1 : 0);
    std::fprintf(stderr, "TARGET_ASSERT_EXECUTED SenderKeepsStubSlot\n");
    GC_EXPECT_EQ(slot, expected);
    GC_EXPECT_EQ(*reinterpret_cast<uintptr_t*>(slot), sentinel);
    GC_EXPECT_FALSE(map.allRegistersSaved);
}

GC_TEST(ReturnSafepointRegMap, OrdinaryCallRejectsRegisterRoot)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        GrowOff grow;
        static Descriptor desc;
        InitDescriptor(desc, uint64_t(1) << R12);
        alignas(16) uintptr_t storage[128] {};
        uintptr_t* fp = &storage[56];
        *StubSlot(fp, R12) = 0x10000 + R12 * 16;
        UnwindContext context{};
        GCStackInfo stack(&context);
        PlantReturnFrame(stack, fp, desc.pc);
        Mutator mutator;
        RootVisitor ignore = [](RootSlot&) {};
        stack.VisitStackRoots(ignore, mutator);
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char buffer[512];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) { transcript.append(buffer, count); }
    close(output[0]);
    std::fwrite(transcript.data(), 1, transcript.size(), stderr);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool rejected = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
        transcript.find("GC register root at ordinary statepoint") != std::string::npos;
    std::fprintf(stderr, "ORDINARY_CALL_TARGET rejected=%d status=%d\n", rejected ? 1 : 0, status);
    std::fprintf(stderr, "TARGET_ASSERT_EXECUTED OrdinaryCallRejectsRegisterRoot\n");
    GC_EXPECT_TRUE(rejected);
}

#endif
