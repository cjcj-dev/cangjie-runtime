// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "SignalManager.h"

#include <algorithm>
#include <atomic>
#include <unistd.h>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/SysCall.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/TracingCollector.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Heap/WCollector/UntagRefFieldBreadcrumb.h"
#endif
#include "LoaderManager.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Signal/SignalUtils.h"
#include "Inspector/CjHeapData.h"
#include "Heap/Collector/TaskQueue.h"
#include "securec.h"
#ifdef COV_SIGNALHANDLE
extern "C" void __gcov_dump(void);
#endif
namespace MapleRuntime {

namespace {
// AS-safe stderr write for signal-handler diagnostic path only.
void WriteSigDiag(const char* buf, size_t len)
{
    if (buf == nullptr || len == 0) {
        return;
    }
    (void)write(STDERR_FILENO, buf, len);
}

// FLOG-compatible ERROR line without logMutex: "<tid> E <msg>\n"
void LogErrorAsSafe(const char* msg)
{
    char buf[512];
    int n = sprintf_s(buf, sizeof(buf), "%d E %s\n", static_cast<int>(GetTid()), msg);
    if (n > 0) {
        WriteSigDiag(buf, static_cast<size_t>(n));
    }
}
} // namespace

void SignalManager::Init()
{
    PrepareSigStack();
    // Block some ignored signals
    BlockSignals();
#if !defined(__OHOS__) && !defined(__ANDROID__) && !defined(__IOS__)
    // Install unexpected handler first
    InstallUnexpectedSignalHandlers();
    // Install sigsegv handler
    InstallSegvHandler();
    // Install sigusr1 handler
    InstallSIGUSR1Handlers();
#endif
#ifdef __OHOS__
    // Install sigusr2 handler
    InstallSIGUSR2Handlers();
#endif
}

void SignalManager::Fini()
{
    FreeSigStack();
}

void SignalManager::PrepareSigStack()
{
    constexpr int stackSizeMultiples = 4;
    signalStack.ss_sp = malloc(SIGSTKSZ * stackSizeMultiples);
    if (signalStack.ss_sp == nullptr) {
        LOG(RTLOG_FATAL, "Alloca signal stack failed.");
    }

    signalStack.ss_size = SIGSTKSZ * stackSizeMultiples;
    signalStack.ss_flags = 0;

    if (sigaltstack(&signalStack, nullptr) == -1) {
        LOG(RTLOG_FATAL, "sigaltstack failed.");
    }
}

void SignalManager::FreeSigStack()
{
    free(signalStack.ss_sp);
}

void SignalManager::BlockSignals()
{
    sigset_t set;
    CHECK_SIGNAL_CALL(sigemptyset, (&set), "sigemptyset failed in BlockSignals");
    CHECK_SIGNAL_CALL(sigaddset, (&set, SIGPIPE), "sigaddset failed in BlockSignals");
    CHECK_SIGNAL_CALL(pthread_sigmask, (SIG_BLOCK, &set, nullptr), "pthread_sigmask failed in BlockSignals");
}

static void CheckStackOverflow(const siginfo_t& info)
{
    if (Runtime::CurrentRef() != nullptr && !Runtime::Current().GetConcurrencyModel().GetStackGuardCheckFlag()) {
        return;
    }
    uintptr_t stackAddr = reinterpret_cast<uintptr_t>(CJThreadStackAddrGet());
    uintptr_t topAddr = stackAddr - MapleRuntime::MRT_PAGE_SIZE;
    uintptr_t sigAddr = reinterpret_cast<uintptr_t>(info.si_addr);
    if (stackAddr != 0 && sigAddr >= topAddr && sigAddr < stackAddr) {
        LogErrorAsSafe("unhandled SIGSEGV from unmanaged stack overflow!");
    }
}

static void CheckSuspendState()
{
    ThreadLocalData* tlData = ThreadLocal::GetThreadLocalData();
    Mutator* mutator = tlData->mutator;
    if (mutator == nullptr) {
        return;
    }
    if (mutator->HasSuspensionRequest(Mutator::SuspensionType::SUSPENSION_FOR_EXIT)) {
        while (true) {
            sleep(INT_MAX);
        }
    }
}

void PrintSignalHandlerStack(int sig, const siginfo_t* info, void* context)
{
    // AS-safe path: key fields (tid/si_addr/pc/fa) via stack buffer + write(2).
    // Full unwind / symbolize / FLOG / pthread_getname_np are deferred out of the
    // signal-context critical path (REPORT-gchang11 §5 D).
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
    PrintUntagRefFieldBreadcrumb();
#endif
    // Emit once per OS signal delivery (HandlerImpl entry) so a user-registered
    // crash handler on an _exit path cannot suppress the pc line.
    uintptr_t sigPc = 0;
    uintptr_t sigFa = 0;
    if (context != nullptr) {
        ucontext_t* ucontext = static_cast<ucontext_t*>(context);
        sigPc = GetPCFromUContext(*ucontext);
        sigFa = GetFAFromUContext(*ucontext);
    }
    const void* siAddr = (info != nullptr) ? info->si_addr : nullptr;

    char line[320];
    int n = sprintf_s(line, sizeof(line),
                      "%d E signal %s (%d) pc=0x%lx fa=0x%lx si_addr=%p\n",
                      static_cast<int>(GetTid()), SignalManager::GetSignalName(static_cast<uint8_t>(sig)), sig,
                      static_cast<unsigned long>(sigPc), static_cast<unsigned long>(sigFa), siAddr);
    if (n > 0) {
        WriteSigDiag(line, static_cast<size_t>(n));
    }
    // PrintSignalStackTrace degraded: only pc/fa hex already emitted above (no unwind/heap).
}

bool SignalManager::HandleUnexpectedSignal(int sig, siginfo_t* info, void* context)
{
    CheckSuspendState();
    // pc/fa/si_addr already emitted at HandlerImpl entry (before user handlers).
#ifdef COV_SIGNALHANDLE
    __gcov_dump();
#endif

    return false;
}

void SignalManager::InstallUnexpectedSignalHandlers()
{
    sigset_t mask;
    CHECK_SIGNAL_CALL(sigemptyset, (&mask), "sigemptyset failed");
    SignalAction sa;
    sa.saSignalAction= HandleUnexpectedSignal;
    sa.scMask = mask;
    sa.scFlags = SA_SIGINFO | SA_ONSTACK;

    AddHandlerToSignalStack(SIGABRT, &sa);
#ifdef __APPLE__
    AddHandlerToSignalStack(SIGSEGV, &sa);
#else
    AddHandlerToSignalStack(SIGBUS, &sa);
#endif
    AddHandlerToSignalStack(SIGILL, &sa);
    AddHandlerToSignalStack(SIGFPE, &sa);
}

void SignalManager::InstallSIGUSR1Handlers() const
{
    sigset_t mask;
    CHECK_SIGNAL_CALL(sigemptyset, (&mask), "sigemptyset failed");
    SignalAction sa;
    sa.saSignalAction= HandleUnexpectedSIGUSR1;
    sa.scMask = mask;
    sa.scFlags = SA_SIGINFO | SA_ONSTACK;
    AddHandlerToSignalStack(SIGUSR1, &sa);
}

#ifdef __OHOS__
void SignalManager::InstallSIGUSR2Handlers() const
{
    sigset_t mask;
    CHECK_SIGNAL_CALL(sigemptyset, (&mask), "sigemptyset failed");
    SignalAction sa;
    sa.saSignalAction= HandleUnexpectedSIGUSR2;
    sa.scMask = mask;
    sa.scFlags = SA_SIGINFO | SA_ONSTACK;
    AddHandlerToSignalStack(SIGUSR2, &sa);
}

struct ProfDumpNode {
    int (*func)(void);
    ProfDumpNode *next;
};

std::atomic<ProfDumpNode*> profileDumpList {nullptr};

extern "C" void RegisterProfileDumpFunction(int (*func)(void))
{
    if (func == nullptr) {
        return;
    }

    // Check if func is already registered
    ProfDumpNode *current = profileDumpList.load(std::memory_order_relaxed);
    while (current != nullptr) {
        if (current->func == func) {
            return;
        }
        current = current->next;
    }

    // Not found, allocate and add
    ProfDumpNode *node = reinterpret_cast<ProfDumpNode*>(malloc(sizeof(ProfDumpNode)));
    if (node == nullptr) {
        LOG(RTLOG_FATAL, "Failed to allocate for ProfDumpNode");
        return;
    }

    node->func = func;
    ProfDumpNode *old = profileDumpList.load(std::memory_order_relaxed);
    do {
        node->next = old;
    } while (!profileDumpList.compare_exchange_weak(old, node, std::memory_order_relaxed));

    return;
}


extern "C" MRT_EXPORT
    void CJ_MRT_RegisterProfDumpFunc(int (*func)(void)) __attribute__((alias("RegisterProfileDumpFunction")));

bool SignalManager::HandleUnexpectedSIGUSR2(int sig, siginfo_t* info, void* context)
{
    ProfDumpNode *current = profileDumpList.load(std::memory_order_relaxed);
    if (current == nullptr) {
        LOG(RTLOG_INFO, "[CJ]: No Profile Dump Registered.");
        return true;
    }
    LOG(RTLOG_INFO, "[CJ]: Inst Profile Dump Start.");

    while (current != nullptr) {
        if (current->func != nullptr) {
            current->func();
        }
        current = current->next;
    }

    LOG(RTLOG_INFO, "[CJ]: Inst Profile Dump Finished.");
    return true;
}
#endif

bool SignalManager::HandleUnexpectedSIGUSR1(int sig, siginfo_t* info, void* context)
{
    Heap::GetHeap().GetCollectorResources().RequestHeapDump(GCTask::TaskType::TASK_TYPE_DUMP_HEAP);
    return true;
}

// Handle unexpected SIGSEGV
bool SignalManager::HandleUnexpectedSigsegv(int sig, siginfo_t* info, void* context)
{
    CheckSuspendState();
    // Do more functional things here.
    if (info != nullptr) {
        CheckStackOverflow(*info);
    }
    // pc/fa/si_addr already emitted at HandlerImpl entry (before user handlers).
    return false;
}

void SignalManager::InstallSegvHandler()
{
    sigset_t mask;
    // Allow some signals to be triggered when handling SIGSEGV
    CHECK_SIGNAL_CALL(sigfillset, (&mask), "sigfillset failed in InstallSegvHandler");
    CHECK_SIGNAL_CALL(sigdelset, (&mask, SIGABRT), "sigdelset SIGABRT failed in InstallSegvHandler");
    CHECK_SIGNAL_CALL(sigdelset, (&mask, SIGBUS), "sigdelset SIGBUS failed in InstallSegvHandler");
    CHECK_SIGNAL_CALL(sigdelset, (&mask, SIGFPE), "sigdelset SIGFPE failed in InstallSegvHandler");
    CHECK_SIGNAL_CALL(sigdelset, (&mask, SIGILL), "sigdelset SIGILL failed in InstallSegvHandler");
    CHECK_SIGNAL_CALL(sigdelset, (&mask, SIGSEGV), "sigdelset SIGSEGV failed in InstallSegvHandler");

    if (Runtime::Current().GetConcurrencyModel().GetStackGuardCheckFlag()) {
        // Alloc extra one page memory to handle stack overflow
        constexpr uint8_t minPageCount = 16;
        extraStackSize = std::max(AlignUp<uint32_t>(MINSIGSTKSZ, MapleRuntime::MRT_PAGE_SIZE),
                                  static_cast<uint32_t>(minPageCount * MapleRuntime::MRT_PAGE_SIZE));
        extraStack = PagePool::Instance().GetPage(extraStackSize);
        stack_t ss{};
        ss.ss_sp = extraStack;
        ss.ss_size = extraStackSize;
        CHECK_SIGNAL_CALL(sigaltstack, (&ss, nullptr), "sigaltstack failed in InstallSegvHandler");
    }

    CHECK_SIGNAL_CALL(sigemptyset, (&mask), "sigemptyset failed");
    SignalAction unexcept;
    unexcept.saSignalAction= HandleUnexpectedSigsegv;
    unexcept.scMask = mask;
    unexcept.scFlags = SA_RESTART | SA_SIGINFO | SA_ONSTACK;
#ifdef __APPLE__
    AddHandlerToSignalStack(SIGBUS, &unexcept);
#else
    AddHandlerToSignalStack(SIGSEGV, &unexcept);
#endif
}

void SignalManager::AddHandlerToSignalStack(int signal, SignalAction* sa)
{
    SignalStack::InitializeSignalStack();

    if (signal <= 0 || signal >= _NSIG) {
        LOG(RTLOG_FATAL, "Invalid signal %d", signal);
    }

    SignalStack::GetStacks()[signal].AddHandler(sa);
    SignalStack::GetStacks()[signal].MarkSig(signal);
}

void SignalManager::RemoveHandlerFromSignalStack(int signal, bool (*fn)(int, siginfo_t*, void*))
{
    SignalStack::InitializeSignalStack();

    if (signal <= 0 || signal >= _NSIG) {
        LOG(RTLOG_FATAL, "Invalid signal %d", signal);
    }

    SignalStack::GetStacks()[signal].RemoveHandler(fn);
}

} // namespace MapleRuntime
