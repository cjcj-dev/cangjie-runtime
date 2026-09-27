// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// ZStackWatermark::process (zStackWatermark.cpp:209) consumes the register
// locations produced by the stub. Cangjie has no return statepoint, so the
// return map is supplied explicitly (HotSpot safepoint.cpp:800-839).
#include <cstdio>
#include <cstring>
#include <vector>
#include "CangjieRuntime.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/Handshake.h"
#include "Mutator/MutatorManager.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Loader/ElfUnloadQuiescence.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/StackFrameCursor.h"
#include "gc_unittest.hpp"

#if defined(__linux__) && defined(__x86_64__)
using namespace MapleRuntime;
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
        if (value <= 11) { Put(value, 4); } else { Put(12, 4); Put(value, 8); }
    }
};
void InitDescriptor(Descriptor& d, uint64_t mask)
{
    std::memset(&d, 0, sizeof(d));
    d.descriptorOffset = reinterpret_cast<char*>(&d.stackMapOffset) - reinterpret_cast<char*>(&d.descriptorOffset);
    d.stackMapOffset = reinterpret_cast<char*>(d.bits) - reinterpret_cast<char*>(&d.stackMapOffset);
    Bits b {d.bits};
    b.Var(0); b.Var(0); b.Var(0); // stack size, format, prologue
    b.Var(2); b.Var(1); b.Var(1); b.Var(1); b.Var(1); b.Var(0);
    b.Put(0, 32); b.Put(0, 1); b.Put(0, 1); b.Put(0, 1); b.Put(0, 1);
    b.Put(16, 32); b.Put(1, 1); b.Put(0, 1); b.Put(0, 1); b.Put(0, 1);
    b.Var(1); b.Var(33); b.Put(mask, 33);
    b.Var(0); b.Var(0); b.Var(0); // empty slot table
    b.Var(0); b.Var(0); // empty line table
    b.Var(0); // empty derived table
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(d.pc));
}
struct ConfigScope {
    StackGrowConfig old = CangjieRuntime::stackGrowConfig;
    ConfigScope() { CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_OFF; }
    ~ConfigScope() { CangjieRuntime::stackGrowConfig = old; }
};
// Independent oracle: register order emitted by the 15 push instructions,
// followed by a 264-byte vector save area. Do not use X86StubLayout.h here.
constexpr unsigned savedGprs[] = {0, 3, 2, 1, 5, 4, 7, 8, 9, 10, 11, 12, 13, 14, 15};
void CheckSlots(bool xmm)
{
    ConfigScope config;
    static Descriptor desc[33];
    size_t mismatches = 0;
    for (unsigned reg = 0; reg < 33; ++reg) {
        if (xmm ? reg < 17 : (reg > 15 || reg == 6)) { continue; }
        InitDescriptor(desc[reg], uint64_t(1) << reg);
        alignas(16) uintptr_t storage[64] {};
        auto* fp = &storage[56];
        for (unsigned i = 0; i < 15; ++i) { fp[-1 - int(i)] = 0x10000 + savedGprs[i] * 16; }
        for (unsigned i = 0; i < 32; ++i) { fp[-48 + int(i)] = 0x20000 + i * 16; }
        fp[-10] = reinterpret_cast<uintptr_t>(desc[reg].pc);
        fp[-11] = fp[-10] + 16;
        uintptr_t* expected = nullptr;
        if (xmm) { expected = fp - 48 + 2 * (reg - 17); }
        else {
            for (unsigned i = 0; i < 15; ++i) { if (savedGprs[i] == reg) { expected = fp - 1 - i; } }
        }
        const uintptr_t old = expected[0];
        const uintptr_t oldHigh = xmm ? expected[1] : 0;
        MachineFrame machine;
        machine.SetFA(reinterpret_cast<FrameAddress*>(fp));
        machine.SetSP(reinterpret_cast<uintptr_t>(storage));
        FrameInfo frame(machine, FrameType::RETURN_SAFEPOINT);
        RegSlotsMap locations;
        std::vector<uintptr_t> addresses;
        std::vector<uintptr_t> values;
        RootVisitor visitor = [&](RootSlot& slot) {
            addresses.push_back(reinterpret_cast<uintptr_t>(&slot));
            values.push_back(raw(slot.LoadPlain()));
            StorePlain(slot, to_zaddress(0x40000));
        };
        StackFrameCursor::ProcessReturnFrame(visitor, nullptr, locations, frame);
        bool ok = addresses.size() == (xmm ? 2u : 1u);
        ok = ok && addresses[0] == reinterpret_cast<uintptr_t>(expected) && values[0] == old && expected[0] == 0x40000;
        if (xmm) {
            ok = ok && addresses[1] == reinterpret_cast<uintptr_t>(expected + 1) && values[1] == oldHigh && expected[1] == 0x40000;
        }
        std::fprintf(stderr, "STUB_SLOT_TARGET reg=%u visits=%zu expected=%p match=%d\n", reg, addresses.size(), expected, ok);
        mismatches += !ok;
    }
    GC_EXPECT_EQ(mismatches, size_t(0));
}
}
GC_TEST(StubRegisterRoots, GprSlots) { CheckSlots(false); }
GC_TEST(StubRegisterRoots, XmmSlots) { CheckSlots(true); }
GC_TEST(StubRegisterRoots, BitmapPositions)
{
    ConfigScope config;
    static Descriptor desc[33];
    size_t mismatches = 0;
    for (unsigned bit = 0; bit < 33; ++bit) {
        InitDescriptor(desc[bit], uint64_t(1) << bit);
        const auto pc = reinterpret_cast<uintptr_t>(desc[bit].pc);
        auto map = StackMapBuilder(pc, pc + 16, 0).Build<HeapReferenceMap>();
        const auto count = map.CountRootSlots().baseRegs;
        const size_t expected = bit < 16 ? 1 : bit == 16 ? 0 : 2;
        std::fprintf(stderr, "STUB_BITMAP_TARGET bit=%u count=%zu expected=%zu\n", bit, count, expected);
        mismatches += !map.IsValid() || count != expected;
    }
    GC_EXPECT_EQ(mismatches, size_t(0));
}

namespace {
class StubModel final : public ConcurrencyModel {
public:
    void VisitGCRoots(RootVisitor*) override {}
    size_t GetReservedStackSize() const override { return 0; }
    bool GetStackGuardCheckFlag() const override { return false; }
};
class StubRuntime final : public Runtime {
public:
    StubRuntime(MutatorManager& manager, ConcurrencyModel& model)
    {
        mutatorManager = &manager; concurrencyModel = &model; runtime = this;
    }
    ~StubRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}
};
class RewriteXmm final : public HandshakeClosure {
public:
    uintptr_t* before;
    uintptr_t* after;
    uint32_t seen = 0;
    RewriteXmm(uintptr_t* from, uintptr_t* to) : HandshakeClosure("stub-xmm"), before(from), after(to) {}
    void do_thread(Mutator* thread) override
    {
        thread->VisitMutatorRoots([&](RootSlot& slot) {
            const auto value = reinterpret_cast<uintptr_t>(to_object(safe(slot.LoadPlain())));
            for (unsigned i = 0; i < 32; ++i) {
                if (value == before[i]) {
                    seen |= uint32_t(1) << i;
                    StorePlain(slot, from_object(reinterpret_cast<BaseObject*>(after[i])));
                }
            }
        });
    }
};
// This is only the native-to-managed ABI bridge. The saves, root discovery,
// handles and restores under test all belong to CJ_MCC_HandleReturnSafepoint.
extern "C" void InvokeStubXmm(ThreadLocalData*, uintptr_t, uintptr_t*, uintptr_t*);
asm(R"(
.text
.global InvokeStubXmm
.hidden InvokeStubXmm
.type InvokeStubXmm,@function
InvokeStubXmm:
 pushq %rbp
 movq %rsp,%rbp
 pushq %r12
 pushq %r13
 pushq %r15
 subq $8,%rsp
 movq %rdi,%r15
 movq %rsi,%r10
 leaq 16(%rsi),%r11
 movq %rdx,%r12
 movq %rcx,%r13
 movdqu 0(%r12),%xmm0
 movdqu 16(%r12),%xmm1
 movdqu 32(%r12),%xmm2
 movdqu 48(%r12),%xmm3
 movdqu 64(%r12),%xmm4
 movdqu 80(%r12),%xmm5
 movdqu 96(%r12),%xmm6
 movdqu 112(%r12),%xmm7
 movdqu 128(%r12),%xmm8
 movdqu 144(%r12),%xmm9
 movdqu 160(%r12),%xmm10
 movdqu 176(%r12),%xmm11
 movdqu 192(%r12),%xmm12
 movdqu 208(%r12),%xmm13
 movdqu 224(%r12),%xmm14
 movdqu 240(%r12),%xmm15
 callq CJ_MCC_HandleReturnSafepoint@PLT
 movdqu %xmm0,0(%r13)
 movdqu %xmm1,16(%r13)
 movdqu %xmm2,32(%r13)
 movdqu %xmm3,48(%r13)
 movdqu %xmm4,64(%r13)
 movdqu %xmm5,80(%r13)
 movdqu %xmm6,96(%r13)
 movdqu %xmm7,112(%r13)
 movdqu %xmm8,128(%r13)
 movdqu %xmm9,144(%r13)
 movdqu %xmm10,160(%r13)
 movdqu %xmm11,176(%r13)
 movdqu %xmm12,192(%r13)
 movdqu %xmm13,208(%r13)
 movdqu %xmm14,224(%r13)
 movdqu %xmm15,240(%r13)
 addq $8,%rsp
 popq %r15
 popq %r13
 popq %r12
 popq %rbp
 retq
.size InvokeStubXmm,.-InvokeStubXmm
)");
}
GC_OTHER_VM_TEST(StubRegisterRoots, RealReturnStub)
{
    ConfigScope config;
    MutatorManager manager;
    StubModel model;
    StubRuntime instance(manager, model);
    ThreadLocalData* tls = ThreadLocal::GetThreadLocalData();
    manager.RegisterMarkFlushThread(tls);
    Mutator owner;
    owner.SetManagedContext(false);
    tls->SetMutator(&owner);
    static Descriptor descriptor;
    InitDescriptor(descriptor, ((uint64_t(1) << 16) - 1) << 17);
    alignas(16) char from[32][16] {};
    alignas(16) char to[32][16] {};
    uintptr_t before[32], after[32], actual[32] {};
    for (unsigned i = 0; i < 32; ++i) {
        before[i] = reinterpret_cast<uintptr_t>(from[i]);
        after[i] = reinterpret_cast<uintptr_t>(to[i]);
    }
    RewriteXmm rewrite(before, after);
    HandshakeOperation operation(&rewrite, &owner);
    Handshake::Current().add_operation(&operation);
    InvokeStubXmm(tls, reinterpret_cast<uintptr_t>(descriptor.pc), before, actual);
    size_t mismatches = 0;
    for (unsigned i = 0; i < 32; ++i) {
        std::fprintf(stderr, "STUB_RETURN_TARGET lane=%u actual=%zx expected=%zx match=%d\n", i, actual[i], after[i], actual[i] == after[i]);
        mismatches += actual[i] != after[i];
    }
    std::fprintf(stderr, "STUB_RETURN_TARGET seen=%08x mismatches=%zu\n", rewrite.seen, mismatches);
    tls->SetMutator(nullptr);
    GC_EXPECT_EQ(mismatches, size_t(0));
    GC_EXPECT_EQ(rewrite.seen, UINT32_MAX);
}
#endif
