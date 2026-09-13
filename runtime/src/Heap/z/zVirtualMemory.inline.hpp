// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <limits>
#include <cstddef>
#include <cstdint>
namespace MapleRuntime {
namespace {
constexpr size_t kDefaultSafeFraction = 2;
constexpr unsigned long kMaxNumaNodes = sizeof(unsigned long) * 8;
constexpr int kMpolPreferred = 1;
constexpr int kMpolMemsAllowed = 2;

bool AddOverflows(uintptr_t start, size_t size)
{
    return size > std::numeric_limits<uintptr_t>::max() - start;
}

}
}
