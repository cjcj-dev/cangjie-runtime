#include "b09_runtime_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zStat.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"

#include <cstdint>
#include <cstdio>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(__linux__) && defined(__x86_64__)
#include "Exception/Exception.h"
#include <ucontext.h>

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
#ifndef CJThreadGetMutator
void* CJ_CJThreadGetMutator(void);
#endif
intptr_t MRT_StackGrow(intptr_t frameBase, uint32_t adjustedSize, void* ip);
}

namespace {

constexpr size_t kChainEndStack = 1073741824;
constexpr unsigned kInitialStack = 1024 * 1024;

enum class GrowArm { kChainEnd, kFfi };

struct GrowResult {
    bool ownerOk;
    bool reachedChainEnd;
    bool anchorReady;
    intptr_t offset;
    uintptr_t anchor;
    uintptr_t expectedAnchor;
    size_t before;
    size_t after;
    int exceptionType;
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
    Mutator* owner = MutatorManager::Instance().CreateMutator();
    g_mutator = owner;
    g_result.ownerOk = owner != nullptr && owner->GetCjthreadPtr() == static_cast<void*>(g_thread) &&
        Mutator::GetMutator() == owner && static_cast<Mutator*>(CJ_CJThreadGetMutator()) == owner &&
        ThreadLocal::GetMutator() == owner;
    if (!g_result.ownerOk) {
        return;
    }
    MRT_PreRunManagedCode(owner, 1, ThreadLocal::GetThreadLocalData());
    UnwindContext& context = owner->GetUnwindContext();
    const uintptr_t fa = reinterpret_cast<uintptr_t>(context.frameInfo.mFrame.GetFA());
    const uint32_t* ip = context.frameInfo.mFrame.GetIP();
    g_result.reachedChainEnd = fa == 0 && ip == nullptr;
    uint32_t* anchorBefore = context.anchorFA;
    g_result.anchorReady = anchorBefore != nullptr;
    if (!g_result.anchorReady || (g_arm == GrowArm::kChainEnd && !g_result.reachedChainEnd)) {
        return;
    }
    g_result.before = CJ_CJThreadStackSizeGetByCJThrd(g_thread);
    const intptr_t frameBase = g_arm == GrowArm::kFfi ? 0
        : reinterpret_cast<intptr_t>(__builtin_frame_address(0));
    const intptr_t offset = MRT_StackGrow(frameBase, 0, nullptr);
    g_result.offset = offset;
    g_result.anchor = reinterpret_cast<uintptr_t>(context.anchorFA);
    if (offset > 0) {
        g_result.expectedAnchor = reinterpret_cast<uintptr_t>(anchorBefore) + static_cast<uintptr_t>(offset);
    } else {
        g_result.expectedAnchor = reinterpret_cast<uintptr_t>(anchorBefore);
    }
    g_result.after = CJ_CJThreadStackSizeGetByCJThrd(g_thread);
    g_result.exceptionType = static_cast<int>(owner->GetExceptionWrapper().GetExceptionType());
}

void RunArm(GrowArm arm)
{
    B09RuntimeFixture runtime(8);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
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
    g_mutator = nullptr;
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
        "FIX_EXTENDED_STACK_FA0 arm=%d owner=%d chain=%d anchor_ready=%d offset=%ld before=%zu after=%zu "
        "anchor=%#zx expected=%#zx exception=%d\n",
        static_cast<int>(arm), g_result.ownerOk ? 1 : 0, g_result.reachedChainEnd ? 1 : 0,
        g_result.anchorReady ? 1 : 0, static_cast<long>(g_result.offset), g_result.before, g_result.after,
        g_result.anchor, g_result.expectedAnchor, g_result.exceptionType);
    GC_EXPECT_TRUE(g_result.ownerOk);
    GC_EXPECT_TRUE(g_result.anchorReady);
    if (arm == GrowArm::kChainEnd) {
        GC_EXPECT_TRUE(g_result.reachedChainEnd);
    }
    if (g_result.offset > 0) {
        GC_EXPECT_TRUE(g_result.before < kChainEndStack);
        GC_EXPECT_TRUE(g_result.after >= kChainEndStack);
        GC_EXPECT_TRUE(g_result.after > g_result.before);
        GC_EXPECT_EQ(g_result.anchor, g_result.expectedAnchor);
        GC_EXPECT_TRUE(g_result.exceptionType != static_cast<int>(ExceptionWrapper::ExceptionType::STACK_OVERFLOW));
    } else {
        GC_EXPECT_EQ(g_result.offset, static_cast<intptr_t>(0));
        GC_EXPECT_EQ(g_result.after, g_result.before);
        GC_EXPECT_EQ(g_result.anchor, g_result.expectedAnchor);
        if (arm == GrowArm::kChainEnd) {
            GC_EXPECT_EQ(g_result.exceptionType, static_cast<int>(ExceptionWrapper::ExceptionType::STACK_OVERFLOW));
        } else {
            GC_EXPECT_TRUE(g_result.exceptionType != static_cast<int>(ExceptionWrapper::ExceptionType::STACK_OVERFLOW));
        }
    }
    if (g_mutator != nullptr) {
        (void)g_mutator->EnterSaferegion(false);
    }
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
#endif
