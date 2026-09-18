// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Link stub for HeapGcState::AbortUnimplemented when the linked libcangjie-runtime
// predates named-abort export. Prefer product definition when the .so provides it
// (weak so a newer runtime wins).

#include <cstdio>
#include <cstdlib>

#include "Heap/z/zCollectedHeap.hpp"

namespace MapleRuntime {

[[noreturn]] void HeapGcState::AbortUnimplemented(const char* method)
{
    std::fprintf(stderr, "HeapGcState::AbortUnimplemented: %s\n", method);
    std::fflush(stderr);
    std::abort();
}

} // namespace MapleRuntime
