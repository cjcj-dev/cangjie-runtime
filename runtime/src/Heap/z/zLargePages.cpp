// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zLargePages.cpp:28-58.

#include "Heap/z/zLargePages.hpp"

#include <unistd.h>

#include "Base/Globals.h"
#include "Base/LogFile.h"

namespace MapleRuntime {

ZLargePages::State ZLargePages::_state;
bool ZLargePages::_os_enforced_transparent_mode;

static size_t physical_memory() {
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page_size = sysconf(_SC_PAGESIZE);
  if (pages <= 0 || page_size <= 0) {
    return 0;
  }
  return static_cast<size_t>(pages) * static_cast<size_t>(page_size);
}

void ZLargePages::initialize() {
  pd_initialize();

  const size_t memory = physical_memory();
  VLOG(REPORT, "Memory: %zuM", memory / MB);
  VLOG(REPORT, "Large Page Support: %s", to_string());
}

const char* ZLargePages::to_string() {
  switch (_state) {
  case Explicit:
    return "Enabled (Explicit)";

  case Transparent:
    if (_os_enforced_transparent_mode) {
      return "Enabled (Transparent, OS enforced)";
    } else {
      return "Enabled (Transparent)";
    }

  default:
    if (_os_enforced_transparent_mode) {
      return "Disabled (OS enforced)";
    } else {
      return "Disabled";
    }
  }
}

} // namespace MapleRuntime
