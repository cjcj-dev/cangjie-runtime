// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#if defined(__linux__)
#include <cstddef>
#include <climits>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
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
} metadata, secondaryMetadata;
}
extern "C" MAddress* PackageInitImageRootSlot() { return reinterpret_cast<MAddress*>(&metadata.root); }
extern "C" void PackageInitImageSetRoot(uintptr_t value)
{
    (void)metadata.root.Exchange(to_zpointer(static_cast<MAddress>(value)));
}
static void* PrepareMetadata(ImageMetadata& value)
{
    value.roots[0] = &value.root;
    value.header.cJFileSize = sizeof(value);
    value.header.tables[GC_FLAGS_TABLE] = { offsetof(ImageMetadata, flags), sizeof(value.flags) };
    value.header.tables[GLOBAL_INIT_FUNC_TABLE] = { offsetof(ImageMetadata, entries), sizeof(value.entries) };
    value.header.tables[GC_ROOT_TABLE] = { offsetof(ImageMetadata, roots), sizeof(value.roots) };
    return &value;
}
extern "C" void* PackageInitImageMetadata() { return PrepareMetadata(metadata); }
extern "C" void* PackageInitImageSecondaryMetadata() { return PrepareMetadata(secondaryMetadata); }

// Real ELF records: the owner image has a link-loader relocation to data
// supplied by the foreign image. PCs are text, descriptors are ordinary data.
// Wrong-owner input is installed on a dedicated page, RW then RX, never W+X.
#if defined(GC_METADATA_FOREIGN_IMAGE)
extern "C" { int32_t PackageInitForeignDescriptor[8] {}; }
#endif
#if defined(GC_METADATA_OWNER_IMAGE)
extern "C" { __attribute__((visibility("hidden"))) int32_t PackageInitOwnerDescriptor[8] {}; }
extern "C" unsigned char PackageInitOwnerCode[];
// A real zero-root row, not a missing descriptor or missing stack map.
alignas(Uptr) static unsigned char ownerStackMap[64] {};

asm(".pushsection .gc_unit_metadata,\"ax\",@progbits\n"
    ".balign 65536\n.globl PackageInitOwnerCode\nPackageInitOwnerCode:\n"
    ".long PackageInitOwnerDescriptor - .\n.zero 32\n"
    ".long PackageInitOwnerDescriptor - .\n.zero 32\n"
    ".long 0\n.zero 32\n.zero 65428\n.popsection\n");
extern "C" const uint32_t* PackageInitOwnerPC(size_t row)
{
    ownerStackMap[1] = 0x10; // one PC=0 row
    ownerStackMap[2] = 0x11;
    ownerStackMap[3] = 0x11; // four zero root/table indices
    PackageInitOwnerDescriptor[0] = reinterpret_cast<char*>(ownerStackMap) -
        reinterpret_cast<char*>(PackageInitOwnerDescriptor);
    return reinterpret_cast<const uint32_t*>(PackageInitOwnerCode + 36 * row + 4);
}
#if defined(GC_METADATA_BOUNDARY_IMAGE)
// The dedicated Linux link recipe places this section at the first byte of
// its own RX PT_LOAD. Before accepting this input, readelf and the actual
// registered ranges must confirm that the preceding bytes lack owner coverage.
asm(".pushsection .a2_boundary,\"ax\",@progbits\n"
    ".globl PackageInitBoundaryCode\nPackageInitBoundaryCode:\n.zero 32\n.popsection\n");
extern "C" unsigned char PackageInitBoundaryCode[];
extern "C" const uint32_t* PackageInitBoundaryPC()
{
    return reinterpret_cast<const uint32_t*>(PackageInitBoundaryCode);
}
#endif
extern "C" bool PackageInitOwnerSetForeign(const void* descriptor)
{
    const intptr_t offset = reinterpret_cast<intptr_t>(descriptor) -
                            reinterpret_cast<intptr_t>(PackageInitOwnerCode + 36);
    if (offset < INT32_MIN || offset > INT32_MAX) { return false; }
    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(PackageInitOwnerCode) & ~(pageSize - 1));
    if (mprotect(page, pageSize, PROT_READ | PROT_WRITE) != 0) { return false; }
    const int32_t displacement = static_cast<int32_t>(offset);
    std::memcpy(PackageInitOwnerCode + 36, &displacement, sizeof(displacement));
    return mprotect(page, pageSize, PROT_READ | PROT_EXEC) == 0;
}

#endif

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
