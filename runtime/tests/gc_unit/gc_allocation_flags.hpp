// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Heap/z/zAllocationFlags.hpp"

namespace MapleRuntime::GcUnit {
// Fixtures that require a best-effort page request use the product flag.
inline ZAllocationFlags NonBlockingAllocationFlags()
{
    ZAllocationFlags flags;
    flags.set_non_blocking();
    return flags;
}
} // namespace MapleRuntime::GcUnit
