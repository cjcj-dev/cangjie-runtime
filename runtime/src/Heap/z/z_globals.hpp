// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
// ZGC z_globals.hpp: keep defaults in one flag table.
#define Z_FLAGS(product) \
    product(double, ZAllocationSpikeTolerance, 2.0) \
    product(double, ZFragmentationLimit, 5.0) \
    product(double, ZYoungCompactionLimit, 25.0)
namespace MapleRuntime {
#define DECLARE_Z_FLAG(type, name, value) constexpr type name = value;
Z_FLAGS(DECLARE_Z_FLAG)
#undef DECLARE_Z_FLAG
}
