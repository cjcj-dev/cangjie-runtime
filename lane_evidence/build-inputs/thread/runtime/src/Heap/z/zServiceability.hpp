// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <cstddef>

namespace MapleRuntime {
struct ZMemoryUsage {
    size_t used;
    size_t current;
    size_t max;
};

struct ZMemoryUsageInfo {
    ZMemoryUsage young;
    ZMemoryUsage old;
};

// zServiceability.cpp:41-53,142-150. Both pools share the heap limit;
// committed capacity is partitioned with old occupancy taking precedence.
ZMemoryUsageInfo ComputeMemoryUsageInfo(size_t capacity, size_t maxCapacity,
                                      size_t youngUsed, size_t oldUsed);

class ZServiceability {
public:
    ZServiceability() = default;
};
}
