// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zLargePages.cpp:28-58.

#include "Heap/z/zLargePages.hpp"

#include "Base/Globals.h"
#include "Base/LogFile.h"

namespace MapleRuntime {

ZLargePages::State ZLargePages::_state;
bool ZLargePages::_os_enforced_transparent_mode;

void ZLargePages::initialize() {
  pd_initialize();
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
