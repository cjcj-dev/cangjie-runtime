// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Real runtime/task/GC entrance for signals_posix.cpp:642-654 regression.
#include "Cangjie.h"
#include "Heap/z/zHeap.hpp"
#include "SignalManager.h"
#include "Base/Log.h"
#include "ObjectModel/MObject.h"
#include "ExceptionManager.h"
#include "CJThread/src/runtime/schedule/include/schedule.h"
#include <cerrno>
#include <sys/syscall.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

extern "C" void CJ_MCC_AddSignalHandler(int, SignalAction*);
extern "C" void* CJ_CJThreadStackAddrGet(void);
extern "C" bool CJ_CJThreadIsStackGuardAddress(const void*);
static std::atomic<int> delivered{0};
static std::atomic<int> deliveryCount{0};
static std::atomic<long> deliveryThread{0};
static volatile sig_atomic_t nativeResult = 0;
static void NativeHandler(int sig)
{
    sigset_t mask;
    sigprocmask(SIG_SETMASK, nullptr, &mask);
    nativeResult = sigismember(&mask, SIGUSR2) && sigismember(&mask, sig) ? sig : -1;
}
static void NativeInfoHandler(int sig, siginfo_t* info, void* context)
{
    NativeHandler(sig);
    if (info == nullptr || context == nullptr) { nativeResult = -2; }
}

// The debugger calls a real Logger FATAL producer while the selected product
// frame owns its watermark mutex. No synthetic remembered message or signal.
extern "C" __attribute__((noinline)) void SignalFatal()
{
    LOG(RTLOG_FATAL, "SIGNAL_WATERMARK_REAL_FATAL_1253");
}

static bool RecordSignal(int sig, siginfo_t*, void*)
{
    deliveryThread.store(syscall(SYS_gettid), std::memory_order_relaxed);
    delivered.store(sig, std::memory_order_release);
    deliveryCount.fetch_add(1, std::memory_order_release);
    return true;
}

extern "C" __attribute__((noinline)) void* SignalWatermarkWork(void*)
{
    MapleRuntime::Heap::GetHeap().RequestGC(MapleRuntime::GC_REASON_USER);
    return nullptr;
}

namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize);
extern "C" void MCC_ThrowException(ExceptionRef);
}
static MapleRuntime::ExceptionRef pendingException;
extern "C" __attribute__((noinline)) void SignalThrowAgain()
{
    MapleRuntime::MCC_ThrowException(pendingException);
}
static void* SignalExceptionWork(void*)
{
    using namespace MapleRuntime;
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(64);
    pendingException = MCC_NewObject(type, 64 + TYPEINFO_PTR_SIZE);
    SignalThrowAgain();
    return nullptr;
}
static void* SignalGuardWork(void*)
{
    // Touch the actual guard page allocated for this runtime task. No fake
    // siginfo, watermark, mutator state or direct handler invocation.
    auto* guard = static_cast<volatile char*>(CJ_CJThreadStackAddrGet()) - 1;
    std::fprintf(stderr, "SIGNAL_GUARD_INPUT address=%p in_guard=%d\n",
                 const_cast<char*>(guard), CJ_CJThreadIsStackGuardAddress(const_cast<char*>(guard)));
    *guard = 1;
    return nullptr;
}

int main(int argc, char** argv)
{
    if (argc != 3) { return 2; }
    const int sig = std::atoi(argv[2]);
    RuntimeParam param{};
    param.heapParam.heapSize = 64 * 1024;
    param.coParam.processorNum = 1;
    if (InitCJRuntime(&param) != E_OK) { return 3; }
    SignalAction action{};
    action.saSignalAction = RecordSignal;
    action.scFlags = SA_SIGINFO;
    sigemptyset(&action.scMask);
    CJ_MCC_AddSignalHandler(sig, &action);
    std::fprintf(stderr, "SIGNAL_INPUT registered=%d mode=%s\n", sig, argv[1]);
    const bool notificationFirst = sig == SIGUSR1;
    auto waitForNotification = [&]() {
        if (!notificationFirst) { return; }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (delivered.load(std::memory_order_acquire) != sig && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
    };
    // SIGPIPE is blocked by runtime initialization; exercise its explicit
    // HotSpot category with the same real OS entrance as the other cases.
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, sig);
    pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
    if (std::strncmp(argv[1], "native", 6) == 0) {
        struct sigaction native{};
        sigemptyset(&native.sa_mask);
        sigaddset(&native.sa_mask, SIGUSR2);
        native.sa_flags = SA_RESETHAND;
        if (std::strcmp(argv[1], "native-info") == 0) {
            native.sa_flags |= SA_SIGINFO;
            native.sa_sigaction = NativeInfoHandler;
        } else {
            native.sa_handler = NativeHandler;
        }
        if (sigaction(sig, &native, nullptr) != 0) { return 5; }
        std::raise(sig);
        struct sigaction after{};
        sigaction(sig, nullptr, &after);
        waitForNotification();
        bool ok = notificationFirst
            ? nativeResult == 0 && after.sa_handler != SIG_DFL && delivered.load() == sig
            : nativeResult == sig && after.sa_handler == SIG_DFL && delivered.load() == 0;
        std::fprintf(stderr, "SIGNAL_NATIVE_TARGET executed=1 result=%d reset=%d managed=%d\n",
                     nativeResult, after.sa_handler == SIG_DFL, delivered.load());
        std::_Exit(ok ? 0 : 1);
    } else if (std::strcmp(argv[1], "ignored") == 0) {
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        sigaction(sig, &ignore, nullptr);
        std::raise(sig);
        waitForNotification();
        std::fprintf(stderr, "SIGNAL_IGNORE_TARGET executed=1 managed=%d\n", delivered.load());
        std::_Exit(delivered.load() == (notificationFirst ? sig : 0) ? 0 : 1);
    } else if (std::strcmp(argv[1], "guard") == 0 || std::strcmp(argv[1], "exception") == 0) {
        auto task = RunCJTask(std::strcmp(argv[1], "guard") == 0 ? SignalGuardWork : SignalExceptionWork, nullptr);
        void* result = nullptr;
        if (task == nullptr || GetTaskRet(task, &result) != E_OK) { return 4; }
        return 1;
    } else if (std::strcmp(argv[1], "watermark") == 0) {
        auto task = RunCJTask(SignalWatermarkWork, nullptr);
        void* result = nullptr;
        if (task == nullptr || GetTaskRet(task, &result) != E_OK) { return 4; }
        ReleaseHandle(task);
    } else if (std::strcmp(argv[1], "unhandled") == 0) {
        std::raise(sig);
        return 1;
    } else {
        int expected = std::strcmp(argv[1], "burst") == 0 ? 8 : 1;
        for (int i = 0; i < expected; ++i) { std::raise(sig); }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (deliveryCount.load(std::memory_order_acquire) != expected && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const int observed = delivered.load(std::memory_order_acquire);
        std::fprintf(stderr, "SIGNAL_NORMAL_TARGET executed=1 signal=%d observed=%d\n", sig, observed);
        int count = deliveryCount.load(std::memory_order_acquire);
        std::fprintf(stderr, "SIGNAL_PENDING_TARGET executed=1 expected=%d observed=%d\n", expected, count);
        bool separate = deliveryThread.load() != syscall(SYS_gettid);
        int fini = FiniCJRuntime();
        std::fprintf(stderr, "SIGNAL_DISPATCH_TARGET separate=%d fini=%d\n", separate, fini);
        std::_Exit(observed == sig && count == expected && separate && fini == E_OK ? 0 : 1);
    }
    std::_Exit(0);
}
