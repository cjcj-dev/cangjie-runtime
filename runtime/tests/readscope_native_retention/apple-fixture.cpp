// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <cstdio>
#include "Loader/BinaryFile/CjFile/CjFileMeta.h"
using namespace MapleRuntime;
extern "C" void MRT_LibraryOnLoad(uint64_t, bool);
extern "C" void MRT_LibraryUnLoad(uint64_t);
// The same public slot as the normally produced Section.cpp object. Its value
// and its native owner are both checked by product retention, not a pathname.
extern "C" { __attribute__((visibility("default"))) uintptr_t _CJMetadataStart = 0; }
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
        _CJMetadataStart = reinterpret_cast<uintptr_t>(&header);
        MRT_LibraryOnLoad(_CJMetadataStart, true);
        ++initializations;
        std::fprintf(stderr, "APPLE_FIXTURE_INIT count=%u\n", initializations);
    }
    ~NativeLifecycle()
    {
        ++finalizations;
        std::fprintf(stderr, "APPLE_FIXTURE_FINI count=%u\n", finalizations);
        MRT_LibraryUnLoad(_CJMetadataStart);
    }
} lifecycle;
}
extern "C" unsigned ReadScopeInitCount() { return initializations; }
extern "C" unsigned ReadScopeFiniCount() { return finalizations; }
extern "C" int ReadScopeValue() { return 37; }
