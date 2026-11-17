// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Deliberately uses only the common public C API. No CJ image/V1 registration.
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <unistd.h>
#include "Cangjie.h"

static void Event(const char* name, int result)
{
    timespec now {};
    clock_gettime(CLOCK_REALTIME, &now);
    std::printf("EXIT_EVENT pid=%d realtime=%lld.%09ld name=%s result=%d\n",
                static_cast<int>(getpid()), static_cast<long long>(now.tv_sec), now.tv_nsec, name, result);
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    if (argc != 2 || (std::strcmp(argv[1], "return") != 0 && std::strcmp(argv[1], "fini") != 0)) {
        return 2;
    }
    Event("start", 0);
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&InitCJRuntime), &info) == 0 || info.dli_fname == nullptr) {
        Event("loaded-runtime", 1);
        return 3;
    }
    std::printf("EXIT_RUNTIME pid=%d path=%s\n", static_cast<int>(getpid()), info.dli_fname);
    std::fflush(stdout);
    RuntimeParam parameters {};
    const auto init = InitCJRuntime(&parameters);
    Event("init", static_cast<int>(init));
    if (init != E_OK) {
        return 4;
    }
    if (std::strcmp(argv[1], "fini") == 0) {
        const auto fini = FiniCJRuntime();
        Event("fini", static_cast<int>(fini));
        if (fini != E_OK) {
            return 5;
        }
    }
    Event("return", 0);
    return 0;
}
