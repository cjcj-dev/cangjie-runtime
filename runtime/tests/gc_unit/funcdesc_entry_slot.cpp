// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Standalone integration consumer for the paired LLVM entry-slot.ll artifact.
// Usage: funcdesc_entry_slot <linked-llc-output.so> <leaf|neighbor>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include "StackMap/StackMap.h"

using namespace MapleRuntime;

int main(int argc, char** argv)
{
    if (argc != 3 || (std::strcmp(argv[2], "leaf") != 0 && std::strcmp(argv[2], "neighbor") != 0)) {
        return 2;
    }
    const bool leaf = std::strcmp(argv[2], "leaf") == 0;
    void* image = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (image == nullptr) {
        std::fprintf(stderr, "INPUT_LOAD_FAILURE %s\n", dlerror());
        return 2;
    }
    const Uptr pc = reinterpret_cast<Uptr>(dlsym(image, leaf ? "slot_leaf" : "slot_neighbor"));
    if (pc == 0 || ElfUnloadQuiescence::LinkImage(pc) == nullptr) {
        std::fprintf(stderr, "INPUT_REGISTRATION_FAILURE\n");
        return 2;
    }
    const auto desc = MFuncDesc::GetFuncDesc(pc);
    std::fprintf(stderr, "FUNCDESC_TARGET leaf=%d present=%d expected=1\n", leaf, desc != nullptr);
    if (desc == nullptr || desc->GetStackMap() == nullptr) { return 1; }
    const FramePrologue prologue(desc->GetStackMap());
    std::fprintf(stderr, "FRAME_TARGET leaf=%d frameSize=%u\n", leaf, prologue.GetFrameSize());
    return 0;
}
