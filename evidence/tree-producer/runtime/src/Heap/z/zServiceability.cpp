// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zServiceability.hpp"
#include <algorithm>

namespace MapleRuntime {
ZMemoryUsageInfo ComputeMemoryUsageInfo(size_t capacity, size_t maxCapacity,
                                      size_t youngUsed, size_t oldUsed)
{
    const size_t oldCapacity = std::min(oldUsed, capacity);
    const size_t youngCapacity = capacity - oldCapacity;
    return {{std::min(youngUsed, youngCapacity), youngCapacity, maxCapacity},
            {oldCapacity, oldCapacity, maxCapacity}};
}
}
