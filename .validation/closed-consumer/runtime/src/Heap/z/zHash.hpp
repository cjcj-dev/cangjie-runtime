// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zHash.hpp:24-38
#pragma once
#include <cstdint>

#include "Heap/z/zAddress.hpp"

namespace MapleRuntime {
class ZHash {
public:
    static uint32_t uint32_to_uint32(uint32_t key);
    static uint32_t address_to_uint32(uintptr_t key);
    static uint32_t offset_to_uint32(zoffset key);
};
} // namespace MapleRuntime
