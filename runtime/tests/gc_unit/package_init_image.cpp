// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#if defined(__linux__)
#include <cstddef>
#include "Loader/BinaryFile/CjFile/CjFileMeta.h"
#include "ObjectModel/RefField.h"
using namespace MapleRuntime;
extern "C" void PackageInitImagePackage() {}
extern "C" void PackageInitImageUnit() {}
namespace {
struct ImageMetadata {
    CJFileHeader header {};
    CJGCFlagsTable flags { 1, 1, 0 };
    Uptr entries[1] { reinterpret_cast<Uptr>(&PackageInitImagePackage) };
    // A static root published by the library image itself. The product
    // registration entry (CjFileLoader.cpp:44 -> CJFile::LoadLinuxCJFileMeta)
    // must hand this table to the root enumeration. The GC root table holds
    // slot addresses, as LoaderManager.h:25-27 declares.
    NativeSlot root { MAddress { 0 } };
    NativeSlot* roots[1] { nullptr };
} metadata;
}
extern "C" MAddress* PackageInitImageRootSlot() { return reinterpret_cast<MAddress*>(&metadata.root); }
extern "C" void PackageInitImageSetRoot(MAddress value) { (void)metadata.root.Exchange(to_zpointer(value)); }
extern "C" void* PackageInitImageMetadata()
{
    metadata.roots[0] = &metadata.root;
    metadata.header.cJFileSize = sizeof(metadata);
    metadata.header.tables[GC_FLAGS_TABLE] = { offsetof(ImageMetadata, flags), sizeof(metadata.flags) };
    metadata.header.tables[GLOBAL_INIT_FUNC_TABLE] = { offsetof(ImageMetadata, entries), sizeof(metadata.entries) };
    metadata.header.tables[GC_ROOT_TABLE] = { offsetof(ImageMetadata, roots), sizeof(metadata.roots) };
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
