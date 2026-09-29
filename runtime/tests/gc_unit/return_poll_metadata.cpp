// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Real LLVM frames -> product safepoint entry -> observed watermark frontier.
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "CangjieRuntime.h"
#include "Loader/ElfUnloadQuiescence.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
using namespace MapleRuntime;
class PairConcurrencyModel final : public ConcurrencyModel {
public:
 void VisitGCRoots(RootVisitor*) override {}
 size_t GetReservedStackSize() const override { return 0; }
 bool GetStackGuardCheckFlag() const override { return false; }
};
class PairRuntime final : public Runtime {
public:
 PairRuntime(MutatorManager& manager, ConcurrencyModel& model)
 {
  mutatorManager = &manager; concurrencyModel = &model; runtime = this;
 }
 ~PairRuntime() override { runtime = nullptr; }
 RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
 void SetGCThreshold(uint64_t) override {}
};

extern "C" void poll_chain(int);
extern "C" void no_poll_chain(int);
extern "C" void mixed_chain(int);
extern "C" void invoke_metadata(ThreadLocalData*, void (*)(int));
extern "C" uintptr_t metadata_anchor;
uintptr_t metadata_anchor;
asm(R"(
.text
.global invoke_metadata
.type invoke_metadata,@function
invoke_metadata:
 pushq %rbp
 movq %rsp,%rbp
 pushq %r15
 subq $8,%rsp
 movq %rbp,metadata_anchor(%rip)
 movq %rdi,%r15
 movq $9,%rdi
 callq *%rsi
 addq $8,%rsp
 popq %r15
 popq %rbp
 retq
.size invoke_metadata,.-invoke_metadata
)");
static bool expectPoll;
static bool passed;
static bool mixed;
static int drainMode;
extern "C" __attribute__((noinline)) void observe_watermark(int)
{
    auto& owner = *Mutator::GetMutator();
    auto& context = owner.GetUnwindContext();
    auto* observerFP = reinterpret_cast<uintptr_t*>(__builtin_frame_address(0));
    auto* managedFP = reinterpret_cast<uintptr_t*>(observerFP[0]);
    context.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(managedFP));
    context.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(__builtin_return_address(0)));
    context.frameInfo.mFrame.SetSP(reinterpret_cast<uintptr_t>(observerFP + 2));
    context.anchorFA = reinterpret_cast<uint32_t*>(metadata_anchor);
    context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
    owner.SetManagedContext(true);
    const uint32_t epoch = *ZPointerStoreGoodMaskLowOrderBitsAddr;
    *ZPointerStoreGoodMaskLowOrderBitsAddr = epoch == 7 ? 8 : 7;
    StackWatermarkSet::on_safepoint(owner);
    auto& watermark = owner.GetStackWatermark();
    const auto expected = expectPoll ? reinterpret_cast<uintptr_t*>(managedFP[0]) + 2 : nullptr;
    const uintptr_t frontier = watermark.last_processed_raw();
    const bool done = watermark.IsDone();
    passed = frontier == reinterpret_cast<uintptr_t>(expected) && done == !expectPoll;
    std::fprintf(stderr, "RETURN_METADATA_TARGET poll=%d mode=%d frontier=%p expected=%p done=%d pass=%d\n",
                 expectPoll, drainMode, reinterpret_cast<void*>(frontier), expected, done, passed);
    if (drainMode == 1) {
        watermark.finish_processing(nullptr);
    } else if (drainMode == 2) {
        watermark.BeginGrowFlush();
        watermark.EndGrowFlush();
    }
    if (drainMode != 0) {
        auto* fp = managedFP;
        uintptr_t previousSP = reinterpret_cast<uintptr_t>(observerFP + 2);
        uintptr_t lastBarrier = 0;
        unsigned frameIndex = 0;
        while (fp != reinterpret_cast<uintptr_t*>(metadata_anchor)) {
            auto* caller = reinterpret_cast<uintptr_t*>(fp[0]);
            if (expectPoll && (!mixed || frameIndex < 4) && caller != reinterpret_cast<uintptr_t*>(metadata_anchor)) { lastBarrier = previousSP; }
            previousSP = reinterpret_cast<uintptr_t>(fp + 2);
            fp = caller;
            ++frameIndex;
        }
        const bool drainPassed = watermark.IsDone() && watermark.last_processed_raw() == lastBarrier;
        std::fprintf(stderr, "RETURN_METADATA_DRAIN mode=%d frontier=%p expected=%p done=%d pass=%d\n",
                     drainMode, reinterpret_cast<void*>(watermark.last_processed_raw()),
                     reinterpret_cast<void*>(lastBarrier), watermark.IsDone(), drainPassed);
        passed &= drainPassed;
    }
    *ZPointerStoreGoodMaskLowOrderBitsAddr = epoch;
    watermark.Reset();
    owner.SetManagedContext(false);
}
int main(int argc, char** argv)
{
    const int mode = argc > 1 ? std::atoi(argv[1]) : 1;
    drainMode = argc > 2 ? std::atoi(argv[2]) : 0;
    mixed = mode == 5;
    expectPoll = mode == 1 || mode == 3 || mixed;
    CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_ON;
    MutatorManager manager; PairConcurrencyModel model; PairRuntime instance(manager, model);
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(&poll_chain));
    auto* tls = ThreadLocal::GetThreadLocalData();
    ZGlobalsPointers::initialize();
    Mutator owner; owner.SetManagedContext(false); tls->SetMutator(&owner);
    void (*function)(int) = mixed ? mixed_chain : (mode == 0 ? no_poll_chain : poll_chain);
    // Reserved-bit and missing-descriptor inputs are changes to compiler output,
    // never a replacement implementation of has_barrier or the frame iterator.
    if (mode >= 2 && mode <= 4) {
        auto* slot = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(function) - 4);
        auto* desc = reinterpret_cast<uint8_t*>(slot) + *slot;
        void* target = mode == 4 ? static_cast<void*>(slot) : static_cast<void*>(desc + 28);
        const auto pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
        auto* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(target) & ~(pageSize - 1));
        if (mprotect(page, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) { return 2; }
        const uint32_t value = mode == 4 ? 0 : (mode == 3 ? 3 : 2);
        std::memcpy(target, &value, sizeof(value));
    }
    invoke_metadata(tls, function);
    tls->SetMutator(nullptr);
    // This minimal fixture has no heap generations for the process TLS destructor.
    // Product results and all assertions are complete before process teardown.
    std::fflush(nullptr);
    std::_Exit(passed ? 0 : 1);
}
