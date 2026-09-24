// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/linux/gc/z/zLargePages_linux.cpp:29-45. HotSpot reads the
// -XX:+UseTransparentHugePages / -XX:+UseLargePages flags (os::Linux::
// thp_requested(), UseLargePages); runtime parameters here arrive through
// the environment (PLAN infra I15): cjUseTransparentHugePages / cjUseLargePages.
// HugePages::shmem_thp_info() is the /sys/kernel/mm/transparent_hugepage/
// shmem_enabled file (hugepages.cpp ShmemTHPSupport::scan_os).

#include "RuntimeConfig.h"
#include "Heap/z/zLargePages.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace MapleRuntime {

#define ZFILENAME_SHMEM_ENABLED "/sys/kernel/mm/transparent_hugepage/shmem_enabled"

// hugepages.cpp ShmemTHPSupport: the bracketed token names the active mode.
enum class ShmemThpMode { unknown, always, within_size, advise, never, deny, force };

static ShmemThpMode shmem_thp_mode() {
  FILE* const file = fopen(ZFILENAME_SHMEM_ENABLED, "r");
  if (file == nullptr) {
    return ShmemThpMode::unknown;
  }
  char line[256];
  ShmemThpMode mode = ShmemThpMode::unknown;
  if (fgets(line, sizeof(line), file) != nullptr) {
    const char* const open = strchr(line, '[');
    const char* const close = open != nullptr ? strchr(open, ']') : nullptr;
    if (open != nullptr && close != nullptr) {
      const size_t len = static_cast<size_t>(close - open - 1);
      if (strncmp(open + 1, "always", len) == 0) { mode = ShmemThpMode::always; }
      else if (strncmp(open + 1, "within_size", len) == 0) { mode = ShmemThpMode::within_size; }
      else if (strncmp(open + 1, "advise", len) == 0) { mode = ShmemThpMode::advise; }
      else if (strncmp(open + 1, "never", len) == 0) { mode = ShmemThpMode::never; }
      else if (strncmp(open + 1, "deny", len) == 0) { mode = ShmemThpMode::deny; }
      else if (strncmp(open + 1, "force", len) == 0) { mode = ShmemThpMode::force; }
    }
  }
  fclose(file);
  return mode;
}

static bool shmem_thp_is_disabled() {
  const ShmemThpMode mode = shmem_thp_mode();
  return mode == ShmemThpMode::never || mode == ShmemThpMode::deny;
}

static bool shmem_thp_is_forced() {
  return shmem_thp_mode() == ShmemThpMode::force;
}

static bool env_flag(const char* name) {
  const char* const value = GetRuntimeConfigValue(name);
  return value != nullptr && strcmp(value, "1") == 0;
}

void ZLargePages::pd_initialize() {
  if (env_flag("cjUseTransparentHugePages")) {
    // Check if the OS config turned off transparent huge pages for shmem.
    _os_enforced_transparent_mode = shmem_thp_is_disabled();
    _state = _os_enforced_transparent_mode ? Disabled : Transparent;
    return;
  }

  if (env_flag("cjUseLargePages")) {
    _state = Explicit;
    return;
  }

  // Check if the OS config turned on transparent huge pages for shmem.
  _os_enforced_transparent_mode = shmem_thp_is_forced();
  _state = _os_enforced_transparent_mode ? Transparent : Disabled;
}

} // namespace MapleRuntime
