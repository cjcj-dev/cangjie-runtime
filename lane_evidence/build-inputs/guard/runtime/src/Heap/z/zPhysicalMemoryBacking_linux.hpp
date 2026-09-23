// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/linux/gc/z/zPhysicalMemoryBacking_linux.hpp:31-77.

#pragma once
#include <cstddef>
#include <cstdint>

#include "Heap/z/zAddress.hpp"

namespace MapleRuntime {

class ZErrno;

class ZPhysicalMemoryBacking {
private:
  int      _fd;
  size_t   _size;
  uint64_t _filesystem;
  size_t   _block_size;
  size_t   _available;
  bool     _initialized;

  void warn_available_space(size_t max_capacity) const;
  void warn_max_map_count(size_t max_capacity) const;

  int create_mem_fd(const char* name) const;
  int create_file_fd(const char* name) const;
  int create_fd(const char* name) const;

  bool is_tmpfs() const;
  bool is_hugetlbfs() const;
  bool tmpfs_supports_transparent_huge_pages() const;

  ZErrno fallocate_compat_mmap_hugetlbfs(zbacking_offset offset, size_t length, bool touch) const;
  ZErrno fallocate_compat_mmap_tmpfs(zbacking_offset offset, size_t length) const;
  ZErrno fallocate_compat_pwrite(zbacking_offset offset, size_t length) const;
  ZErrno fallocate_fill_hole_compat(zbacking_offset offset, size_t length) const;
  ZErrno fallocate_fill_hole_syscall(zbacking_offset offset, size_t length) const;
  ZErrno fallocate_fill_hole(zbacking_offset offset, size_t length) const;
  ZErrno fallocate_punch_hole(zbacking_offset offset, size_t length) const;
  ZErrno split_and_fallocate(bool punch_hole, zbacking_offset offset, size_t length) const;
  ZErrno fallocate(bool punch_hole, zbacking_offset offset, size_t length) const;

  bool commit_inner(zbacking_offset offset, size_t length) const;
  size_t commit_numa_preferred(zbacking_offset offset, size_t length, uint32_t numa_id) const;
  size_t commit_default(zbacking_offset offset, size_t length) const;

public:
  ZPhysicalMemoryBacking(size_t max_capacity);
  ~ZPhysicalMemoryBacking();

  bool is_initialized() const;

  void warn_commit_limits(size_t max_capacity) const;

  size_t commit(zbacking_offset offset, size_t length, uint32_t numa_id) const;
  size_t uncommit(zbacking_offset offset, size_t length) const;

  void map(zaddress_unsafe addr, size_t size, zbacking_offset offset) const;
  void unmap(zaddress_unsafe addr, size_t size) const;
};

} // namespace MapleRuntime
