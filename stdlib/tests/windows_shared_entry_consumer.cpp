// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Consume a normally produced std.core DLL and a public-cjc-produced CJ DLL.
// No synthetic metadata, CRT override, or runtime-private registration API.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include "Cangjie.h"

extern "C" void* CJ_MCC_GetPackageByQualifiedName(const char*);

static void Check(const char* name, bool passed)
{
    std::printf("TARGET %s executed=1 pass=%d\n", name, passed);
    std::fflush(stdout);
    if (!passed) { std::exit(1); }
}

static void Modules()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    Check("module-snapshot", snapshot != INVALID_HANDLE_VALUE);
    MODULEENTRY32 entry {};
    entry.dwSize = sizeof(entry);
    Check("module-first", Module32First(snapshot, &entry) != 0);
    do {
        std::printf("MODULE base=%p size=%lu path=%s\n", entry.modBaseAddr,
                    static_cast<unsigned long>(entry.modBaseSize), entry.szExePath);
    } while (Module32Next(snapshot, &entry));
    CloseHandle(snapshot);
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    if (argc != 3) { return 2; }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    HMODULE core = LoadLibraryA(argv[1]);
    // Independent OS/CRT controls run before the CJ registration assertion.
    Check("std-native-load-control", core != nullptr);
    Check("std-native-identity-control", GetModuleHandleA(argv[1]) == core);
    Modules();
    // With only the helper CRT-entry selection cut, native loading still works,
    // but this public publication boundary must reject the pending std image.
    Check("std-public-commit", LoadCJLibrary(argv[1]) == E_OK);
    Check("std-core-package-published", CJ_MCC_GetPackageByQualifiedName("std.core") != nullptr);
    RuntimeParam parameter {};
    Check("runtime-init", InitCJRuntime(&parameter) == E_OK);
    Check("public-cjc-dll-load", LoadCJLibrary(argv[2]) == E_OK);
    using Value = int (*)();
    auto value = reinterpret_cast<Value>(FindCJSymbol(argv[2], "ReadScopeStdProducerValue"));
    Check("public-cjc-dll-symbol", value != nullptr);
    Check("public-cjc-dll-result", value() == 37);
    Modules();
    Check("public-cjc-dll-close", UnloadCJLibrary(argv[2]) == E_OK);
    Check("std-public-close", UnloadCJLibrary(argv[1]) == E_OK);
    Check("std-native-close", FreeLibrary(core) != 0);
    Check("runtime-fini", FiniCJRuntime() == E_OK);
    return 0;
}
