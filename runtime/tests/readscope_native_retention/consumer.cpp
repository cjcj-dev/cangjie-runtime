// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "Cangjie.h"
namespace {
uintptr_t observed = 0;
void Check(const char* name, bool pass)
{
    std::printf("TARGET %s executed=1 pass=%d\n", name, pass);
    std::fflush(stdout);
    if (!pass) { std::exit(1); }
}
}
extern "C" __declspec(dllexport) void ReadScopeObserve(uintptr_t address) { observed = address; }
int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 3) { return 2; }
    const bool rollback = std::strcmp(argv[2], "native-failure") == 0;
    if (rollback) { SetEnvironmentVariableA("READSCOPE_NATIVE_ATTACH_FAIL", "1"); }
    HMODULE first = LoadLibraryA(argv[1]);
    if (rollback) {
        Check("native-attach-rejected", first == nullptr);
        Check("native-constructor-reached", observed != 0);
        MEMORY_BASIC_INFORMATION memory {};
        Check("native-pin-survives-post-registration-rollback",
              VirtualQuery(reinterpret_cast<const void*>(observed), &memory, sizeof(memory)) != 0 &&
              memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE);
        return 0;
    }
    Check("native-load", first != nullptr);
    using Counter = unsigned (*)();
    auto init = reinterpret_cast<Counter>(GetProcAddress(first, "ReadScopeInitCount"));
    auto fini = reinterpret_cast<Counter>(GetProcAddress(first, "ReadScopeFiniCount"));
    Check("actual-native-constructor", observed != 0 && init != nullptr && fini != nullptr && init() == 1 && fini() == 0);
    Check("public-load", LoadCJLibrary(argv[1]) == E_OK);
    Check("public-load-same-native-identity",
          FindCJSymbol(argv[1], "ReadScopeValue") ==
              reinterpret_cast<void*>(GetProcAddress(first, "ReadScopeValue")));
    Check("unknown-close-rejected", UnloadCJLibrary("readscope-no-such-library.dll") != 0);
    Check("logical-close", UnloadCJLibrary(argv[1]) == 0);
    Check("external-native-close", FreeLibrary(first) != 0);
    Check("no-runtime-fini", init() == 1 && fini() == 0);
    auto function = reinterpret_cast<int (*)()>(FindCJSymbol(argv[1], "ReadScopeValue"));
    Check("published-code-remains-callable", function != nullptr && function() == 37);
    HMODULE reopened = LoadLibraryA(argv[1]);
    Check("native-reopen-same-identity", reopened == first);
    Check("native-reopen-no-double-init", init() == 1 && fini() == 0);
    Check("native-reopen-close", FreeLibrary(reopened) != 0);
    if (std::strcmp(argv[2], "runtime-fini") == 0) {
        RuntimeParam parameter {};
        Check("runtime-init", InitCJRuntime(&parameter) == E_OK);
        Check("runtime-fini", FiniCJRuntime() == E_OK);
        Check("metadata-fini-keeps-native-code", function() == 37 && fini() == 0);
    }
    return 0;
}
