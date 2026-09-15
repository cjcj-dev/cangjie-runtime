// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/posix/gc/z/zVirtualMemoryManager_posix.cpp:33-59.

#include "Heap/z/zVirtualMemoryManager.hpp"

#include <cassert>
#include <sys/mman.h>

#include "Heap/z/zAddress.inline.hpp"

namespace MapleRuntime {

void ZVirtualMemoryReserver::pd_register_callbacks(ZVirtualMemoryRegistry* registry) {
  // Does nothing
  (void)registry;
}

bool ZVirtualMemoryReserver::pd_reserve(zaddress_unsafe addr, size_t size) {
  int flags = MAP_ANONYMOUS|MAP_PRIVATE|MAP_NORESERVE;
#if defined(__linux__) && defined(MAP_FIXED_NOREPLACE)
  flags |= MAP_FIXED_NOREPLACE;
#endif

  void* const res = mmap((void*)untype(addr), size, PROT_NONE, flags, -1, 0);
  if (res == MAP_FAILED) {
    // Failed to reserve memory
    return false;
  }

  if (res != (void*)untype(addr)) {
    // Failed to reserve memory at the requested address
    munmap(res, size);
    return false;
  }

  // Success
  return true;
}

void ZVirtualMemoryReserver::pd_unreserve(zaddress_unsafe addr, size_t size) {
  const int res = munmap((void*)untype(addr), size);
  assert(res == 0);
  (void)res;
}

} // namespace MapleRuntime
