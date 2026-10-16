// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#pragma once
#include "Heap/z/zAbort.hpp"

namespace MapleRuntime {
inline bool ZAbort::should_abort()
{
    return _should_abort.load(std::memory_order_relaxed);
}
} // namespace MapleRuntime
