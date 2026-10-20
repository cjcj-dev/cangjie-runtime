#include "b09_runtime_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zStat.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"

#include <cstdint>
#include <cstdio>
#include <ucontext.h>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(__linux__) && defined(__x86_64__)
extern "C" {
struct CJThread;
typedef uintptr_t* (*GrowTlsHook)();
#ifndef ScheduleGetTlsHookRegister
int CJ_ScheduleGetTlsHookRegister(GrowTlsHook func);
#endif
uintptr_t MRT_GetThreadLocalData();
#ifndef CJThreadNew
void* CJ_CJThreadNew(void* schedule, const void* attr, void* (*func)(void*, unsigned int), const void* arg,
    unsigned int argSize, int createSource, uintptr_t rootColor);
void CJ_CJThreadAttrInit(struct CJThreadAttr* attr);
void CJ_CJThreadAttrStackSizeSet(struct CJThreadAttr* attr, unsigned int size);
size_t CJ_CJThreadStackSizeGetByCJThrd(struct CJThread* cjthread);
void* CJ_CJThreadStackAddrGetByCJThrd(struct CJThread* cjthread);
void* CJ_CJThreadStackBaseAddrGetByCJThrd(struct CJThread* cjthread);
#endif
intptr_t MRT_StackGrow(intptr_t frameBase, uint32_t adjustedSize, void* ip);
}

namespace {

constexpr size_t kChainEndStack = 1073741824;
constexpr unsigned kInitialStack = 1024 * 1024;

enum class GrowArm { kChainEnd, kFfi };

struct GrowResult {
    intptr_t offset;
    uintptr_t fa;
    const uint32_t* ip;
    uint32_t* anchor;
    uint32_t* expectedAnchor;
    size_t before;
    size_t after;
    bool lockReleased;
};

GrowResult g_result;
Mutator* g_mutator;
struct CJThread* g_thread;
ucontext_t g_back;
GrowArm g_arm;

void* UnusedEntry(void*, unsigned int)
{
    return nullptr;
}

void GrowBody();
extern "C" void GrowEntry()
{
    GrowBody();
}

void GrowBody()
{
    ThreadLocal::SetThreadType(ThreadType::CJ_PROCESSOR);
    (void)CJ_ScheduleGetTlsHookRegister(reinterpret_cast<GrowTlsHook>(MRT_GetThreadLocalData));
    ThreadLocal::SetCJThread(g_thread);
    ThreadLocal::SetMutator(g_mutator);
    g_mutator->InitStackInfo(ThreadLocal::GetThreadLocalData());
    UnwindContext& context = g_mutator->GetUnwindContext();
    context.GoIntoManagedCode();
    uint32_t anchorSlot = 0;
    context.anchorFA = &anchorSlot;
    g_result.fa = reinterpret_cast<uintptr_t>(context.frameInfo.mFrame.GetFA());
    g_result.ip = context.frameInfo.mFrame.GetIP();
    if (g_result.fa != 0 || g_result.ip != nullptr) {
        g_result.offset = -2;
        return;
    }
    g_result.before = CJ_CJThreadStackSizeGetByCJThrd(g_thread);
    const intptr_t frameBase = g_arm == GrowArm::kFfi ? 0
        : reinterpret_cast<intptr_t>(__builtin_frame_address(0));
    const intptr_t offset = MRT_StackGrow(frameBase, 0, nullptr);
    g_result.offset = offset;
    g_result.fa = reinterpret_cast<uintptr_t>(context.frameInfo.mFrame.GetFA());
    g_result.ip = context.frameInfo.mFrame.GetIP();
    g_result.anchor = context.anchorFA;
    g_result.expectedAnchor = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uintptr_t>(&anchorSlot) + static_cast<uintptr_t>(offset));
    g_result.after = CJ_CJThreadStackSizeGetByCJThrd(g_thread);
    g_mutator->GetStackWatermark().start_processing();
    g_result.lockReleased = true;
}

void RunArm(GrowArm arm)
{
    B09RuntimeFixture runtime(8);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    Mutator* mutator = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_TRUE(mutator != nullptr);
    CJThreadAttr attr{};
    CJ_CJThreadAttrInit(&attr);
    CJ_CJThreadAttrStackSizeSet(&attr, kInitialStack);
    void* scheduler = runtime.GetConcurrencyModel().GetThreadScheduler();
#ifdef CJThreadNew
    void* created = CJThreadNew(scheduler, &attr, UnusedEntry, nullptr, 0, CJTHREAD_CREATE_SOURCE_DEFAULT, 0);
#else
    void* created = CJ_CJThreadNew(scheduler, &attr, UnusedEntry, nullptr, 0, 0, 0);
#endif
    GC_EXPECT_TRUE(created != nullptr);
    auto* thread = reinterpret_cast<struct CJThread*>(created);
    void* low = CJ_CJThreadStackAddrGetByCJThrd(thread);
    void* high = CJ_CJThreadStackBaseAddrGetByCJThrd(thread);
    GC_EXPECT_TRUE(low != nullptr && high != nullptr);
    g_result = {};
    g_mutator = mutator;
    g_thread = thread;
    g_arm = arm;
    ucontext_t stackContext{};
    GC_EXPECT_EQ(getcontext(&stackContext), 0);
    stackContext.uc_stack.ss_sp = low;
    stackContext.uc_stack.ss_size = reinterpret_cast<uintptr_t>(high) - reinterpret_cast<uintptr_t>(low);
    stackContext.uc_link = &g_back;
    makecontext(&stackContext, GrowEntry, 0);
    GC_EXPECT_EQ(swapcontext(&g_back, &stackContext), 0);
    std::fprintf(stderr,
        "FIX_EXTENDED_STACK_FA0 arm=%d offset=%ld before=%zu after=%zu fa=%#zx ip=%p anchor=%p expected=%p lock=%d\n",
        static_cast<int>(arm), static_cast<long>(g_result.offset), g_result.before, g_result.after,
        g_result.fa, static_cast<const void*>(g_result.ip), static_cast<void*>(g_result.anchor),
        static_cast<void*>(g_result.expectedAnchor), g_result.lockReleased ? 1 : 0);
    GC_EXPECT_TRUE(g_result.offset > 0);
    GC_EXPECT_EQ(g_result.fa, static_cast<uintptr_t>(0));
    GC_EXPECT_TRUE(g_result.ip == nullptr);
    GC_EXPECT_TRUE(g_result.before < kChainEndStack);
    GC_EXPECT_TRUE(g_result.after >= kChainEndStack);
    GC_EXPECT_EQ(g_result.anchor, g_result.expectedAnchor);
    GC_EXPECT_TRUE(g_result.lockReleased);
    (void)mutator->EnterSaferegion(false);
}

} // namespace

GC_COMPONENT_OTHER_VM_TEST(FixExtendedStackFa0, GrowsAtChainEndWithoutUnwind)
{
    RunArm(GrowArm::kChainEnd);
}

GC_COMPONENT_OTHER_VM_TEST(FixExtendedStackFa0, FfiMaxGrowStillReturnsOffset)
{
    RunArm(GrowArm::kFfi);
}
#else
GC_COMPONENT_OTHER_VM_TEST(FixExtendedStackFa0, GrowsAtChainEndWithoutUnwind)
{
    GC_EXPECT_TRUE(false);
}
GC_COMPONENT_OTHER_VM_TEST(FixExtendedStackFa0, FfiMaxGrowStillReturnsOffset)
{
    GC_EXPECT_TRUE(false);
}
#endif
