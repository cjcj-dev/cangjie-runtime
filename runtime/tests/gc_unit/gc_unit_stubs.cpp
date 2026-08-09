// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Link stub for Collector::AbortUnimplemented when the linked libcangjie-runtime
// predates named-abort export. Prefer product definition when the .so provides it
// (weak so a newer runtime wins).

#include <cstdio>
#include <cstdlib>

#include "Heap/Collector/Collector.h"

namespace MapleRuntime {

__attribute__((weak)) [[noreturn]] void Collector::AbortUnimplemented(const char* method)
{
    std::fprintf(stderr, "Collector::AbortUnimplemented: %s\n", method);
    std::fflush(stderr);
    std::abort();
}

} // namespace MapleRuntime
