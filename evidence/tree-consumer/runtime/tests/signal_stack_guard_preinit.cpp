// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// HotSpot counterpart: runtime/StackGuardPages/TestStackGuardPages.java and
// exeinvoke.c: install a signal handler and verify native/VM lifecycle states.
// Query cases call the product guard query before TLS initialization.
// Fault cases separately exercise SignalStack hard-fault termination; that path
// terminates before the registered callback and does not test the guard query.
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Common/Runtime.h"
#include "SignalManager.h"

class PublishedRuntime final : public MapleRuntime::Runtime {
public:
    PublishedRuntime() { runtime = this; }
    ~PublishedRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return {}; }
    void SetGCThreshold(uint64_t) override {}
};

int main(int argc, char** argv)
{
    if (argc != 2 || (strcmp(argv[1], "no-runtime") != 0 && strcmp(argv[1], "published-runtime") != 0 &&
                      strcmp(argv[1], "query-no-runtime") != 0 && strcmp(argv[1], "query-published-runtime") != 0)) {
        return 2;
    }
    if (strncmp(argv[1], "query-", 6) == 0) {
        using GuardQuery = bool (*)(const void*);
        auto query = reinterpret_cast<GuardQuery>(dlsym(RTLD_DEFAULT, "CJ_CJThreadIsStackGuardAddress"));
        if (query == nullptr) {
            fprintf(stderr, "missing product guard query: %s\n", dlerror());
            return 3;
        }
        if (strcmp(argv[1], "query-published-runtime") == 0) {
            (void)new PublishedRuntime();
        }
        // Native TLS is still unregistered. A stack address cannot belong to a
        // managed guard zone; reading that result must not call an unset hook.
        int nativeLocal = 0;
        bool inGuard = query(&nativeLocal);
        fprintf(stderr, "GUARD_QUERY_RESULT case=%s in_guard=%d expected=0\n", argv[1], inGuard);
        return inGuard ? 1 : 0;
    }
    using Handler = bool (*)(int, siginfo_t*, void*);
    // Use the existing product callback without adding a test-only export or
    // recompiling any product source into this executable.
    auto handler = reinterpret_cast<Handler>(dlsym(RTLD_DEFAULT,
        "_ZN12MapleRuntime13SignalManager23HandleUnexpectedSigsegvEiP9siginfo_tPv"));
    if (handler == nullptr) {
        fprintf(stderr, "missing product callback: %s\n", dlerror());
        return 3;
    }
    pid_t child = fork();
    if (child < 0) {
        return 4;
    }
    if (child == 0) {
        if (strcmp(argv[1], "published-runtime") == 0) {
            // Deliberately leave concurrencyModel null, as during initialization.
            (void)new PublishedRuntime();
        }
        SignalAction action{};
        action.saSignalAction = handler;
        sigemptyset(&action.scMask);
        action.scFlags = SA_SIGINFO;
        MapleRuntime::SignalManager::AddHandlerToSignalStack(SIGSEGV, &action);
        void* page = mmap(nullptr, static_cast<size_t>(sysconf(_SC_PAGESIZE)),
                          PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            _exit(5);
        }
        fprintf(stderr, "FAULT_ARMED case=%s address=%p\n", argv[1], page);
        fflush(stderr);
        *static_cast<volatile unsigned char*>(page) = 1;
        _exit(6);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        return 7;
    }
    bool originalSignal = WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    fprintf(stderr, "SIGNAL_RESULT case=%s status=%d signal=%d expected=%d\n", argv[1], status,
            WIFSIGNALED(status) ? WTERMSIG(status) : 0, SIGSEGV);
    return originalSignal ? 0 : 1;
}
