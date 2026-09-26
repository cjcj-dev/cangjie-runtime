// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Phase 8: ZGenerationOld::remap_young_roots (zGeneration.cpp:1503-1508).
// Remap space is four one-hots; a flip is xor, so a colour published at N is
// load-good again at N+2 unless roots are remapped between young flips.

#include "Heap/z/zStackWatermark.hpp"
#include "gc_unittest.hpp"
#include "Mutator/ThreadLocal.h"
#include "Mutator/Mutator.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Loader/ElfUnloadQuiescence.h"
#include "CangjieRuntime.h"

#include <cstdint>
#include <cstring>

extern "C" void HandleReturnSafepoint(MapleRuntime::ThreadLocalData* tlData);
extern "C" uint32_t unwindPCForReturnSafepointHandlerStub;

namespace {
using namespace MapleRuntime;

static void ManagedFrameIp() {}

struct EmptyFuncDesc {
    int32_t descriptorOffset;
    uint32_t pc;
    int32_t stackMapOffset;
    uint32_t rest[6];
    uint8_t bits[32];
};

struct ReturnFuncDesc {
    int32_t descriptorOffset;
    uint32_t pc[4];
    int32_t stackMapOffset;
    uint32_t rest[6];
    uint8_t bits[256];
};

struct ChainNode {
    uintptr_t before;
    FrameAddress fa;
};

EmptyFuncDesc gEmptyDesc;
ReturnFuncDesc gReturnDesc;
bool gImagesReady = false;

void EnsureImages()
{
    if (gImagesReady) {
        return;
    }
    std::memset(&gEmptyDesc, 0, sizeof(gEmptyDesc));
    gEmptyDesc.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&gEmptyDesc.stackMapOffset) -
        reinterpret_cast<char*>(&gEmptyDesc.descriptorOffset));
    gEmptyDesc.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(gEmptyDesc.bits) -
        reinterpret_cast<char*>(&gEmptyDesc.stackMapOffset));
    std::memset(&gReturnDesc, 0, sizeof(gReturnDesc));
    gReturnDesc.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&gReturnDesc.stackMapOffset) -
        reinterpret_cast<char*>(&gReturnDesc.descriptorOffset));
    gReturnDesc.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(gReturnDesc.bits) -
        reinterpret_cast<char*>(&gReturnDesc.stackMapOffset));
    size_t bit = 0;
    auto put = [&](uint32_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i, ++bit) {
            gReturnDesc.bits[bit / 8] |= static_cast<uint8_t>(((value >> i) & 1u) << (bit % 8));
        }
    };
    auto var = [&](uint32_t value) {
        if (value <= 11) {
            put(value, 4);
        } else {
            put(12, 4);
            put(value, 8);
        }
    };
    var(0);
    var(0);
    var(0);
    var(2);
    var(4);
    var(1);
    var(1);
    var(1);
    if (CangjieRuntime::stackGrowConfig == StackGrowConfig::STACK_GROW_ON) {
        var(0);
        var(0);
    }
    var(0);
    put(0, 32);
    put(0, 4);
    put(0, 1);
    put(0, 1);
    put(0, 1);
    put(16, 32);
    put(1, 4);
    put(0, 1);
    put(0, 1);
    put(0, 1);
    var(1);
    var(16);
    put(1u, 16);
    var(0);
    var(8);
    var(0);
    var(0);
    var(0);
    var(0);
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(gReturnDesc.pc));
    gImagesReady = true;
}

uintptr_t EmptyStartPC()
{
    EnsureImages();
    return reinterpret_cast<uintptr_t>(&gEmptyDesc.pc);
}

void LinkManaged(ChainNode& node, FrameAddress* caller, const uint32_t* ip)
{
    node.before = EmptyStartPC() + 9;
    node.fa.callerFrameAddress = caller;
    node.fa.returnAddress = ip;
}

#if defined(__x86_64__)
uint64_t* StubSlot(FrameAddress* fa, int index)
{
    return reinterpret_cast<uint64_t*>(fa) - 1 - index;
}
constexpr int kReturnSlot = 0;
constexpr int kStartSlot = 9;
constexpr int kSiteSlot = 10;
#elif defined(__aarch64__)
uint64_t* StubSlot(FrameAddress* fa, int index)
{
    return reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(fa) + 16 + static_cast<uintptr_t>(index) * 8);
}
constexpr int kReturnSlot = 0;
constexpr int kSiteSlot = 16;
constexpr int kStartSlot = 17;
#endif

class RewriteReturnRoot : public HandshakeClosure {
public:
    RewriteReturnRoot(BaseObject* from, BaseObject* to)
        : HandshakeClosure("return-root-scope"), from(from), to(to) {}
    void do_thread(ThreadLocalData* tls) override
    {
        if (tls == nullptr || tls->mutator == nullptr) {
            return;
        }
        const bool managed = tls->mutator->IsManagedContext();
        tls->mutator->SetManagedContext(false);
        tls->mutator->VisitMutatorRoots([&](RootSlot& slot) {
            if (to_object(safe(slot.LoadPlain())) == from) {
                StorePlain(slot, from_object(to));
                rewritten = true;
            }
        });
        tls->mutator->SetManagedContext(managed);
    }
    BaseObject* from;
    BaseObject* to;
    bool rewritten = false;
};

} // namespace

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// No-frame threads still pass through the product head-processing entry.
GC_TEST(StackWatermark, PackedEpochDoneIsIdempotent)
{
    Mutator owner;
    auto& watermark = owner.GetStackWatermark();
    uint32_t* epoch = ZPointerStoreGoodMaskLowOrderBitsAddr;
    const uint32_t saved = *epoch;
    struct Restore { uint32_t* p; uint32_t value; ~Restore() { *p = value; } } restore { epoch, saved };
    *epoch = 7;
    StackWatermarkSet::start_processing(owner);
    GC_EXPECT_TRUE(watermark.IsDone(7));
    const uint32_t first = watermark.PackedState();
    StackWatermarkSet::start_processing(owner);
    GC_EXPECT_EQ(watermark.PackedState(), first);
    *epoch = 8;
    StackWatermarkSet::start_processing(owner);
    GC_EXPECT_TRUE(watermark.IsDone(8));
    GC_EXPECT_FALSE(watermark.IsDone(7));
    GC_EXPECT_EQ(watermark.prev_head_color(), uintptr_t(7));
    MachineFrame machine;
    machine.SetSP(0x1000);
    const FrameInfo frame(machine, FrameType::MANAGED);
    GC_EXPECT_EQ(watermark.prev_frame_color(frame), uintptr_t(7));
    std::fprintf(stderr, "PREV_FRAME_COLOR color=%zx\n", watermark.prev_frame_color(frame));
}

// zStackWatermark.cpp:64-76: completed intervals cover every older frontier.
GC_TEST(StackWatermark, HistoricalColorCoverage)
{
    const ZColorWatermark complete { 1, 0 };
    const ZColorWatermark lower { 2, 0x1000 };
    const ZColorWatermark higher { 3, 0x2000 };
    GC_EXPECT_TRUE(complete.covers(lower));
    GC_EXPECT_TRUE(complete.covers(complete));
    GC_EXPECT_FALSE(lower.covers(complete));
    GC_EXPECT_FALSE(lower.covers(higher));
    GC_EXPECT_TRUE(higher.covers(lower));
}

// HotSpot safepointMechanism.cpp:81-94: a disarmed return poll must be
// above every stack address, while ordinary polling only observes bit zero.
GC_TEST(StackWatermark, SharedPollWordArmAndDisarm)
{
    ThreadLocalData* tls = ThreadLocal::GetThreadLocalData();
    const uintptr_t saved = tls->GetPollWord();
    struct Restore {
        ThreadLocalData* tls;
        uintptr_t value;
        ~Restore() { tls->SetPollWord(value); }
    } restore { tls, saved };
    class PollOperation : public HandshakeClosure {
    public:
        PollOperation() : HandshakeClosure("return-poll-ABI") {}
        void do_thread(ThreadLocalData*) override {}
    } closure;
    HandshakeOperation operation(&closure, tls);
    HandshakeState& state = Handshake::Current();
    state.add_operation(&operation);
    const uintptr_t armed = tls->GetPollWord();
    state.process_by_self();
    const uintptr_t disarmed = tls->GetPollWord();
    GC_EXPECT_EQ(armed, ThreadLocalData::PollBit);
    GC_EXPECT_EQ(disarmed, ThreadLocalData::DisarmedPollWord);
    GC_EXPECT_TRUE(reinterpret_cast<uintptr_t>(&saved) > armed);
    GC_EXPECT_FALSE(reinterpret_cast<uintptr_t>(&saved) > disarmed);
    GC_EXPECT_FALSE(tls->IsPollArmed());
    std::fprintf(stderr, "POLL_WORD_RESULT armed=%zx disarmed=%zx\n", armed, disarmed);
}

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
// stackWatermark.cpp:184-196 and zStackWatermark.cpp:83-87: a stack move keeps the
// unfinished frontier and the historical colour that names it.
GC_TEST(StackWatermark, RemapRetainsLogicalStackIdentityAcrossGrow)
{
    uint32_t* epoch = ZPointerStoreGoodMaskLowOrderBitsAddr;
    const uint32_t saved = *epoch;
    struct Restore {
        uint32_t* p;
        uint32_t value;
        ~Restore() { *p = value; }
    } restore { epoch, saved };
    Mutator owner;
    const uint32_t first = saved == 7 ? 8 : 7;
    *epoch = first;
    ChainNode nodes[6];
    std::memset(nodes, 0, sizeof(nodes));
    for (int i = 0; i < 5; ++i) {
        LinkManaged(nodes[i], &nodes[i + 1].fa, reinterpret_cast<const uint32_t*>(&ManagedFrameIp));
    }
    nodes[5].fa.callerFrameAddress = nullptr;
    nodes[5].fa.returnAddress = nullptr;
    UnwindContext& context = owner.GetUnwindContext();
    context.frameInfo.mFrame.SetFA(&nodes[0].fa);
    context.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(&ManagedFrameIp));
    context.frameInfo.mFrame.SetSP(0x8000);
    context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
    StackWatermarkSet::start_processing(owner);
    auto& watermark = owner.GetStackWatermark();
    GC_EXPECT_FALSE(watermark.IsDone());
    const uintptr_t mark = watermark.watermark();
    const uintptr_t last = watermark.last_processed_raw();
    GC_EXPECT_NE(mark, uintptr_t(0));
    GC_EXPECT_NE(last, uintptr_t(0));
    watermark.OnStackGrow(4096);
    GC_EXPECT_FALSE(watermark.IsDone());
    GC_EXPECT_EQ(watermark.watermark(), mark + 4096);
    GC_EXPECT_EQ(watermark.last_processed_raw(), last + 4096);
    *epoch = first + 1;
    StackWatermarkSet::start_processing(owner);
    watermark.OnStackGrow(4096);
    MachineFrame machine;
    machine.SetSP(last + 4096 + 100);
    const FrameInfo frame(machine, FrameType::MANAGED);
    GC_EXPECT_EQ(watermark.prev_frame_color(frame), uintptr_t(first));
    std::fprintf(stderr, "GROW_RESULT mark=%zx last=%zx color=%zx\n", watermark.watermark(),
        watermark.last_processed_raw(), watermark.prev_frame_color(frame));
}

// safepoint.cpp:818-839: a return oop stays the value published by request
// processing, including when this epoch's frame walk has already started.
GC_TEST(StackWatermark, ReturnRootIdentityAcrossRequest)
{
    EnsureImages();
    uint32_t* epoch = ZPointerStoreGoodMaskLowOrderBitsAddr;
    const uint32_t savedEpoch = *epoch;
    ThreadLocalData* tls = ThreadLocal::GetThreadLocalData();
    Mutator* savedMutator = tls->mutator;
    const uintptr_t savedPoll = tls->GetPollWord();
    struct Restore {
        ThreadLocalData* tls;
        Mutator* mutator;
        uintptr_t poll;
        uint32_t* epoch;
        uint32_t epochValue;
        ~Restore()
        {
            tls->SetMutator(mutator);
            tls->SetPollWord(poll);
            *epoch = epochValue;
        }
    } restore { tls, savedMutator, savedPoll, epoch, savedEpoch };
    alignas(16) static char originalBytes[16];
    alignas(16) static char replacedBytes[16];
    BaseObject* original = reinterpret_cast<BaseObject*>(originalBytes);
    BaseObject* replaced = reinterpret_cast<BaseObject*>(replacedBytes);
    Mutator owner;
    tls->SetMutator(&owner);
    const uint32_t first = savedEpoch == 7 ? 8 : 7;
    *epoch = first;
    alignas(16) uint64_t raw[48];
    std::memset(raw, 0, sizeof(raw));
    FrameAddress* stub = reinterpret_cast<FrameAddress*>(&raw[8]);
    ChainNode caller;
    ChainNode anchor;
    std::memset(&caller, 0, sizeof(caller));
    std::memset(&anchor, 0, sizeof(anchor));
    LinkManaged(caller, &anchor.fa, reinterpret_cast<const uint32_t*>(&ManagedFrameIp));
    anchor.fa.returnAddress = nullptr;
    stub->callerFrameAddress = &caller.fa;
    stub->returnAddress = reinterpret_cast<const uint32_t*>(&ManagedFrameIp);
    const uintptr_t startPC = reinterpret_cast<uintptr_t>(gReturnDesc.pc);
    *StubSlot(stub, kStartSlot) = startPC;
    *StubSlot(stub, kSiteSlot) = startPC + 16;
    StorePlain(RootSlotAt(StubSlot(stub, kReturnSlot)), from_object(original));
    UnwindContext& context = owner.GetUnwindContext();
    context.frameInfo.mFrame.SetFA(stub);
    context.frameInfo.mFrame.SetIP(&unwindPCForReturnSafepointHandlerStub);
    context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
    MachineFrame callerMachine;
    callerMachine.SetFA(&caller.fa);
    callerMachine.SetSP(FrameInfo(context.frameInfo.mFrame, FrameType::RETURN_SAFEPOINT).CallerSP());
    const FrameInfo callerFrame(callerMachine, FrameType::MANAGED);
    GC_EXPECT_FALSE(owner.GetStackWatermark().is_frame_safe(callerFrame));
    RewriteReturnRoot cold(original, replaced);
    HandshakeOperation coldOp(&cold, tls);
    Handshake::Current().add_operation(&coldOp);
    HandleReturnSafepoint(tls);
    const BaseObject* coldValue = to_object(safe(RootSlotAt(StubSlot(stub, kReturnSlot)).LoadPlain()));
    GC_EXPECT_TRUE(cold.rewritten);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(coldValue), reinterpret_cast<uintptr_t>(replaced));
    GC_EXPECT_TRUE(owner.GetStackWatermark().is_frame_safe(callerFrame));
    std::fprintf(stderr, "RETURN_ROOT_RESULT phase=cold value=%p safe=1\n", coldValue);
    StorePlain(RootSlotAt(StubSlot(stub, kReturnSlot)), from_object(original));
    RewriteReturnRoot warm(original, replaced);
    HandshakeOperation warmOp(&warm, tls);
    Handshake::Current().add_operation(&warmOp);
    HandleReturnSafepoint(tls);
    const BaseObject* warmValue = to_object(safe(RootSlotAt(StubSlot(stub, kReturnSlot)).LoadPlain()));
    GC_EXPECT_TRUE(warm.rewritten);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(warmValue), reinterpret_cast<uintptr_t>(replaced));
    GC_EXPECT_TRUE(owner.GetStackWatermark().is_frame_safe(callerFrame));
    std::fprintf(stderr, "RETURN_ROOT_RESULT phase=started value=%p safe=1\n", warmValue);
}
#endif
