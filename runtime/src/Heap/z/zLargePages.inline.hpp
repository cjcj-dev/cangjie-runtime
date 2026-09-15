// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zLargePages.inline.hpp:29-39.

#pragma once
#include "Heap/z/zLargePages.hpp"

namespace MapleRuntime {

inline bool ZLargePages::is_enabled() {
  return _state != Disabled;
}

inline bool ZLargePages::is_explicit() {
  return _state == Explicit;
}

inline bool ZLargePages::is_transparent() {
  return _state == Transparent;
}

} // namespace MapleRuntime
