// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Compiler-format inputs in two real loader images; no product implementation.
#include <cstdint>
#include <cstddef>
#include <climits>
#include <cstring>
#ifdef _WIN64
#include <windows.h>
#define A2_EXPORT __declspec(dllexport)
#define A2_LOCAL
#else
#include <sys/mman.h>
#include <unistd.h>
#define A2_EXPORT __attribute__((visibility("default")))
#define A2_LOCAL __attribute__((visibility("hidden")))
#endif

#if defined(GC_METADATA_FOREIGN_IMAGE)
extern "C" { A2_LOCAL int32_t A2ForeignDescriptor[12] {}; }
extern "C" A2_EXPORT const void* A2GetForeignDescriptor() { return A2ForeignDescriptor; }
#else
extern "C" { A2_LOCAL int32_t A2OwnerDescriptors[2][12] {}; }
extern "C" unsigned char A2OwnerCode[], A2DataCode[];
#ifdef __APPLE__
extern "C" uintptr_t A2OwnerMap[];
asm(".section __TEXT,__cjtestcode,regular,pure_instructions\n.balign 65536\n"
    ".globl _A2OwnerCode\n_A2OwnerCode:\n"
    ".long _A2OwnerDescriptors - .\n.zero 32\n"
    ".long _A2OwnerDescriptors + 48 - .\n.zero 32\n"
    ".long 0\n.zero 32\n.zero 65428\n"
    ".section __CJ_METADATA,__cjfuncmap,regular\n.balign 8\n"
    ".globl _A2OwnerMap\n_A2OwnerMap:\n"
    ".quad _A2OwnerCode + 4\n.quad _A2OwnerDescriptors\n"
    ".quad _A2OwnerCode + 40\n.quad _A2OwnerDescriptors + 48\n"
    ".quad _A2DataCode + 4\n.quad _A2OwnerDescriptors\n"
    ".data\n.globl _A2DataCode\n_A2DataCode:\n.long _A2OwnerDescriptors - .\n.zero 16\n.text\n");
#elif defined(_WIN64)
asm(".section .gcmeta,\"xr\"\n.balign 65536\n.globl A2OwnerCode\nA2OwnerCode:\n"
    ".long A2OwnerDescriptors - .\n.zero 32\n"
    ".long A2OwnerDescriptors + 48 - .\n.zero 32\n"
    ".long 0\n.zero 32\n.zero 65428\n"
    ".data\n.globl A2DataCode\nA2DataCode:\n.long A2OwnerDescriptors - .\n.zero 16\n.text\n");
#else
asm(".pushsection .gc_unit_metadata,\"ax\",@progbits\n.balign 65536\n"
    ".globl A2OwnerCode\nA2OwnerCode:\n"
    ".long A2OwnerDescriptors - .\n.zero 32\n"
    ".long A2OwnerDescriptors + 48 - .\n.zero 32\n"
    ".long 0\n.zero 32\n.zero 65428\n.popsection\n"
    ".pushsection .data\n.globl A2DataCode\nA2DataCode:\n.long A2OwnerDescriptors - .\n.zero 16\n.popsection\n");
#endif
#if defined(__linux__) && defined(GC_METADATA_CONTIGUOUS_IMAGE)
// Reuse a2_contiguous.ld/a2_prefix_hole.py: exact LOAD ownership differs
// from page readability. The PC records are data inputs, never executed.
asm(".pushsection .a2_left,\"a\",@progbits\n"
    ".zero 4094\n.globl A2ContinuousPrefix\nA2ContinuousPrefix:\n.short 0\n.popsection\n"
    ".pushsection .a2_right,\"ax\",@progbits\n.short 0\n"
    ".globl A2ContinuousCode\nA2ContinuousCode:\n.zero 32\n.popsection\n");
extern "C" unsigned char A2ContinuousPrefix[], A2ContinuousCode[];
extern "C" A2_EXPORT const uint32_t* A2GetContinuousPC()
{
    const intptr_t delta = reinterpret_cast<intptr_t>(A2OwnerDescriptors) -
                           reinterpret_cast<intptr_t>(A2ContinuousPrefix);
    if (delta < INT32_MIN || delta > INT32_MAX) { return nullptr; }
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    if (page != 4096) { return nullptr; } // explicit loader recipe applicability
    const uintptr_t first = reinterpret_cast<uintptr_t>(A2ContinuousPrefix) & ~(page - 1);
    const uintptr_t last = reinterpret_cast<uintptr_t>(A2ContinuousCode) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(first), last - first + page, PROT_READ | PROT_WRITE) != 0) {
        return nullptr;
    }
    const int32_t displacement = static_cast<int32_t>(delta);
    std::memcpy(A2ContinuousPrefix, &displacement, sizeof(displacement));
    if (mprotect(reinterpret_cast<void*>(first), last - first, PROT_READ) != 0 ||
        mprotect(reinterpret_cast<void*>(last), page, PROT_READ | PROT_EXEC) != 0) { return nullptr; }
    return reinterpret_cast<const uint32_t*>(A2ContinuousCode);
}
#endif
extern "C" A2_EXPORT const uint32_t* A2GetOwnerPC(size_t row)
{
    return reinterpret_cast<const uint32_t*>(A2OwnerCode + row * 36 + 4);
}
extern "C" A2_EXPORT const uint32_t* A2GetDataPC() { return reinterpret_cast<const uint32_t*>(A2DataCode + 4); }
extern "C" A2_EXPORT const void* A2GetOwnerDescriptor(size_t row) { return A2OwnerDescriptors[row]; }
extern "C" A2_EXPORT bool A2SetForeignDescriptor(const void* descriptor)
{
#ifdef __APPLE__
    // Mach-O consumes the relocated function map, not the ELF prefix.
    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(A2OwnerMap) & ~(pageSize - 1));
    if (mprotect(page, pageSize, PROT_READ | PROT_WRITE) != 0) { return false; }
    A2OwnerMap[3] = reinterpret_cast<uintptr_t>(descriptor);
    return true;
#else
    const intptr_t offset = reinterpret_cast<intptr_t>(descriptor) - reinterpret_cast<intptr_t>(A2OwnerCode + 36);
    if (offset < INT32_MIN || offset > INT32_MAX) { return false; }
    const int32_t displacement = static_cast<int32_t>(offset);
#ifdef _WIN64
    DWORD protection = 0, ignored = 0;
    if (!VirtualProtect(A2OwnerCode, 65536, PAGE_READWRITE, &protection)) { return false; }
    std::memcpy(A2OwnerCode + 36, &displacement, sizeof(displacement));
    return VirtualProtect(A2OwnerCode, 65536, protection, &ignored) != 0;
#else
    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(A2OwnerCode) & ~(pageSize - 1));
    if (mprotect(page, pageSize, PROT_READ | PROT_WRITE) != 0) { return false; }
    std::memcpy(A2OwnerCode + 36, &displacement, sizeof(displacement));
    return mprotect(page, pageSize, PROT_READ | PROT_EXEC) == 0;
#endif
#endif
}
#endif
