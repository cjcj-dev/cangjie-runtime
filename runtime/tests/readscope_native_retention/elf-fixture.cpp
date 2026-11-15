// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Finite ELF native-loader fixture, not a generated Cangjie library.
#include <cstddef>
#include <cstdio>
#include <dlfcn.h>
#include "Loader/BinaryFile/CjFile/CjFileMeta.h"
#include "Loader/NativeImageRetention.h"
#include "ObjectModel/PackageInfo.h"
using namespace MapleRuntime;
extern "C" void MRT_LibraryOnLoadRetainedV1(uint64_t, bool, NativeImageRetainerV1);
extern "C" void MRT_LibraryUnLoad(uint64_t);
extern "C" __attribute__((visibility("protected"), noinline))
void* __CJ_ImageRetainV1(const char* path, int flags)
{
    void* result = dlopen(path, flags);
    // The return PC seen by the native loader must belong to this image.
    asm volatile("" : "+r"(result) : : "memory");
    return result;
}
namespace {
struct alignas(16) EmptyPackage {
    PackageInfo* related = nullptr;
    U32 typeInfos = 0, templates = 0, methods = 0, fields = 0;
    U8 valid = 1, reserved[7] {};
    DataRefOffset64<char> module {}, name {}, version {};
    void* slots[2] {};
};
static_assert(sizeof(EmptyPackage) == ((sizeof(PackageInfo) + 15) & ~size_t(15)), "package metadata layout");
struct ImageMetadata {
    CJFileHeader header {};
    CJGCFlagsTable flags { 1, 1, 0 };
    const char* sdkVersion = "0.0.1";
    alignas(16) EmptyPackage package {};
} metadata;
unsigned initializations = 0, finalizations = 0;
struct NativeLifecycle {
    NativeLifecycle()
    {
        const char* name = "readscope.elf";
        const char* version = "0.0.1";
        metadata.header.magic = 0x12345678;
        metadata.header.cJFileSize = sizeof(metadata);
        // CJFile::GetSDKVersion reads an in-image pointer slot, even when
        // version mismatch checks are disabled by the product configuration.
        metadata.header.cJSDKVersionOffset = offsetof(ImageMetadata, sdkVersion);
        metadata.header.tables[GC_FLAGS_TABLE] = { offsetof(ImageMetadata, flags), sizeof(metadata.flags) };
        metadata.header.tables[PACKINFO_TABLE] = { offsetof(ImageMetadata, package), sizeof(metadata.package) };
        auto& package = metadata.package;
        package.name.refOffset = reinterpret_cast<Sptr>(name) - reinterpret_cast<Sptr>(&package.name);
        package.module.refOffset = reinterpret_cast<Sptr>(name) - reinterpret_cast<Sptr>(&package.module);
        package.version.refOffset = reinterpret_cast<Sptr>(version) - reinterpret_cast<Sptr>(&package.version);
        MRT_LibraryOnLoadRetainedV1(reinterpret_cast<uint64_t>(&metadata), true, __CJ_ImageRetainV1);
        ++initializations;
        std::fprintf(stderr, "ELF_FIXTURE_INIT count=%u\n", initializations);
    }
    ~NativeLifecycle()
    {
        ++finalizations;
        std::fprintf(stderr, "ELF_FIXTURE_FINI count=%u\n", finalizations);
        MRT_LibraryUnLoad(reinterpret_cast<uint64_t>(&metadata));
    }
} lifecycle;
}
extern "C" unsigned ReadScopeInitCount() { return initializations; }
extern "C" unsigned ReadScopeFiniCount() { return finalizations; }
extern "C" int ReadScopeValue() { return 37; }
extern "C" void* ReadScopePackage() { return &metadata.package; }
