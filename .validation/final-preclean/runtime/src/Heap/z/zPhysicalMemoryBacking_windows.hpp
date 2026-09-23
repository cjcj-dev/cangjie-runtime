// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/windows/gc/z/zPhysicalMemoryBacking_windows.hpp:32-50.

#pragma once
#include "Heap/z/zAddress.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace MapleRuntime {

class ZPhysicalMemoryBackingImpl;

class ZPhysicalMemoryBacking {
private:
  ZPhysicalMemoryBackingImpl* _impl;

public:
  ZPhysicalMemoryBacking(size_t max_capacity);

  bool is_initialized() const;
  void warn_commit_limits(size_t max_capacity) const;
  size_t commit(zbacking_offset offset, size_t length, uint32_t numa_id);
  size_t uncommit(zbacking_offset offset, size_t length);
  void map(zaddress_unsafe addr, size_t size, zbacking_offset offset) const;
  void unmap(zaddress_unsafe addr, size_t size) const;
};

}
