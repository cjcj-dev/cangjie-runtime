// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "Cangjie.h"
namespace {
uintptr_t observed = 0;
unsigned exceptionEvents = 0, detachEvents = 0;
bool Report(const char* name, bool pass)
{
    std::printf("TARGET %s executed=1 pass=%d\n", name, pass);
    std::fflush(stdout);
    return pass;
}
void Check(const char* name, bool pass)
{
    Report(name, pass);
    if (!pass) { std::exit(1); }
}
HMODULE LoadWithExceptionObservation(const char* path, bool* caught)
{
    HMODULE result = nullptr;
    __try {
        result = LoadLibraryA(path);
    } __except (GetExceptionCode() == 0xe042434a ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        *caught = true;
    }
    return result;
}
}
extern "C" __declspec(dllexport) void ReadScopeObserve(uintptr_t address) { observed = address; }
extern "C" __declspec(dllexport) void ReadScopeObserveNativeEvent(unsigned event)
{
    if (event == 1) { ++exceptionEvents; }
    if (event == 2) { ++detachEvents; }
}
int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 4) { return 2; }
    const bool exception = std::strcmp(argv[2], "native-exception") == 0;
    const bool reuse = std::strcmp(argv[2], "native-reuse") == 0;
    const bool rollback = exception || reuse || std::strcmp(argv[2], "native-failure") == 0;
    if (exception) {
        SetEnvironmentVariableA("READSCOPE_NATIVE_ATTACH_EXCEPTION", "1");
    } else if (rollback) {
        SetEnvironmentVariableA("READSCOPE_NATIVE_ATTACH_FAIL", "1");
    }
    bool caught = false;
    HMODULE first = LoadWithExceptionObservation(argv[1], &caught);
    const DWORD loadError = first == nullptr ? GetLastError() : ERROR_SUCCESS;
    if (rollback) {
        Check("native-attach-rejected", first == nullptr);
        if (exception) {
            std::printf("NATIVE_EXCEPTION raised=%u detach=%u caller_caught=%d load_error=%lu\n",
                        exceptionEvents, detachEvents, caught, static_cast<unsigned long>(loadError));
            Check("native-exception-observed", exceptionEvents == 1);
            Check("native-exception-no-detach", detachEvents == 0);
        }
        Check("native-constructor-reached", observed != 0);
        const uintptr_t failedMetadata = observed;
        MEMORY_BASIC_INFORMATION memory {};
        bool safe = Report("native-pin-survives-post-registration-rollback",
            VirtualQuery(reinterpret_cast<const void*>(failedMetadata), &memory, sizeof(memory)) != 0 &&
            memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE);
        std::printf("NATIVE_ROLLBACK metadata=%p allocation=%p state=%lu type=%lu exception=%d\n",
                    reinterpret_cast<void*>(failedMetadata), memory.AllocationBase,
                    static_cast<unsigned long>(memory.State), static_cast<unsigned long>(memory.Type), caught);
        if (reuse) {
            SetEnvironmentVariableA("READSCOPE_NATIVE_ATTACH_FAIL", nullptr);
            observed = 0;
            HMODULE second = LoadLibraryA(argv[3]);
            Check("independent-native-load-after-rollback", second != nullptr && observed != 0);
            std::printf("NATIVE_REUSE old_metadata=%p new_metadata=%p same_address=%d\n",
                        reinterpret_cast<void*>(failedMetadata), reinterpret_cast<void*>(observed),
                        failedMetadata == observed);
            safe &= Report("failed-image-address-not-reassigned", failedMetadata != observed);
            Check("independent-native-close", FreeLibrary(second) != 0);
        }
        return safe ? 0 : 1;
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
