// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "SignalStack.h"
#include "SignalManager.h"
#include "Cangjie.h"

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <csignal>
#include <cstdlib>
#include <climits>
#include <ucontext.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <type_traits>
#include <utility>
#include "Cangjie.h"
#include "Base/SysCall.h"
#include "securec.h"
#include "Common/ScopedObjectAccess.h"
#include "CJThread/src/syscall/include/inner/syscall_impl.h"
#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <semaphore.h>
#endif
#ifdef COV_SIGNALHANDLE
extern "C" void __gcov_dump(void);
#endif
namespace MapleRuntime {
SignalStack SignalStack::stacks[_NSIG];

static decltype(&sigaction) g_linkedSignalAction;
static decltype(&sigprocmask) g_linkedSignalProcmask;
// HotSpot signals_posix.cpp:350-377: one counter per signal and a semaphore.
static std::atomic<int> g_pendingSignals[_NSIG + 1]{};
static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal counters must be lock-free");
#ifdef __APPLE__
static semaphore_t g_signalSemaphore;
#else
static sem_t g_signalSemaphore;
#endif
static CJThreadHandle g_signalDispatcher = nullptr;
static std::mutex g_handlerMutex;

static void NotifySignal(int signal)
{
    g_pendingSignals[signal].fetch_add(1, std::memory_order_relaxed);
#ifdef __APPLE__
    semaphore_signal(g_signalSemaphore);
#else
    sem_post(&g_signalSemaphore);
#endif
}

static int WaitForSignal()
{
    for (;;) {
        for (int signal = 0; signal <= _NSIG; ++signal) {
            int pending = g_pendingSignals[signal].load(std::memory_order_relaxed);
            if (pending > 0 && g_pendingSignals[signal].compare_exchange_strong(
                pending, pending - 1, std::memory_order_relaxed)) {
                return signal;
            }
        }
        // semaphore.inline.hpp:33-41: only the dispatcher may transition.
        ScopedEnterSaferegion blocked(false);
        SyscallEnter();
#ifdef __APPLE__
        while (semaphore_wait(g_signalSemaphore) == KERN_ABORTED) {}
#else
        while (sem_wait(&g_signalSemaphore) != 0 && errno == EINTR) {}
#endif
        SyscallExit();
    }
}


// AS-safe helpers for the signal handler path (POSIX async-signal-safe only).
namespace {
void WriteAsSafe(const char* buf, size_t len)
{
    if (buf == nullptr || len == 0) {
        return;
    }
    (void)write(STDERR_FILENO, buf, len);
}

void WriteAsSafeCStr(const char* str)
{
    if (str == nullptr) {
        return;
    }
    size_t len = 0;
    while (str[len] != '\0') {
        ++len;
    }
    WriteAsSafe(str, len);
}

// Match FLOG(RTLOG_ERROR, "CJNative Handle signal: %d.") byte sequence exactly:
// "<tid> E CJNative Handle signal: <n>.\n"
void LogHandleSignalAsSafe(int signal)
{
    char buf[96];
    int n = sprintf_s(buf, sizeof(buf), "%d E CJNative Handle signal: %d.\n",
                      static_cast<int>(GetTid()), signal);
    if (n > 0) {
        WriteAsSafe(buf, static_cast<size_t>(n));
    }
}

void RaiseDefaultAsSafe(int signal)
{
    struct sigaction dfl = {};
    dfl.sa_handler = SIG_DFL;
    if (g_linkedSignalAction != nullptr) {
        g_linkedSignalAction(signal, &dfl, nullptr);
    }
    raise(signal);
}
} // namespace

void SignalStack::AddHandler(SignalAction* sa)
{
    std::lock_guard<std::mutex> lock(g_handlerMutex);
    handlerStack.push_back(*sa);
}

void SignalStack::RemoveHandler(bool (*fn)(int, siginfo_t*, void*))
{
    std::lock_guard<std::mutex> lock(g_handlerMutex);
    for (std::vector<SignalAction>::iterator it = handlerStack.begin(); it != handlerStack.end(); it++) {
        if ((*it).saSignalAction == fn) {
            handlerStack.erase(it);
            break;
        }
    }
}

// HotSpot signals_posix.cpp:404-447. Native chained handlers run on the
// interrupted thread, with their own mask, before the unhandled-fatal branch.
static bool CallChainedHandler(int signal, siginfo_t* info, void* context)
{
    auto& action = SignalStack::GetStacks()[signal].sigAction;
    if (action.sa_handler == SIG_DFL) { return false; }
    if (action.sa_handler == SIG_IGN) { return true; }
    const auto handler = action.sa_handler;
    const auto sigactionHandler = action.sa_sigaction;
    const int flags = action.sa_flags;
    sigset_t mask = action.sa_mask;
    if (!(flags & SA_NODEFER)) { sigaddset(&mask, signal); }
    if (flags & SA_RESETHAND) { action.sa_handler = SIG_DFL; }
    sigset_t previous;
    g_linkedSignalProcmask(SIG_SETMASK, &mask, &previous);
    if (flags & SA_SIGINFO) { sigactionHandler(signal, info, context); }
    else { handler(signal); }
    g_linkedSignalProcmask(SIG_SETMASK, &previous, nullptr);
    return true;
}

void PrintSignalHandlerStack(int sig, const siginfo_t* info, void* context);

void SignalStack::Handler(int signal, siginfo_t* siginfo, void* context)
{
    const int savedErrno = errno;
    // HotSpot signals_posix.cpp:609-613: only these categories chain first.
    bool handled = false;
    if (signal == SIGPIPE || signal == SIGXFSZ) {
        CallChainedHandler(signal, siginfo, context);
        handled = true;
    }
    // signals_posix.cpp:637-641 / os.cpp:371-380: platform/notification
    // handling precedes the fallback native chain for all other signals.
    if (!handled) {
        switch (signal) {
            case SIGSEGV:
            case SIGBUS:
            case SIGFPE:
            case SIGILL:
            case SIGABRT:
            case SIGTRAP:
                handled = SignalManager::HandlePlatformSignal(signal, siginfo, context);
                break;
            default:
                NotifySignal(signal);
                handled = true;
                break;
        }
    }
    if (!handled) {
        handled = CallChainedHandler(signal, siginfo, context);
    }
    if (!handled) {
        // signals_posix.cpp:650-655 VMError::report_and_die. Symbolization
        // (dladdr/sprintf in PrintSignalHandlerStack) belongs here, not in
        // the platform step at signals_posix.cpp:637-641.
        PrintSignalHandlerStack(signal, siginfo, context);
        LogHandleSignalAsSafe(signal);
        RaiseDefaultAsSafe(signal);
    }
    errno = savedErrno;
}

void SignalStack::HandlerImpl(int signal)
{
    // The managed dispatcher owns execution. User signal delivery carries a
    // signal number, not an interrupted stack (os.cpp:signal_thread_entry).
    std::vector<SignalAction> handlers;
    {
        std::lock_guard<std::mutex> lock(g_handlerMutex);
        handlers = stacks[signal].handlerStack;
    }
    for (auto it = handlers.rbegin(); it != handlers.rend(); ++it) {
        if (it->saSignalAction == nullptr) { break; }
        sigset_t previous;
        g_linkedSignalProcmask(SIG_SETMASK, &it->scMask, &previous);
        bool handled = it->saSignalAction(signal, nullptr, nullptr);
        g_linkedSignalProcmask(SIG_SETMASK, &previous, nullptr);
        if (handled) { return; }
    }
    RaiseDefaultAsSafe(signal);
}

void* SignalStack::DispatchSignals(void*)
{
    // os.cpp:371-380: a dedicated managed task, including an exit signal.
    for (;;) {
        int signal = WaitForSignal();
        if (signal == _NSIG) { return nullptr; }
        HandlerImpl(signal);
    }
}

void SignalStack::StartDispatcher()
{
    g_signalDispatcher = RunCJTask(DispatchSignals, nullptr);
    CHECK(g_signalDispatcher != nullptr);
}

void SignalStack::StopDispatcher()
{
    if (g_signalDispatcher == nullptr) { return; }
    NotifySignal(_NSIG);
    void* result = nullptr;
    GetTaskRet(g_signalDispatcher, &result);
    ReleaseHandle(g_signalDispatcher);
    g_signalDispatcher = nullptr;
}

template <typename T>
static void FindSymbolInLibc(T* result, const char* name)
{
#if defined(__OHOS__) || defined(__ANDROID__)
    constexpr const char* libName = "libc.so";
#elif defined(__APPLE__)
    constexpr const char* libName = "libc.dylib";
#else
    constexpr const char* libName = "libc.so.6";
#endif
    static void* libc = []() {
        void* res = dlopen(libName, RTLD_LOCAL | RTLD_LAZY);
        if (!res) {
            LOG(RTLOG_FATAL, "failed to dlopen %s: %s", libName, dlerror());
        }
        return res;
    }();

    void* sym = dlsym(libc, name);
    if (sym == nullptr) {
        LOG(RTLOG_FATAL, "failed to find %s in %s", name, libName);
    }
    *result = reinterpret_cast<T>(sym);
}

__attribute__((constructor)) void SignalStack::InitializeSignalStack()
{
    static std::once_flag once;
    std::call_once(once, []() {
#ifdef __APPLE__
        CHECK(semaphore_create(mach_task_self(), &g_signalSemaphore, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS);
#else
        CHECK(sem_init(&g_signalSemaphore, 0, 0) == 0);
#endif
        FindSymbolInLibc(&g_linkedSignalAction, "sigaction");
        FindSymbolInLibc(&g_linkedSignalProcmask, "sigprocmask");
    });
}

void SignalStack::Register(int signal)
{
    struct sigaction handlerAction = {};
    sigfillset(&handlerAction.sa_mask);

    handlerAction.sa_sigaction = SignalStack::Handler;
    handlerAction.sa_flags = SA_RESTART | SA_SIGINFO | SA_ONSTACK;

    // Change the current signal behavior and store the old behavior into `sigAction`.
    g_linkedSignalAction(signal, &handlerAction, &sigAction);
    // Do not modify the current behavior; only query and confirm the current behavior.
    g_linkedSignalAction(signal, nullptr, &handlerAction);
}

struct sigaction SignalStack::GetAction()
{
    return sigAction;
}

void SignalStack::SetAction(const struct sigaction* newAction)
{
    sigAction = *newAction;
}

static int SigactionImpl(int signal, const struct sigaction* newAction, struct sigaction* oldAction,
                         int (*linked)(int, const struct sigaction*, struct sigaction*))
{
    if (signal <= 0 || signal >= _NSIG) {
        errno = EINVAL;
        return -1;
    }

    if (SignalStack::GetStacks()[signal].IsMarked()) {
        struct sigaction tmpAction = SignalStack::GetStacks()[signal].GetAction();
        if (newAction != nullptr) {
            SignalStack::GetStacks()[signal].SetAction(newAction);
        }
        if (oldAction != nullptr) {
            *oldAction = tmpAction;
        }
        return 0;
    }

    return linked(signal, newAction, oldAction);
}

// since the -fvisibility=hidden property causes this symbol defaults to GLOBAL+HIDDEN
// this will cause the symbol to become LOCAL+HIDDEN when linked to libcangjie-runtime.so
// specify visibility for symbols that need to be exported
extern "C" MRT_EXPORT int sigaction(int signal, const struct sigaction* newAction, struct sigaction* oldAction)
{
    SignalStack::InitializeSignalStack();
    return SigactionImpl(signal, newAction, oldAction, g_linkedSignalAction);
}

extern "C" MRT_EXPORT sighandler_t signal(int signal, sighandler_t handler)
{
    SignalStack::InitializeSignalStack();

    if (signal <= 0 || signal >= _NSIG) {
        errno = EINVAL;
        return SIG_ERR;
    }

    struct sigaction sa = {};
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    if (SignalStack::GetStacks()[signal].IsMarked()) {
        sighandler_t oldhandler =
            reinterpret_cast<sighandler_t>(SignalStack::GetStacks()[signal].GetAction().sa_handler);
        SignalStack::GetStacks()[signal].SetAction(&sa);
        return oldhandler;
    }

    if (g_linkedSignalAction(signal, &sa, &sa) == -1) {
        return SIG_ERR;
    }

    return reinterpret_cast<sighandler_t>(sa.sa_handler);
}

void AdjustSignalMask(sigset_t* set)
{
    for (int i = 1; i < _NSIG; ++i) {
        if (SignalStack::GetStacks()[i].IsMarked() && sigismember(set, i)) {
            sigdelset(set, i);
        }
    }
}

int SigProcMask(int how, const sigset_t* newSet, sigset_t* oldSet,
                int (*linked)(int, const sigset_t*, sigset_t*))
{
    if (newSet == nullptr) {
        return linked(how, newSet, oldSet);
    }

    sigset_t tmpset = *newSet;

    if (how == SIG_BLOCK || how == SIG_SETMASK) {
        AdjustSignalMask(&tmpset);
    }

    return linked(how, &tmpset, oldSet);
}

extern "C" MRT_EXPORT int sigprocmask(int how, const sigset_t* newSet, sigset_t* oldSet)
{
    SignalStack::InitializeSignalStack();
    return SigProcMask(how, newSet, oldSet, g_linkedSignalProcmask);
}

} // namespace MapleRuntime
