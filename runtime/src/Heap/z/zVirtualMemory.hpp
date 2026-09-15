// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <cstdint>
#include <cstddef>
namespace MapleRuntime {
struct MemoryRange {
    uintptr_t start{ 0 };
    size_t size{ 0 };

    uintptr_t End() const { return start + size; }
    bool IsNull() const { return start == 0 || size == 0; }
};

}
