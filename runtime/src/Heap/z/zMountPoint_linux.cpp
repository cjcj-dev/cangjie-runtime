// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/linux/gc/z/zMountPoint_linux.cpp:37-158. AllocateHeapAt (a HotSpot
// flag) arrives here as the cjAllocateHeapAt environment variable.

#include "Heap/z/zMountPoint_linux.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Heap/z/zErrno.hpp"

namespace MapleRuntime {

// Mount information, see proc(5) for more details.
#define PROC_SELF_MOUNTINFO        "/proc/self/mountinfo"

ZMountPoint::ZMountPoint(const char* filesystem, const char** preferred_mountpoints) {
  const char* const allocate_heap_at = getenv("cjAllocateHeapAt");
  if (allocate_heap_at != nullptr) {
    // Use specified path
    _path = strdup(allocate_heap_at);
  } else {
    // Find suitable path
    _path = find_mountpoint(filesystem, preferred_mountpoints);
  }
}

ZMountPoint::~ZMountPoint() {
  free(_path);
  _path = nullptr;
}

char* ZMountPoint::get_mountpoint(const char* line, const char* filesystem) const {
  char* line_mountpoint = nullptr;
  char* line_filesystem = nullptr;

  // Parse line and return a newly allocated string containing the mount point if
  // the line contains a matching filesystem and the mount point is accessible by
  // the current user.
  if (sscanf(line, "%*u %*u %*u:%*u %*s %ms %*[^-]- %ms", &line_mountpoint, &line_filesystem) != 2 ||
      strcmp(line_filesystem, filesystem) != 0 ||
      access(line_mountpoint, R_OK|W_OK|X_OK) != 0) {
    // Not a matching or accessible filesystem
    free(line_mountpoint);
    line_mountpoint = nullptr;
  }

  free(line_filesystem);

  return line_mountpoint;
}

void ZMountPoint::get_mountpoints(const char* filesystem, ZArray<char*>* mountpoints) const {
  FILE* fd = fopen(PROC_SELF_MOUNTINFO, "r");
  if (fd == nullptr) {
    ZErrno err;
    LOG(RTLOG_ERROR, "Failed to open %s: %s", PROC_SELF_MOUNTINFO, err.to_string());
    return;
  }

  char* line = nullptr;
  size_t length = 0;

  while (getline(&line, &length, fd) != -1) {
    char* const mountpoint = get_mountpoint(line, filesystem);
    if (mountpoint != nullptr) {
      mountpoints->push_back(mountpoint);
    }
  }

  free(line);
  fclose(fd);
}

void ZMountPoint::free_mountpoints(ZArray<char*>* mountpoints) const {
  for (char* mountpoint : *mountpoints) {
    free(mountpoint);
  }
  mountpoints->clear();
}

char* ZMountPoint::find_preferred_mountpoint(const char* filesystem,
                                              ZArray<char*>* mountpoints,
                                              const char** preferred_mountpoints) const {
  // Find preferred mount point
  for (char* mountpoint : *mountpoints) {
    for (const char** preferred = preferred_mountpoints; *preferred != nullptr; preferred++) {
      if (!strcmp(mountpoint, *preferred)) {
        // Preferred mount point found
        return strdup(mountpoint);
      }
    }
  }

  // Preferred mount point not found
  LOG(RTLOG_ERROR, "More than one %s filesystem found:", filesystem);
  for (char* mountpoint : *mountpoints) {
    LOG(RTLOG_ERROR, "  %s", mountpoint);
  }

  return nullptr;
}

char* ZMountPoint::find_mountpoint(const char* filesystem, const char** preferred_mountpoints) const {
  char* path = nullptr;
  ZArray<char*> mountpoints;

  get_mountpoints(filesystem, &mountpoints);

  if (mountpoints.size() == 0) {
    // No mount point found
    LOG(RTLOG_ERROR, "Failed to find an accessible %s filesystem", filesystem);
  } else if (mountpoints.size() == 1) {
    // One mount point found
    path = strdup(mountpoints.at(0));
  } else {
    // More than one mount point found
    path = find_preferred_mountpoint(filesystem, &mountpoints, preferred_mountpoints);
  }

  free_mountpoints(&mountpoints);

  return path;
}

const char* ZMountPoint::get() const {
  return _path;
}

} // namespace MapleRuntime
