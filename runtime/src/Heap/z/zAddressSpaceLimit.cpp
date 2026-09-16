// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zAddressSpaceLimit.cpp:33-47. os::reserve_memory_limit() is the
// RLIMIT_AS soft limit (os_posix.cpp); MaxVirtMemFraction is the HotSpot
// flag default 2 (no flag system here, PLAN infra I15).

#include "Heap/z/zAddressSpaceLimit.hpp"

#include <cstdint>
#include <limits>
#include <sys/resource.h>

#include "Base/Globals.h"
#include "Base/LogFile.h"
#include "Heap/z/zAddress.hpp"

namespace MapleRuntime {

static const size_t MaxVirtMemFraction = 2;

static size_t reserve_memory_limit() {
  struct rlimit rlim;
  if (getrlimit(RLIMIT_AS, &rlim) != 0 || rlim.rlim_cur == RLIM_INFINITY) {
    return std::numeric_limits<size_t>::max();
  }
  return static_cast<size_t>(rlim.rlim_cur);
}

size_t ZAddressSpaceLimit::heap() {
  // Allow the heap to occupy 50% of the address space
  const size_t limit = reserve_memory_limit() / MaxVirtMemFraction;
  return AlignUp(limit, ZBackingGranuleSize);
}

void ZAddressSpaceLimit::print_limits() {
  const size_t limit = reserve_memory_limit();

  if (limit == std::numeric_limits<size_t>::max()) {
    VLOG(REPORT, "Address Space Size: unlimited");
  } else {
    VLOG(REPORT, "Address Space Size: limited (%zuM)", limit / MB);
  }
}

} // namespace MapleRuntime
