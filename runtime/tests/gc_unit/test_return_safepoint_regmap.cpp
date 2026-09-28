// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// Return-safepoint stubs are save-all frames. The walk keeps one register map
// and hands the stub slots to the sender (frame_x86.inline.hpp:455-460,
// stackWatermark.cpp:140). Clearing that map makes the sender's stack-pointer
// register unresolved.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Loader/ElfUnloadQuiescence.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/StackFrameCursor.h"
#include "UnwindStack/StackGrowStackInfo.h"
#include "gc_unittest.hpp"

#if defined(__linux__) && defined(__x86_64__)
#define MRT_TEST_RETURN_SAFEPOINT_REGMAP 1
#endif

#if defined(MRT_TEST_RETURN_SAFEPOINT_REGMAP)

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct ReturnPointFuncDesc {
    int32_t descriptorOffset;
    uint32_t pc[4];
    int32_t stackMapOffset;
    uint32_t rest[6];
    uint8_t bits[256];
};

ReturnPointFuncDesc gDesc;
bool gLinked = false;

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

void BuildBits(uint8_t* data)
{
    std::memset(data, 0, 256);
    Bits b {data};
    b.Var(0); b.Var(0); b.Var(0); b.Var(2); b.Var(4); b.Var(1); b.Var(1); b.Var(1); b.Var(0);
    b.Put(0, 32); b.Put(0, 4); b.Put(0, 1); b.Put(0, 1); b.Put(0, 1);
    b.Put(16, 32); b.Put(1, 4); b.Put(1, 1); b.Put(0, 1); b.Put(0, 1);
    b.Var(1); b.Var(16); b.Put(1, 16); b.Var(1); b.Var(8); b.Var(1);
    b.Put(0, 8); b.Put(1, 1); b.Var(0); b.Var(0); b.Var(0);
}

uintptr_t StartPC()
{
    if (!gLinked) {
        std::memset(&gDesc, 0, sizeof(gDesc));
        gDesc.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&gDesc.stackMapOffset) -
            reinterpret_cast<char*>(&gDesc.descriptorOffset));
        gDesc.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(gDesc.bits) -
            reinterpret_cast<char*>(&gDesc.stackMapOffset));
        BuildBits(gDesc.bits);
        ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(gDesc.pc));
        gLinked = true;
    }
    return reinterpret_cast<uintptr_t>(gDesc.pc);
}

alignas(16) uint64_t gArea[64];

FrameInfo MakeReturnFrame(uintptr_t sentinel, SlotAddress* r13Slot)
{
    const uintptr_t startPC = StartPC();
    FrameAddress* stub = reinterpret_cast<FrameAddress*>(&gArea[32]);
    stub->callerFrameAddress = nullptr;
    stub->returnAddress = nullptr;
    auto slot = [&](int index) { return reinterpret_cast<uint64_t*>(stub) - 1 - index; };
    *slot(9) = startPC;
    *slot(10) = startPC + 16;
    *slot(12) = sentinel;
    *r13Slot = reinterpret_cast<SlotAddress>(slot(12));
    MachineFrame machine;
    machine.SetFA(stub);
    machine.SetSP(reinterpret_cast<uintptr_t>(&gArea[0]));
    return FrameInfo(machine, FrameType::RETURN_SAFEPOINT);
}

} // namespace

GC_TEST(ReturnSafepointRegMap, SenderKeepsStubSlot)
{
    std::memset(gArea, 0, sizeof(gArea));
    const uintptr_t sentinel = 0x1000;
    SlotAddress expected = nullptr;
    const FrameInfo frame = MakeReturnFrame(sentinel, &expected);
    RegSlotsMap map;
    int visits = 0;
    RootVisitor visitor = [&](RootSlot&) { ++visits; };
    StackFrameCursor::ProcessReturnFrame(visitor, nullptr, map, frame);
    const bool recorded = map.HasReg(R13) && map.addrMap[R13] == expected;
    const uintptr_t value = recorded ? *reinterpret_cast<uintptr_t*>(map.addrMap[R13]) : 0;
    std::fprintf(stderr, "RETURN_SPILL_SLOT recorded=%d slot=%p expected=%p value=%p visits=%d\n",
        recorded ? 1 : 0, static_cast<void*>(map.addrMap[R13]), static_cast<void*>(expected),
        reinterpret_cast<void*>(value), visits);
    GC_EXPECT_TRUE(recorded);
    GC_EXPECT_EQ(value, sentinel);
}

GC_TEST(ReturnSafepointRegMap, StreamAndStackGrowShareStubSlot)
{
    std::memset(gArea, 0, sizeof(gArea));
    const uintptr_t sentinel = 0x2000;
    SlotAddress expected = nullptr;
    const FrameInfo frame = MakeReturnFrame(sentinel, &expected);
    StackFrameStream stream;
    stream.PublishCalleeRegisters(frame);
    StackGrowStackInfo grow;
    grow.PublishCalleeRegisters(frame);
    const SlotAddress streamSlot = stream.RegisterMap().addrMap[R13];
    const SlotAddress growSlot = grow.RegisterMap().addrMap[R13];
    std::fprintf(stderr, "RETURN_SPILL_SHARED stream=%p grow=%p expected=%p\n",
        static_cast<void*>(streamSlot), static_cast<void*>(growSlot), static_cast<void*>(expected));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(streamSlot), reinterpret_cast<uintptr_t>(expected));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(growSlot), reinterpret_cast<uintptr_t>(expected));
    GC_EXPECT_TRUE(stream.RegisterMap().allRegistersSaved);
    GC_EXPECT_TRUE(grow.RegisterMap().allRegistersSaved);
}

#endif
