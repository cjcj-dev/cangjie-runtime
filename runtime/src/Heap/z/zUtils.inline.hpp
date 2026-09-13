// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Base/Log.h"
namespace MapleRuntime {
namespace {
constexpr size_t MARK_STRIPE_SHIFT = 20;
bool IsPowerOfTwo(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

size_t Log2Exact(size_t value)
{
    CHECK_DETAIL(IsPowerOfTwo(value), "mark stripe count must be a power of two: %zu", value);
    size_t result = 0;
    while ((static_cast<size_t>(1) << result) != value) {
        ++result;
    }
    return result;
}
}
}
