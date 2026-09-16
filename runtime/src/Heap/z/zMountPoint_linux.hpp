// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/linux/gc/z/zMountPoint_linux.hpp:30-50.

#pragma once
#include "Heap/z/zArray.hpp"

namespace MapleRuntime {

class ZMountPoint {
private:
  char* _path;

  char* get_mountpoint(const char* line,
                       const char* filesystem) const;
  void get_mountpoints(const char* filesystem,
                       ZArray<char*>* mountpoints) const;
  void free_mountpoints(ZArray<char*>* mountpoints) const;
  char* find_preferred_mountpoint(const char* filesystem,
                                  ZArray<char*>* mountpoints,
                                  const char** preferred_mountpoints) const;
  char* find_mountpoint(const char* filesystem,
                        const char** preferred_mountpoints) const;

public:
  ZMountPoint(const char* filesystem, const char** preferred_mountpoints);
  ~ZMountPoint();

  const char* get() const;
};

} // namespace MapleRuntime
