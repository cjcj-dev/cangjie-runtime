// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#if defined(__linux__)
#include <cstddef>
#include "Loader/BinaryFile/CjFile/CjFileMeta.h"
using namespace MapleRuntime;
extern "C" void PackageInitImagePackage() {}
extern "C" void PackageInitImageUnit() {}
namespace {
struct ImageMetadata {
    CJFileHeader header {};
    CJGCFlagsTable flags { 1, 1, 0 };
    Uptr entries[1] { reinterpret_cast<Uptr>(&PackageInitImagePackage) };
} metadata;
}
extern "C" void* PackageInitImageMetadata()
{
    metadata.header.cJFileSize = sizeof(metadata);
    metadata.header.tables[GC_FLAGS_TABLE] = { offsetof(ImageMetadata, flags), sizeof(metadata.flags) };
    metadata.header.tables[GLOBAL_INIT_FUNC_TABLE] = { offsetof(ImageMetadata, entries), sizeof(metadata.entries) };
    return &metadata;
}

namespace {
void (*unloadNotice)() = nullptr;
}
extern "C" void MRT_LibraryUnLoad(uint64_t);
extern "C" void PackageInitImageArmUnload(void (*notice)()) { unloadNotice = notice; }
__attribute__((destructor)) static void UnloadImage()
{
    if (unloadNotice != nullptr) {
        unloadNotice();
        MRT_LibraryUnLoad(reinterpret_cast<uintptr_t>(&metadata));
    }
}

#endif
