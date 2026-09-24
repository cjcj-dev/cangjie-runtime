// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/bsd/gc/z/zLargePages_bsd.cpp:27-32.

#include "RuntimeConfig.h"
#include "Heap/z/zLargePages.hpp"

#include <cstdlib>
#include <cstring>

namespace MapleRuntime {

void ZLargePages::pd_initialize() {
  _os_enforced_transparent_mode = false;
  const char* const value = GetRuntimeConfigValue("cjUseLargePages");
  if (value != nullptr && strcmp(value, "1") == 0) {
    _state = Explicit;
  } else {
    _state = Disabled;
  }
}

}
