// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// HotSpot safepoint.cpp:800-806 protects the value a returning frame hands back
// through one saved register slot (cpu/x86/frame_x86.inline.hpp:371-376); no
// stack base is involved. StackFrameCursor::ProcessReturnFrame builds the
// return-point map with base 0 (StackFrameCursor.cpp:136), so nothing in that
// map may be visited through the base: HeapReferenceMap::VisitDerivedPtr
// (StackMap.h:130-149) unconditionally scans slot roots at stackBase + bias and
// SlotRoot::VisitGCRoots (SlotRoot.h:52-75) loads through that address.
//
// The return-point metadata is fabricated in-process (the pattern already used
// by test_remap_young_roots.cpp) so the row really carries one register root
// and one slot root. The product decoder builds the map; only the metadata
// bytes and the stub frame are supplied by the test.

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <setjmp.h>

#include "CangjieRuntime.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Loader/ElfUnloadQuiescence.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/StackFrameCursor.h"
#include "gc_unittest.hpp"

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#define MRT_TEST_RETURN_FRAME_SLOT_ROOT 1
#endif

#if defined(MRT_TEST_RETURN_FRAME_SLOT_ROOT)

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

ReturnPointFuncDesc gReturnSlotDesc;
bool gImageLinked = false;

struct Bits {
    uint8_t* data;
    uint32_t bit = 0;
    void Put(uint32_t value, unsigned width)
    {
        for (unsigned i = 0; i < width; ++i, ++bit) { data[bit / 8] |= static_cast<uint8_t>(((value >> i) & 1u) << (bit % 8)); }
    }
    void Var(uint32_t value)
    {
        if (value <= 11) {
            Put(value, 4);
        } else {
            Put(12, 4);
            Put(value, 8);
        }
    }
};

// Compressed stack map: stacksize, compressed format, prologue bitmap, then the
// map table header (2 rows, 4 reg bits, 1 slot bit, 1 line bit, 1 derived bit),
// both rows, the register table, the slot table (one row, bias 0, slot 0) and
// the empty line/derived tables. The row at pc offset 16 -- the return poll
// site -- names register 0 as a root and slot 0 as a root.
void BuildReturnPointBits(uint8_t* data)
{
    std::memset(data, 0, 256);
    Bits b {data};
    b.Var(0);
    b.Var(0);
    b.Var(0);
    b.Var(2);
    b.Var(4);
    b.Var(1);
    b.Var(1);
    b.Var(1);
    b.Var(0);
    b.Put(0, 32);
    b.Put(0, 4);
    b.Put(0, 1);
    b.Put(0, 1);
    b.Put(0, 1);
    b.Put(16, 32);
    b.Put(1, 4);
    b.Put(1, 1);
    b.Put(0, 1);
    b.Put(0, 1);
    b.Var(1);
    b.Var(16);
    b.Put(1, 16);
    b.Var(1);
    b.Var(8);
    b.Var(1);
    b.Put(0, 8);
    b.Put(1, 1);
    b.Var(0);
    b.Var(0);
    b.Var(0);
}

uintptr_t ReturnPointStartPC()
{
    if (!gImageLinked) {
        std::memset(&gReturnSlotDesc, 0, sizeof(gReturnSlotDesc));
        gReturnSlotDesc.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&gReturnSlotDesc.stackMapOffset) -
            reinterpret_cast<char*>(&gReturnSlotDesc.descriptorOffset));
        gReturnSlotDesc.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(gReturnSlotDesc.bits) -
            reinterpret_cast<char*>(&gReturnSlotDesc.stackMapOffset));
        BuildReturnPointBits(gReturnSlotDesc.bits);
        ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(gReturnSlotDesc.pc));
        gImageLinked = true;
    }
    return reinterpret_cast<uintptr_t>(gReturnSlotDesc.pc);
}

// The stub register area is process-static: the product reads it after this
// frame is gone, and a stack array would let the compiler reorder the clearing
// against the slot stores.
alignas(16) uint64_t gStubArea[64];

volatile uint64_t* StubSlotStore(FrameAddress* fa, int index)
{
    return reinterpret_cast<volatile uint64_t*>(reinterpret_cast<uint64_t*>(fa) - 1 - index);
}

#if defined(__x86_64__)
uint64_t* StubSlot(FrameAddress* fa, int index)
{
    return reinterpret_cast<uint64_t*>(fa) - 1 - index;
}
constexpr int kReturnSlot = 0;
constexpr int kStartSlot = 9;
constexpr int kSiteSlot = 10;
#else
uint64_t* StubSlot(FrameAddress* fa, int index)
{
    return reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(fa) + 16 + static_cast<uintptr_t>(index) * 8);
}
constexpr int kReturnSlot = 0;
constexpr int kSiteSlot = 16;
constexpr int kStartSlot = 17;
#endif

// A near-zero load from the product is a real fault, not an assertion failure.
// Trap it so the target assertion reports it instead of killing the suite.
sigjmp_buf gFaultJump;
volatile sig_atomic_t gFaulted = 0;
volatile uintptr_t gFaultAddress = 0;
volatile int gFaultCode = 0;

void FaultHandler(int signal, siginfo_t* info, void*)
{
    gFaulted = 1;
    gFaultAddress = reinterpret_cast<uintptr_t>(info->si_addr);
    gFaultCode = info->si_code;
    siglongjmp(gFaultJump, 1);
}

struct FaultScope {
    FaultScope()
    {
        struct sigaction action {};
        action.sa_sigaction = FaultHandler;
        action.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &saved);
        sigaction(SIGBUS, &action, &savedBus);
    }
    ~FaultScope()
    {
        sigaction(SIGSEGV, &saved, nullptr);
        sigaction(SIGBUS, &savedBus, nullptr);
    }
    struct sigaction saved {};
    struct sigaction savedBus {};
};

} // namespace

// The returned frame has no stack base, so a return-point map that names a slot
// root must not send the product to that slot: no fault, no derived visit, and
// the returned register root still protected.
GC_TEST(ReturnFrameSlotRoot, ReturnPointMapIsNotVisitedThroughStackBase)
{
    const StackGrowConfig savedGrow = CangjieRuntime::stackGrowConfig;
    CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_OFF;
    struct Restore {
        StackGrowConfig* slot;
        StackGrowConfig value;
        ~Restore() { *slot = value; }
    } restore { &CangjieRuntime::stackGrowConfig, savedGrow };

    const uintptr_t startPC = ReturnPointStartPC();
    const uintptr_t sitePC = startPC + 16;
    // The product builder decodes the fabricated metadata: the return point row
    // really carries a register root and a slot root. Existence only -- a
    // failure here means the injection is wrong, not the product.
    HeapReferenceMap decoded = StackMapBuilder(startPC, sitePC, 0).Build<HeapReferenceMap>();
    const StackMapRootCounts counts = decoded.CountRootSlots();
    std::fprintf(stderr, "RETURN_SLOT_INJECT map_valid=%d slot_roots=%zu reg_roots=%zu derived_rows=%zu\n",
        decoded.IsValid() ? 1 : 0, counts.baseSlots, counts.baseRegs, counts.derivedRegs + counts.derivedSlots);
    GC_EXPECT_TRUE(decoded.IsValid());
    GC_EXPECT_EQ(counts.baseSlots, size_t {1});
    GC_EXPECT_EQ(counts.baseRegs, size_t {1});

    std::memset(gStubArea, 0, sizeof(gStubArea));
    alignas(16) static char returnedBytes[16] {};
    alignas(16) static char spareBytes[16] {};
    BaseObject* returned = reinterpret_cast<BaseObject*>(returnedBytes);
    BaseObject* expected = reinterpret_cast<BaseObject*>(spareBytes);
    FrameAddress* stub = reinterpret_cast<FrameAddress*>(&gStubArea[32]);
    stub->callerFrameAddress = nullptr;
    stub->returnAddress = nullptr;
    *StubSlotStore(stub, kStartSlot) = startPC;
    *StubSlotStore(stub, kSiteSlot) = sitePC;
    StorePlain(RootSlotAt(StubSlot(stub, kReturnSlot)), from_object(returned));
    std::atomic_thread_fence(std::memory_order_seq_cst);

    MachineFrame machine;
    machine.SetFA(stub);
    machine.SetSP(reinterpret_cast<uintptr_t>(&gStubArea[0]));
    const FrameInfo frame(machine, FrameType::RETURN_SAFEPOINT);
    RegSlotsMap regSlotsMap;
    size_t rootVisits = 0;
    size_t derivedVisits = 0;
    const RootVisitor roots = [&](RootSlot& slot) {
        ++rootVisits;
        if (to_object(safe(slot.LoadPlain())) == returned) {
            StorePlain(slot, from_object(expected));
        }
    };
    const DerivedPtrVisitor derived = [&](BasePtrType, DerivedSlot&) { ++derivedVisits; };

    gFaulted = 0;
    gFaultAddress = 0;
    gFaultCode = 0;
    {
        FaultScope scope;
        if (sigsetjmp(gFaultJump, 1) == 0) {
            StackFrameCursor::ProcessReturnFrame(roots, &derived, regSlotsMap, frame);
        }
    }
    // The product result, read back from the stub frame after the call.
    const BaseObject* kept = to_object(safe(RootSlotAt(StubSlot(stub, kReturnSlot)).LoadPlain()));
    std::fprintf(stderr, "RETURN_SLOT_TARGET faulted=%d fault_addr=%p si_code=%d derived_visits=%zu root_visits=%zu "
        "returned=%p expected=%p\n", static_cast<int>(gFaulted), reinterpret_cast<void*>(gFaultAddress),
        static_cast<int>(gFaultCode), derivedVisits, rootVisits, reinterpret_cast<const void*>(kept),
        static_cast<const void*>(returned));
    GC_EXPECT_FALSE(gFaulted != 0);
    GC_EXPECT_EQ(derivedVisits, size_t {0});
    GC_EXPECT_EQ(rootVisits, size_t {1});
    // The register root the product protected and rewrote is read back from the
    // stub frame: the returned value is the one this visitor published.
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(kept), reinterpret_cast<uintptr_t>(expected));
}
#endif
