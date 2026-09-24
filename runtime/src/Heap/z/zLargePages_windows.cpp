// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/windows/gc/z/zLargePages_windows.cpp:29-38.

#include "RuntimeConfig.h"
#include "Heap/z/zLargePages.hpp"
#include "Heap/z/zSyscall_windows.hpp"

#include <cstdlib>
#include <cstring>

namespace MapleRuntime {

void ZLargePages::pd_initialize() {
  ZSyscall::initialize();
  _os_enforced_transparent_mode = false;
  const char* const value = GetRuntimeConfigValue("cjUseLargePages");
  if (value != nullptr && strcmp(value, "1") == 0) {
    if (ZSyscall::is_large_pages_supported()) {
      _state = Explicit;
      return;
    }
  }
  _state = Disabled;
}

}
