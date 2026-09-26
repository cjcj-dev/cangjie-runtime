// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/bsd/gc/z/zPhysicalMemoryBacking_bsd.cpp:45-180.

#include "Heap/z/zPhysicalMemoryBacking_bsd.hpp"

#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#include "Base/Log.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zErrno.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zInitialize.hpp"
#include "Heap/z/zLargePages.inline.hpp"

namespace MapleRuntime {

static int vm_flags_superpage() {
  if (!ZLargePages::is_explicit()) {
    return 0;
  }
  const int page_size_in_megabytes = static_cast<int>(ZGranuleSize >> 20);
  return page_size_in_megabytes << VM_FLAGS_SUPERPAGE_SHIFT;
}

static ZErrno mremap_mach(uintptr_t from_addr, uintptr_t to_addr, size_t size) {
  mach_vm_address_t remap_addr = to_addr;
  vm_prot_t remap_cur_prot;
  vm_prot_t remap_max_prot;
  const kern_return_t res = mach_vm_remap(mach_task_self(),
                                          &remap_addr,
                                          size,
                                          0,
                                          VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | vm_flags_superpage(),
                                          mach_task_self(),
                                          from_addr,
                                          FALSE,
                                          &remap_cur_prot,
                                          &remap_max_prot,
                                          VM_INHERIT_COPY)
  return (res == KERN_SUCCESS) ? ZErrno(0) : ZErrno(EINVAL);
}

ZPhysicalMemoryBacking::ZPhysicalMemoryBacking(size_t max_capacity)
  : _base(0),
    _initialized(false) {
  void* const res = mmap(nullptr, max_capacity, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (res == MAP_FAILED) {
    ZInitialize::error("Failed to reserve address space for backing memory");
    return;
  }
  _base = reinterpret_cast<uintptr_t>(res);
  _initialized = true;
}

bool ZPhysicalMemoryBacking::is_initialized() const {
  return _initialized;
}

void ZPhysicalMemoryBacking::warn_commit_limits(size_t max_capacity) const {
  (void)max_capacity;
}

bool ZPhysicalMemoryBacking::commit_inner(zbacking_offset offset, size_t length) const {
  const uintptr_t addr = _base + untype(offset);
  const void* const res = mmap(reinterpret_cast<void*>(addr), length, PROT_READ | PROT_WRITE,
                               MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  return res != MAP_FAILED;
}

size_t ZPhysicalMemoryBacking::commit(zbacking_offset offset, size_t length, uint32_t) const {
  if (commit_inner(offset, length)) {
    return length;
  }
  zbacking_offset start = offset;
  zbacking_offset end = to_zbacking_offset(untype(offset) + length);
  for (;;) {
    length = ((untype(end) - untype(start)) / 2) & ~(ZGranuleSize - 1);
    if (length == 0) {
      return untype(start) - untype(offset);
    }
    if (commit_inner(start, length)) {
      start = to_zbacking_offset(untype(start) + length);
    } else {
      end = to_zbacking_offset(untype(end) - length);
    }
  }
}

size_t ZPhysicalMemoryBacking::uncommit(zbacking_offset offset, size_t length) const {
  const uintptr_t start = _base + untype(offset);
  const void* const res = mmap(reinterpret_cast<void*>(start), length, PROT_NONE,
                               MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (res == MAP_FAILED) {
    return 0;
  }
  return length;
}

void ZPhysicalMemoryBacking::map(zaddress_unsafe addr, size_t size, zbacking_offset offset) const {
  const ZErrno err = mremap_mach(_base + untype(offset), untype(addr), size);
  if (err) {
    LOG(RTLOG_FATAL, "Failed to remap memory (%s)", err.to_string());
  }
}

void ZPhysicalMemoryBacking::unmap(zaddress_unsafe addr, size_t size) const {
  const void* const res = mmap(reinterpret_cast<void*>(untype(addr)), size, PROT_NONE,
       MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (res == MAP_FAILED) {
    ZErrno err;
    LOG(RTLOG_FATAL, "Failed to map memory (%s)", err.to_string());
  }
}

}
