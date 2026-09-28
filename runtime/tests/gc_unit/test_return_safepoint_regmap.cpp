// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// A return-safepoint stub records save-area locations for its sender
// (frame_x86.inline.hpp:455-460, stackWatermark.cpp:140) and does not
// license register GC roots on the ordinary call that follows
// (frame_x86.inline.hpp:464).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "CangjieRuntime.h"
#include "Heap/z/zStackWatermark.hpp"
#include "Loader/ElfUnloadQuiescence.h"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/GcStackInfo.h"
#include "UnwindStack/StackFrameCursor.h"
#include "UnwindStack/StackGrowStackInfo.h"
#include "gc_unittest.hpp"

#if defined(__linux__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" uint32_t unwindPCForReturnSafepointHandlerStub;

#if defined(__linux__) && defined(__x86_64__)
#define MRT_TEST_RETURN_SAFEPOINT_REGMAP 1
#endif

#if defined(MRT_TEST_RETURN_SAFEPOINT_REGMAP)

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct MapDesc {
    int32_t descriptorOffset;
    uint32_t pc[8];
    int32_t stackMapOffset;
    uint32_t rest[6];
    uint8_t bits[256];
};

struct Bits {
    uint8_t* data;
    uint32_t bit = 0;
    void Put(uint32_t value, unsigned width)
    {
        for (unsigned i = 0; i < width; ++i, ++bit) {
            data[bit / 8] |= static_cast<uint8_t>(((value >> i) & 1u) << (bit % 8));
        }
    }
    void Var(uint32_t value)
    {
        if (value <= 11) { Put(value, 4); }
        else { Put(12, 4); Put(value, 8); }
    }
};

MapDesc gStackPtr;
MapDesc gRegRoot;
bool gLinked = false;

void BuildStackPtrBits(uint8_t* data)
{
    std::memset(data, 0, 256);
    Bits b {data};
    b.Var(0); b.Var(0); b.Var(0);
    b.Var(1); b.Var(1); b.Var(1); b.Var(1); b.Var(1); b.Var(1); b.Var(1); b.Var(0);
    b.Put(4, 32); b.Put(0, 1); b.Put(0, 1); b.Put(0, 1); b.Put(0, 1); b.Put(1, 1); b.Put(0, 1);
    b.Var(1); b.Var(33); b.Put(static_cast<uint32_t>(1u << R13), 33);
    b.Var(0); b.Var(0); b.Var(0);
    b.Var(0); b.Var(0);
    b.Var(0);
}

void BuildRegRootBits(uint8_t* data)
{
    std::memset(data, 0, 256);
    Bits b {data};
    b.Var(0); b.Var(0); b.Var(0);
    b.Var(1); b.Var(1); b.Var(1); b.Var(1); b.Var(1); b.Var(0);
    b.Put(4, 32); b.Put(1, 1); b.Put(0, 1); b.Put(0, 1); b.Put(0, 1);
    b.Var(1); b.Var(33); b.Put(static_cast<uint32_t>(1u << R13), 33);
    b.Var(0); b.Var(0); b.Var(0);
    b.Var(0); b.Var(0);
    b.Var(0);
}

void LinkOne(MapDesc& desc, void (*build)(uint8_t*))
{
    std::memset(&desc, 0, sizeof(desc));
    desc.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&desc.stackMapOffset) -
        reinterpret_cast<char*>(&desc.descriptorOffset));
    desc.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(desc.bits) -
        reinterpret_cast<char*>(&desc.stackMapOffset));
    build(desc.bits);
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(desc.pc));
}

void EnsureLinked()
{
    if (gLinked) { return; }
    LinkOne(gStackPtr, BuildStackPtrBits);
    LinkOne(gRegRoot, BuildRegRootBits);
    gLinked = true;
}

struct Chain {
    uintptr_t pad;
    FrameAddress fa;
};

alignas(16) uint64_t gArea[96];

SlotAddress PlantStub(FrameAddress* stub, uintptr_t sentinel)
{
    RegSlotsMap layout;
    RegRoot::RecordStubAllRegister(layout, reinterpret_cast<Uptr>(stub));
    *reinterpret_cast<uintptr_t*>(layout.addrMap[R13]) = sentinel;
    *reinterpret_cast<uintptr_t*>(layout.addrMap[R10]) = reinterpret_cast<uintptr_t>(gStackPtr.pc);
    *reinterpret_cast<uintptr_t*>(layout.addrMap[R11]) = reinterpret_cast<uintptr_t>(gStackPtr.pc) + 4;
    return layout.addrMap[R13];
}

void LinkChain(FrameAddress* stub, Chain* nodes, unsigned count, const uint32_t* ip)
{
    stub->callerFrameAddress = &nodes[0].fa;
    stub->returnAddress = ip;
    for (unsigned i = 0; i < count; ++i) {
        nodes[i].fa.callerFrameAddress = (i + 1 < count) ? &nodes[i + 1].fa : nullptr;
        nodes[i].fa.returnAddress = (i + 1 < count) ? ip : nullptr;
    }
}

struct GrowRestore {
    StackGrowConfig old;
    GrowRestore() : old(CangjieRuntime::stackGrowConfig)
    {
        CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_ON;
    }
    ~GrowRestore() { CangjieRuntime::stackGrowConfig = old; }
};

} // namespace

GC_TEST(ReturnSafepointRegMap, SenderKeepsStubSlot)
{
    GrowRestore grow;
    EnsureLinked();
    std::memset(gArea, 0, sizeof(gArea));
    const uintptr_t sentinel = reinterpret_cast<uintptr_t>(&gArea[90]);
    FrameAddress* stub = reinterpret_cast<FrameAddress*>(&gArea[40]);
    Chain nodes[5];
    std::memset(nodes, 0, sizeof(nodes));
    const uint32_t* ip = gStackPtr.pc + 1;
    LinkChain(stub, nodes, 5, ip);
    const SlotAddress expected = PlantStub(stub, sentinel);
    *reinterpret_cast<uintptr_t*>(stub) = 0x1111;

    Mutator owner;
    owner.SetManagedContext(true);
    owner.SetStackTopAddr(reinterpret_cast<uintptr_t>(&gArea[0]) - 1);
    owner.SetStackSize(sizeof(gArea) + 32);
    UnwindContext& context = owner.GetUnwindContext();
    context.frameInfo.mFrame.SetFA(stub);
    context.frameInfo.mFrame.SetIP(&unwindPCForReturnSafepointHandlerStub);
    context.frameInfo.mFrame.SetSP(reinterpret_cast<uintptr_t>(stub));
    context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
    Mutator* saved = ThreadLocal::GetMutator();
    ThreadLocal::SetMutator(&owner);

    StackFrameCursor cursor(context);
    RootVisitor ignore = [](RootSlot&) {};
    cursor.ProcessOne(ignore, owner);
    const RegSlotsMap& map = cursor.RegMap();
    StackPtrMap pointers = StackMapBuilder(reinterpret_cast<uintptr_t>(gStackPtr.pc),
        reinterpret_cast<uintptr_t>(ip), reinterpret_cast<uintptr_t>(&nodes[0].fa)).Build<StackPtrMap>();
    uintptr_t seenAddr = 0;
    uintptr_t seenValue = 0;
    int visits = 0;
    StackPtrVisitor visit = [&](ObjectRef& slot) {
        seenAddr = reinterpret_cast<uintptr_t>(&slot);
        seenValue = *reinterpret_cast<uintptr_t*>(&slot);
        ++visits;
    };
    const bool resolved = pointers.IsValid() && pointers.VisitStackPointerRegs(visit, nullptr, const_cast<RegSlotsMap&>(map));
    std::fprintf(stderr,
        "SENDER_SLOT_TARGET resolved=%d visits=%d slot=%p expected=%p value=%p sentinel=%p flag=%d\n",
        resolved ? 1 : 0, visits, reinterpret_cast<void*>(seenAddr), static_cast<void*>(expected),
        reinterpret_cast<void*>(seenValue), reinterpret_cast<void*>(sentinel), map.allRegistersSaved ? 1 : 0);
    std::fprintf(stderr, "TARGET_ASSERT_EXECUTED SenderKeepsStubSlot\n");

    GCStackInfo stack(&context);
    stack.GetStack().emplace_back(context.frameInfo.mFrame, FrameType::RETURN_SAFEPOINT);
    FrameInfo managed(ip);
    managed.SetFrameType(FrameType::MANAGED);
    managed.mFrame.SetFA(&nodes[0].fa);
    managed.mFrame.SetIP(ip);
    stack.GetStack().push_back(managed);
    stack.VisitStackRoots(ignore, owner);
    StackGrowStackInfo moving(&context);
    moving.GetStack() = stack.GetStack();
    moving.RecordStackPtrs(ignore, [](ObjectRef&) {}, [](BasePtrType, DerivedSlot&) {}, owner);
    const SlotAddress gcSlot = stack.RegisterMap().addrMap[R13];
    const SlotAddress growSlot = moving.RegisterMap().addrMap[R13];

    uint32_t* epoch = ZPointerStoreGoodMaskLowOrderBitsAddr;
    const uint32_t savedEpoch = *epoch;
    *epoch = savedEpoch == 7 ? 8 : 7;
    StackWatermarkSet::start_processing(owner);
    const bool done = owner.GetStackWatermark().IsDone();
    *epoch = savedEpoch;
    ThreadLocal::SetMutator(saved);

    std::fprintf(stderr, "SENDER_SLOT_SHARED gc=%p grow=%p done=%d\n",
        static_cast<void*>(gcSlot), static_cast<void*>(growSlot), done ? 1 : 0);
    GC_EXPECT_TRUE(resolved);
    GC_EXPECT_EQ(visits, 1);
    GC_EXPECT_EQ(seenAddr, reinterpret_cast<uintptr_t>(expected));
    GC_EXPECT_EQ(seenValue, sentinel);
    GC_EXPECT_FALSE(map.allRegistersSaved);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(gcSlot), reinterpret_cast<uintptr_t>(expected));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(growSlot), reinterpret_cast<uintptr_t>(expected));
    GC_EXPECT_TRUE(done);
}

GC_TEST(ReturnSafepointRegMap, OrdinaryCallRejectsRegisterRoot)
{
    EnsureLinked();
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_OFF;
        EnsureLinked();
        UnwindContext context{};
        GCStackInfo stack(&context);
        MachineFrame stubFrame;
        stubFrame.SetFA(reinterpret_cast<FrameAddress*>(&gArea[40]));
        stubFrame.SetIP(&unwindPCForReturnSafepointHandlerStub);
        stack.GetStack().emplace_back(stubFrame, FrameType::RETURN_SAFEPOINT);
        FrameInfo managed(gRegRoot.pc + 1);
        managed.SetFrameType(FrameType::MANAGED);
        managed.mFrame.SetFA(reinterpret_cast<FrameAddress*>(&gArea[70]));
        managed.mFrame.SetIP(gRegRoot.pc + 1);
        stack.GetStack().push_back(managed);
        Mutator mutator;
        RootVisitor visitor = [](RootSlot&) {};
        stack.VisitStackRoots(visitor, mutator);
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
