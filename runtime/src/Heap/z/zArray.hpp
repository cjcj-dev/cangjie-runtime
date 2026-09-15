// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Placeholder for ZGC zArray.hpp:37-126 (P06 replaces this file with the
// ZArray/ZArraySlice/ZArrayIterator family). Until then ZArray<T> is the
// growable array the memory managers hand ranges through.

#pragma once
#include <vector>

namespace MapleRuntime {

template <typename T>
using ZArray = std::vector<T>;

} // namespace MapleRuntime
