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
#include "Mutator/ThreadLocal.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/GcStackInfo.h"
#include "UnwindStack/StackFrameCursor.h"
#include "UnwindStack/StackGrowStackInfo.h"
#include "Heap/z/zStackWatermark.hpp"
#include "Heap/z/zAddress.inline.hpp"

extern "C" uint32_t unwindPCForReturnSafepointHandlerStub;
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

namespace {
// Metadata and frame bytes are inputs; all classification, unwinding, location
// propagation and pointer consumption happen in the linked product SO.
struct PointerChain {
    Descriptor desc {};
    alignas(16) uintptr_t frames[14][64] {};
    StackGrowConfig savedGrow = CangjieRuntime::stackGrowConfig;
    uint32_t savedEpoch = *ZPointerStoreGoodMaskLowOrderBitsAddr;
    Mutator owner;
    Mutator* savedMutator = ThreadLocal::GetThreadLocalData()->mutator;
    uintptr_t* expectedSlot;
    uintptr_t target;

    explicit PointerChain(bool returning)
    {
        ThreadLocal::GetThreadLocalData()->SetMutator(&owner);
        CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_ON;
        desc.descriptorOffset = reinterpret_cast<char*>(&desc.stackMapOffset) -
            reinterpret_cast<char*>(&desc.descriptorOffset);
        desc.stackMapOffset = reinterpret_cast<char*>(desc.bits) - reinterpret_cast<char*>(&desc.stackMapOffset);
        Bits bits {desc.bits};
        // R13's prologue slot is fp-24. PC0 has no incoming stack pointer;
        // PC16 names R13. GC roots are only the zero-valued fp-16 slots.
        bits.Var(0); bits.Var(0); bits.Var(4); bits.Var(3);
        bits.Var(2); bits.Var(0); bits.Var(1); bits.Var(0); bits.Var(0); bits.Var(1); bits.Var(0); bits.Var(0);
        bits.Put(0, 32); bits.Put(1, 1); bits.Put(0, 1);
        bits.Put(16, 32); bits.Put(1, 1); bits.Put(1, 1);
        bits.Var(1); bits.Var(16); bits.Put(1u << R13, 16);
        bits.Var(1); bits.Var(8); bits.Var(1); bits.Put(0xf0, 8); bits.Put(1, 1);
        bits.Var(0); bits.Var(0); bits.Var(0);
        ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(desc.pc));
        for (unsigned i = 1; i < 14; ++i) {
            frames[i][55] = reinterpret_cast<uintptr_t>(desc.pc) + 9;
            frames[i][56] = i + 1 < 14 ? reinterpret_cast<uintptr_t>(&frames[i + 1][56]) : 0;
            frames[i][57] = reinterpret_cast<uintptr_t>(desc.pc + 4);
        }
        target = reinterpret_cast<uintptr_t>(&frames[8][54]);
        auto& context = owner.GetUnwindContext();
        if (returning) {
            auto* stub = &frames[0][56];
            stub[0] = reinterpret_cast<uintptr_t>(&frames[1][56]);
            stub[1] = reinterpret_cast<uintptr_t>(desc.pc + 4);
            *StubSlot(stub, R10) = reinterpret_cast<uintptr_t>(desc.pc);
            *StubSlot(stub, R11) = reinterpret_cast<uintptr_t>(desc.pc);
            expectedSlot = StubSlot(stub, R13);
            // A consumer that overwrites the incoming map with this prologue
            // first will resolve a different slot and miss the distant frame.
            frames[1][53] = reinterpret_cast<uintptr_t>(&frames[3][54]);
            context.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(stub));
            context.frameInfo.mFrame.SetIP(&unwindPCForReturnSafepointHandlerStub);
        } else {
            expectedSlot = &frames[1][53];
            context.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(&frames[1][56]));
            context.frameInfo.mFrame.SetIP(desc.pc);
        }
        *expectedSlot = target;
        context.frameInfo.mFrame.SetSP(reinterpret_cast<uintptr_t>(frames));
        context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
        owner.SetManagedContext(true);
        owner.SetStackTopAddr(reinterpret_cast<uintptr_t>(frames));
        owner.SetStackSize(sizeof(frames));
        *ZPointerStoreGoodMaskLowOrderBitsAddr = savedEpoch == 7 ? 8 : 7;
    }
    ~PointerChain()
    {
        ThreadLocal::GetThreadLocalData()->SetMutator(savedMutator);
        CangjieRuntime::stackGrowConfig = savedGrow;
        *ZPointerStoreGoodMaskLowOrderBitsAddr = savedEpoch;
    }

    void CheckWatermark()
    {
        // Real phase entrance -> start_processing -> Next -> process_frame ->
        // VisitStackPointerRegs. Observe the frontier computed from its value.
        StackWatermarkSet::on_safepoint(owner);
        const uintptr_t frontier = owner.GetStackWatermark().last_processed_raw();
        const bool bounded = !owner.GetStackWatermark().IsDone();
        std::fprintf(stderr, "WATERMARK_POINTER_TARGET frontier=%p target=%p bounded=%d\n",
            reinterpret_cast<void*>(frontier), reinterpret_cast<void*>(target), bounded);
        GC_EXPECT_TRUE(frontier > target);
        GC_EXPECT_TRUE(bounded);
    }
    void CheckGrow()
    {
        StackGrowStackInfo grow(&owner.GetUnwindContext());
        grow.FillInStackTrace();
        uintptr_t observedSlot = 0;
        uintptr_t observedValue = 0;
        StackPtrVisitor pointer = [&](ObjectRef& slot) {
            // Ignore frame links: the pointer value uniquely identifies R13.
            if (raw(slot.LoadPlain()) == target) {
                observedSlot = reinterpret_cast<uintptr_t>(&slot);
                observedValue = raw(slot.LoadPlain());
            }
        };
        grow.RecordStackPtrs([](ObjectRef&) {}, pointer, [](BasePtrType, DerivedSlot&) {}, owner);
        std::fprintf(stderr, "GROW_POINTER_TARGET slot=%p expected=%p value=%p target=%p\n",
            reinterpret_cast<void*>(observedSlot), expectedSlot,
            reinterpret_cast<void*>(observedValue), reinterpret_cast<void*>(target));
        GC_EXPECT_EQ(observedSlot, reinterpret_cast<uintptr_t>(expectedSlot));
        GC_EXPECT_EQ(observedValue, target);
    }
};
}

GC_TEST(ReturnSafepointRegMap, SenderKeepsStubSlot)
{
    PointerChain chain(true);
    chain.CheckWatermark();
    chain.CheckGrow();
}

GC_TEST(ReturnSafepointRegMap, PrologueReachesWatermark)
{
    PointerChain chain(false);
    chain.CheckWatermark();
}

GC_TEST(ReturnSafepointRegMap, PrologueReachesStackGrow)
{
    PointerChain chain(false);
    chain.CheckGrow();
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
