// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "metadata_code_fixture.hpp"
#include <cstdio>

#if !defined(_WIN64)
#include "Loader/ElfUnloadQuiescence.h"
#include "ObjectModel/MFuncdesc.inline.h"
using namespace MapleRuntime;
struct MetadataShapeValue { uint64_t value; };
extern "C" { MetadataShapeValue metadataShapeValues[2] {{0x1454}, {0x1455}}; }
GC_METADATA_CODE(metadataShapeCode, metadataShapeValues, MetadataShapeValue, value, 2)
#ifdef __APPLE__
extern "C" unsigned char metadataForeignCode[];
asm(".section __TEXT,__cjtestcode,regular,pure_instructions\n"
    ".balign 16\n.globl _metadataForeignCode\n_metadataForeignCode:\n.long 0\n.zero 32\n"
    ".section __CJ_METADATA,__cjfuncmap,regular\n.balign 8\n"
    ".quad _metadataForeignCode + 4\n.quad 0x1454\n"
    ".quad _metadataShapeValues\n.quad _metadataShapeValues\n.text\n");
#endif
int main()
{
    const auto pc = metadataShapeCodePC();
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<Uptr>(pc));
    ElfUnloadQuiescence::ReadScope reader;
    bool passed = true;
    for (size_t i = 0; i != 2; ++i) {
        const auto entry = metadataShapeCodePC(i);
        const auto result = MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(entry));
        const bool ownsCode = ElfUnloadQuiescence::IsLinkedAddress(reinterpret_cast<Uptr>(entry), true);
        const bool dataIsNotCode = !ElfUnloadQuiescence::IsLinkedAddress(
            reinterpret_cast<Uptr>(&metadataShapeValues[i]), true);
        const bool value = result == reinterpret_cast<FuncDescRef>(&metadataShapeValues[i]) &&
            reinterpret_cast<MetadataShapeValue*>(result)->value == 0x1454 + i;
        std::fprintf(stderr, "METADATA_CODE_SHAPE_TARGET row=%zu code=%d data=%d value=%d executed=1\n",
                     i, ownsCode, dataIsNotCode, value);
        passed &= ownsCode && dataIsNotCode && value;
    }
    const bool absent = MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(metadataShapeCodePC(0, false))) == nullptr;
    std::fprintf(stderr, "METADATA_CODE_SHAPE_ABSENT pass=%d executed=1\n", absent);
    bool codeOwner = true, descriptorOwner = true;
#ifdef __APPLE__
    codeOwner = MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(metadataShapeValues)) == nullptr;
    descriptorOwner = MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(metadataForeignCode + 4)) == nullptr;
    std::fprintf(stderr, "METADATA_CODE_SHAPE_PC_OWNER pass=%d executed=1\n", codeOwner);
    std::fprintf(stderr, "METADATA_CODE_SHAPE_DESC_OWNER pass=%d executed=1\n", descriptorOwner);
#endif
    return passed && absent && codeOwner && descriptorOwner ? 0 : 1;
}
#else
int main()
{
    std::fprintf(stderr, "METADATA_CODE_SHAPE_NOT_RUN real PE inputs are supplied by windows_input.S\n");
    return 77;
}
#endif
