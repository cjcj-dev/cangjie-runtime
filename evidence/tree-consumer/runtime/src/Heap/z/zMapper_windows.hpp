// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/windows/gc/z/zMapper_windows.hpp:33-90.

#pragma once

#include "Heap/z/zAddress.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace MapleRuntime {

class ZMapper {
private:
  static HANDLE create_paging_file_mapping(size_t size);
  static bool commit_paging_file_mapping(HANDLE file_handle, uintptr_t file_offset, size_t size);
  static uintptr_t map_view_no_placeholder(HANDLE file_handle, uintptr_t file_offset, size_t size);
  static void unmap_view_no_placeholder(uintptr_t addr, size_t size);
  static uintptr_t commit(uintptr_t addr, size_t size);

public:
  static zaddress_unsafe reserve(zaddress_unsafe addr, size_t size);
  static void unreserve(zaddress_unsafe addr, size_t size);
  static HANDLE create_and_commit_paging_file_mapping(size_t size);
  static void close_paging_file_mapping(HANDLE file_handle);
  static HANDLE create_shared_awe_section();
  static zaddress_unsafe reserve_for_shared_awe(HANDLE awe_section, zaddress_unsafe addr, size_t size);
  static void unreserve_for_shared_awe(zaddress_unsafe addr, size_t size);
  static void split_placeholder(zaddress_unsafe addr, size_t size);
  static void coalesce_placeholders(zaddress_unsafe addr, size_t size);
  static void map_view_replace_placeholder(HANDLE file_handle, uintptr_t file_offset, zaddress_unsafe addr, size_t size);
  static void unmap_view_preserve_placeholder(zaddress_unsafe addr, size_t size);
};

}
