// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zPageAge.hpp"
namespace MapleRuntime {
constexpr uint32_t untype(PageAge age) { return static_cast<uint32_t>(age); }

constexpr PageAge to_pageage(uint32_t age)
{
    return static_cast<PageAge>(age);
}

inline PageAge operator+(PageAge age, size_t size)
{
    return to_pageage(untype(age) + static_cast<uint32_t>(size));
}

inline PageAge operator-(PageAge age, size_t size)
{
    return to_pageage(untype(age) - static_cast<uint32_t>(size));
}

}
