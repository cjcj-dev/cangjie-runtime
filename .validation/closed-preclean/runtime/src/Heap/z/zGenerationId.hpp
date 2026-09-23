// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <cstdint>
namespace MapleRuntime {
enum class Generation : uint8_t {
    Young = 0,
    Old = 1,
};
enum class ZYoungType : uint8_t {
    minor,
    major_full_preclean,
    major_full_roots,
    major_partial_roots,
    none,
};
enum class ZGenerationId : uint8_t {
    young,
    old,
};
enum class ZGenerationIdOptional : uint8_t {
    young,
    old,
    none,
};
}
