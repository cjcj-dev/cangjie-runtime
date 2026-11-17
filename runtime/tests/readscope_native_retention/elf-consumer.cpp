// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include "Cangjie.h"
extern "C" void* CJ_MCC_GetPackageByQualifiedName(const char*);
static void Check(const char* name, bool pass)
{
    std::printf("TARGET %s executed=1 pass=%d\n", name, pass);
    std::fflush(stdout);
    if (!pass) { std::exit(1); }
}
int main(int argc, char** argv)
{
    if (argc != 3) { return 2; }
    void* first = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!first) { std::fprintf(stderr, "%s\n", dlerror()); }
    Check("elf-native-load", first != nullptr);
    using Counter = unsigned (*)();
    auto init = reinterpret_cast<Counter>(dlsym(first, "ReadScopeInitCount"));
    auto fini = reinterpret_cast<Counter>(dlsym(first, "ReadScopeFiniCount"));
    auto value = reinterpret_cast<int (*)()>(dlsym(first, "ReadScopeValue"));
    auto package = reinterpret_cast<void* (*)()>(dlsym(first, "ReadScopePackage"));
    Check("elf-constructor-exactly-once", init && fini && value && package && init() == 1 && fini() == 0);
    Check("elf-public-load", LoadCJLibrary(argv[1]) == E_OK);
    Check("elf-public-native-identity", FindCJSymbol(argv[1], "ReadScopeValue") == reinterpret_cast<void*>(value));
    Check("elf-unknown-close-rejected", UnloadCJLibrary("readscope-no-such-library.so") != 0);
    Check("elf-logical-close", UnloadCJLibrary(argv[1]) == E_OK);
    Check("elf-external-native-close", dlclose(first) == 0);
    Check("elf-no-close-fini", init() == 1 && fini() == 0 && value() == 37);
    void* reopened = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    Check("elf-reopen-same-identity", reopened == first && dlsym(reopened, "ReadScopeValue") == reinterpret_cast<void*>(value));
    Check("elf-reopen-no-double-init", init() == 1 && fini() == 0);
    Check("elf-reopen-close", dlclose(reopened) == 0);
    RuntimeParam parameters {};
    Check("elf-runtime-init", InitCJRuntime(&parameters) == E_OK);
    Check("elf-published-package", CJ_MCC_GetPackageByQualifiedName("readscope.elf") == package());
    if (std::strcmp(argv[2], "runtime-fini") == 0) {
        Check("elf-runtime-fini", FiniCJRuntime() == E_OK);
        Check("elf-code-after-runtime-fini", value() == 37 && fini() == 0);
    }
    return 0;
}
