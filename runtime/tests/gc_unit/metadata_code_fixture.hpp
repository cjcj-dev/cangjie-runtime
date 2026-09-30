// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once

#include <cstddef>
#include <cstdint>

// CodeCache::find_blob (codeCache.cpp:750) owns code, not data buffers.
// These non-executed PC records have compiler-format descriptor displacements
// in executable text; their descriptors remain ordinary data. Link-time
// relocations populate the prefix, so no writable executable mapping is needed.
#ifdef __APPLE__
#define GC_METADATA_ASM_SYMBOL(name) "_" #name
#define GC_METADATA_TEXT_SECTION ".section __TEXT,__cjtestcode,regular,pure_instructions\n"
#define GC_METADATA_TEXT_END ".text\n"
#define GC_METADATA_FUNCTION_MAP(name, storage) \
    ".section __CJ_METADATA,__cjfuncmap,regular\n.balign 8\n" \
    ".set .Lmap_" #name ",0\n.rept %c[rows]\n" \
    ".quad " GC_METADATA_ASM_SYMBOL(name) " + 36 * (.Lmap_" #name " + 1) + 4\n" \
    ".quad " GC_METADATA_ASM_SYMBOL(storage) " + .Lmap_" #name \
    " * %c[stride] + %c[offset]\n" \
    ".set .Lmap_" #name ",.Lmap_" #name " + 1\n.endr\n.text\n"
#else
#define GC_METADATA_ASM_SYMBOL(name) #name
#define GC_METADATA_TEXT_SECTION ".pushsection .text.gc_unit_metadata,\"ax\",@progbits\n"
#define GC_METADATA_TEXT_END ".popsection\n"
#define GC_METADATA_FUNCTION_MAP(name, storage) ""
#endif

#ifdef __clang__
#define GC_METADATA_CODE_ATTRIBUTES __attribute__((noinline, optnone))
#else
#define GC_METADATA_CODE_ATTRIBUTES __attribute__((noinline, noclone))
#endif

// Row zero is an executable PC with an absent descriptor. Following rows name
// each element of the real data array. No runtime lookup or product function
// is copied into this fixture.
#define GC_METADATA_CODE(name, storage, type, member, count) \
    extern "C" unsigned char name[]; \
    GC_METADATA_CODE_ATTRIBUTES static const uint32_t* name##PC(size_t index = 0, bool present = true) \
    { \
        asm volatile(GC_METADATA_TEXT_SECTION \
            ".balign 16\n.globl " GC_METADATA_ASM_SYMBOL(name) "\n" \
            GC_METADATA_ASM_SYMBOL(name) ":\n.long 0\n.zero 32\n" \
            ".set .Lrow_" #name ",0\n.rept %c[rows]\n" \
            ".long " GC_METADATA_ASM_SYMBOL(storage) " + .Lrow_" #name \
            " * %c[stride] + %c[offset] - .\n.zero 32\n" \
            ".set .Lrow_" #name ",.Lrow_" #name " + 1\n.endr\n" \
            GC_METADATA_TEXT_END GC_METADATA_FUNCTION_MAP(name, storage) \
            : : [rows] "i" (count), [stride] "i" (sizeof(type)), \
                [offset] "i" (offsetof(type, member))); \
        return reinterpret_cast<const uint32_t*>(name + 36 * (present ? index + 1 : 0) + 4); \
    }
