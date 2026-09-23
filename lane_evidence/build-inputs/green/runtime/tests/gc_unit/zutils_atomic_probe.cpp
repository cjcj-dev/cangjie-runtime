// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Compile-only consumers of the product header. No copy implementation lives here.
#include "Heap/z/zUtils.inline.hpp"
using namespace MapleRuntime;
extern "C" void zutils_atomic_dynamic(zaddress from, zaddress to, size_t offset, size_t size)
{
    ZUtils::object_copy_disjoint_atomic(from, to, offset, size);
}
extern "C" void zutils_atomic_small(zaddress from, zaddress to)
{
    ZUtils::object_copy_disjoint_atomic(from, to, sizeof(uintptr_t), 8 * sizeof(uintptr_t));
}
extern "C" void zutils_atomic_large(zaddress from, zaddress to)
{
    ZUtils::object_copy_disjoint_atomic(from, to, 0, 17 * sizeof(uintptr_t));
}
extern "C" void zutils_disjoint_control(zaddress from, zaddress to, size_t size)
{
    ZUtils::object_copy_disjoint(from, to, size);
}
