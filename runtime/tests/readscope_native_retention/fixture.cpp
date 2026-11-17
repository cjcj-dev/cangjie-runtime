// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// A real native CRT constructor enters the same metadata producer as Cjstart.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "ObjectModel/PackageInfo.h"
#include "Loader/BinaryFile/CjFile/CjFileMeta.h"
using namespace MapleRuntime;
extern "C" void* MRT_LibraryOnLoadPendingV1(uint64_t, bool, void**);
extern "C" void MRT_LibraryNativeDetachV1(void*);
extern "C" { void* __CJ_NativeRegistrationV1 = nullptr; }
namespace {
CJFileHeader header {};
// Serialized empty package emitted through the real PACKINFO table. Its ABI
// is checked against PackageInfo; public reflection queries must discover the
// successful image and must never discover the failed native instance.
struct alignas(16) EmptyPackage {
    PackageInfo* related = nullptr;
    U32 typeInfos = 0, templates = 0, methods = 0, fields = 0;
    U8 valid = 1, reserved[7] {};
    DataRefOffset64<char> module {}, name {}, version {};
    void* slots[2] {};
} package;
static_assert(sizeof(package) == ((sizeof(PackageInfo) + 15) & ~size_t(15)), "package metadata layout");
U32 packageSize = sizeof(package);
U64 packageAddress = reinterpret_cast<U64>(&package);
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
        char modulePath[MAX_PATH] {};
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<const char*>(&header), &module);
        GetModuleFileNameA(module, modulePath, MAX_PATH);
        const char* name = std::strstr(modulePath, "companion") != nullptr ? "readscope.success" : "readscope.failed";
        package.name.refOffset = reinterpret_cast<Sptr>(name) - reinterpret_cast<Sptr>(&package.name);
        package.module.refOffset = reinterpret_cast<Sptr>(name) - reinterpret_cast<Sptr>(&package.module);
        package.version.refOffset = reinterpret_cast<Sptr>(version) - reinterpret_cast<Sptr>(&package.version);
        header.tables[PACKINFO_TABLE] = { &packageAddress, &packageSize };
        using Observe = void (*)(uintptr_t);
        auto observe = reinterpret_cast<Observe>(GetProcAddress(GetModuleHandleA(nullptr), "ReadScopeObserve"));
        if (observe != nullptr) { observe(reinterpret_cast<uintptr_t>(&header)); }
        MRT_LibraryOnLoadPendingV1(reinterpret_cast<uint64_t>(&header), true, &__CJ_NativeRegistrationV1);
        ++initializations;
        std::fprintf(stderr, "READSCOPE_FIXTURE_INIT count=%u package=%s\n", initializations, package.name.GetDataRef());
    }
    ~NativeLifecycle()
    {
        ++finalizations;
        std::fprintf(stderr, "READSCOPE_FIXTURE_FINI count=%u package=%s\n", finalizations, package.name.GetDataRef());
        MRT_LibraryNativeDetachV1(__CJ_NativeRegistrationV1);
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
    // The loader may consume an attach exception instead of delivering it to
    // the caller's SEH frame. Observe native events in host-owned storage,
    // which remains valid even if the failed image is unmapped.
    using Event = void (*)(unsigned);
    auto event = reinterpret_cast<Event>(GetProcAddress(GetModuleHandleA(nullptr), "ReadScopeObserveNativeEvent"));
    if (reason == DLL_PROCESS_DETACH && event != nullptr) { event(2); }
    char value[2] {};
    if (reason == DLL_PROCESS_ATTACH &&
        GetEnvironmentVariableA("READSCOPE_NATIVE_ATTACH_EXCEPTION", value, 2) != 0) {
        if (event != nullptr) { event(1); }
        RaiseException(0xe042434a, 0, 0, nullptr);
    }
    return reason != DLL_PROCESS_ATTACH || GetEnvironmentVariableA("READSCOPE_NATIVE_ATTACH_FAIL", value, 2) == 0;
}
