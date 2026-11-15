// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// A real native CRT constructor enters the same metadata producer as Cjstart.
#include <windows.h>
#include <cstdio>
#include "Loader/BinaryFile/CjFile/CjFileMeta.h"
using namespace MapleRuntime;
extern "C" void MRT_LibraryOnLoad(uint64_t, bool);
extern "C" void MRT_LibraryUnLoad(uint64_t);
namespace {
CJFileHeader header {};
U32 bytes = sizeof(header), zeroSize = 0, flagsSize = sizeof(CJGCFlagsTable);
U64 zeroAddress = 0, sdkVersionPointer = 0, sdkVersionAddress = 0, flagsAddress = 0;
CJGCFlagsTable flags { 1, 1, 0 };
unsigned initializations = 0, finalizations = 0;
struct NativeLifecycle {
    NativeLifecycle()
    {
        const char* version = "0.0.1";
        sdkVersionAddress = reinterpret_cast<U64>(version);
        sdkVersionPointer = reinterpret_cast<U64>(&sdkVersionAddress);
        flagsAddress = reinterpret_cast<U64>(&flags);
        header.magic = 0x12345678;
        header.version = 0x80000000;
        header.cJFileSize = &bytes;
        header.cJSDKVersionPtr = &sdkVersionPointer;
        for (auto& table : header.tables) { table = { &zeroAddress, &zeroSize }; }
        header.tables[GC_FLAGS_TABLE] = { &flagsAddress, &flagsSize };
        using Observe = void (*)(uintptr_t);
        auto observe = reinterpret_cast<Observe>(GetProcAddress(GetModuleHandleA(nullptr), "ReadScopeObserve"));
        if (observe != nullptr) { observe(reinterpret_cast<uintptr_t>(&header)); }
        MRT_LibraryOnLoad(reinterpret_cast<uint64_t>(&header), true);
        ++initializations;
        std::fprintf(stderr, "READSCOPE_FIXTURE_INIT count=%u\n", initializations);
    }
    ~NativeLifecycle()
    {
        ++finalizations;
        std::fprintf(stderr, "READSCOPE_FIXTURE_FINI count=%u\n", finalizations);
        MRT_LibraryUnLoad(reinterpret_cast<uint64_t>(&header));
    }
} lifecycle;
}
extern "C" __declspec(dllexport) unsigned ReadScopeInitCount() { return initializations; }
extern "C" __declspec(dllexport) unsigned ReadScopeFiniCount() { return finalizations; }
extern "C" __declspec(dllexport) int ReadScopeValue() { return 37; }
// Exercise rollback after the CJ constructor has run, rather than pretending
// native loader failure can only occur before metadata registration.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    char value[2] {};
    if (reason == DLL_PROCESS_ATTACH &&
        GetEnvironmentVariableA("READSCOPE_NATIVE_ATTACH_EXCEPTION", value, 2) != 0) {
        RaiseException(0xe042434a, 0, 0, nullptr);
    }
    return reason != DLL_PROCESS_ATTACH || GetEnvironmentVariableA("READSCOPE_NATIVE_ATTACH_FAIL", value, 2) == 0;
}
