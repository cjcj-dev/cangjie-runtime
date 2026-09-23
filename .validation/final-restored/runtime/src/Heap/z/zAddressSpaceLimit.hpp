// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zAddressSpaceLimit.hpp:30-35.

#pragma once
#include <cstddef>

namespace MapleRuntime {

class ZAddressSpaceLimit {
public:
  static size_t heap();

  static void print_limits();
};

} // namespace MapleRuntime
