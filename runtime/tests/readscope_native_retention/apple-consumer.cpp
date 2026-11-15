// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Cangjie.h"
static void Check(const char* name, bool pass)
{
    std::printf("TARGET %s executed=1 pass=%d\n", name, pass);
    std::fflush(stdout);
    if (!pass) { std::exit(1); }
}
int main(int argc, char** argv)
{
    if (argc != 3) { return 2; }
    void* first = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
    if (!first) { std::fprintf(stderr, "%s\n", dlerror()); }
    Check("apple-native-load", first != nullptr);
    using Counter = unsigned (*)();
    auto init = reinterpret_cast<Counter>(dlsym(first, "ReadScopeInitCount"));
    auto fini = reinterpret_cast<Counter>(dlsym(first, "ReadScopeFiniCount"));
    auto value = reinterpret_cast<int (*)()>(dlsym(first, "ReadScopeValue"));
    Check("apple-constructor-exactly-once", init && fini && value && init() == 1 && fini() == 0);
    Check("apple-public-load", LoadCJLibrary(argv[1]) == E_OK);
    Check("apple-public-native-identity", FindCJSymbol(argv[1], "ReadScopeValue") == reinterpret_cast<void*>(value));
    Check("apple-unknown-close-rejected", UnloadCJLibrary("readscope-no-such-library.dylib") != 0);
    Check("apple-logical-close", UnloadCJLibrary(argv[1]) == 0);
    Check("apple-external-native-close", dlclose(first) == 0);
    Check("apple-no-runtime-fini", init() == 1 && fini() == 0 && value() == 37);
    void* reopened = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL | RTLD_FIRST | RTLD_NOLOAD);
    Check("apple-reopen-same-identity", reopened == first && dlsym(reopened, "ReadScopeValue") == reinterpret_cast<void*>(value));
    Check("apple-reopen-no-double-init", init() == 1 && fini() == 0);
    Check("apple-reopen-close", dlclose(reopened) == 0);
    if (std::strcmp(argv[2], "runtime-fini") == 0) {
        RuntimeParam parameter {};
        Check("apple-runtime-init", InitCJRuntime(&parameter) == E_OK);
        Check("apple-runtime-fini", FiniCJRuntime() == E_OK);
        Check("apple-code-after-runtime-fini", value() == 37 && fini() == 0);
    }
    return 0;
}
