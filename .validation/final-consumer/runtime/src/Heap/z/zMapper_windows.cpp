// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/windows/gc/z/zMapper_windows.cpp:62-309.

#include "Heap/z/zMapper_windows.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zSyscall_windows.hpp"

#include <cassert>

#include "Base/Log.h"

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

namespace MapleRuntime {

zaddress_unsafe ZMapper::reserve(zaddress_unsafe addr, size_t size) {
  void* const res = ZSyscall::VirtualAlloc2(
    GetCurrentProcess(),
    reinterpret_cast<void*>(untype(addr)),
    size,
    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
    PAGE_NOACCESS,
    nullptr,
    0);
  return to_zaddress_unsafe(reinterpret_cast<uintptr_t>(res));
}

void ZMapper::unreserve(zaddress_unsafe addr, size_t size) {
  (void)size;
  const BOOL res = ZSyscall::VirtualFreeEx(GetCurrentProcess(), reinterpret_cast<void*>(untype(addr)), 0, MEM_RELEASE);
  assert(res);
  (void)res;
}

HANDLE ZMapper::create_paging_file_mapping(size_t size) {
  return ZSyscall::CreateFileMappingW(
    INVALID_HANDLE_VALUE,
    nullptr,
    PAGE_READWRITE | SEC_RESERVE,
    static_cast<DWORD>(size >> 32),
    static_cast<DWORD>(size & 0xFFFFFFFFu),
    nullptr);
}

uintptr_t ZMapper::map_view_no_placeholder(HANDLE file_handle, uintptr_t file_offset, size_t size) {
  void* const res = ZSyscall::MapViewOfFile3(
    file_handle, GetCurrentProcess(), nullptr, file_offset, size, 0, PAGE_NOACCESS, nullptr, 0);
  return reinterpret_cast<uintptr_t>(res);
}

void ZMapper::unmap_view_no_placeholder(uintptr_t addr, size_t size) {
  (void)size;
  const BOOL res = ZSyscall::UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(addr), 0);
  assert(res);
  (void)res;
}

uintptr_t ZMapper::commit(uintptr_t addr, size_t size) {
  void* const res = ZSyscall::VirtualAlloc2(
    GetCurrentProcess(), reinterpret_cast<void*>(addr), size, MEM_COMMIT, PAGE_NOACCESS, nullptr, 0);
  return reinterpret_cast<uintptr_t>(res);
}

bool ZMapper::commit_paging_file_mapping(HANDLE file_handle, uintptr_t file_offset, size_t size) {
  const uintptr_t addr = map_view_no_placeholder(file_handle, file_offset, size);
  if (addr == 0) {
    return false;
  }
  const uintptr_t res = commit(addr, size);
  unmap_view_no_placeholder(addr, size);
  return res == addr;
}

HANDLE ZMapper::create_and_commit_paging_file_mapping(size_t size) {
  HANDLE const file_handle = create_paging_file_mapping(size);
  if (file_handle == 0) {
    return 0;
  }
  if (!commit_paging_file_mapping(file_handle, 0, size)) {
    close_paging_file_mapping(file_handle);
    return 0;
  }
  return file_handle;
}

void ZMapper::close_paging_file_mapping(HANDLE file_handle) {
  const BOOL res = CloseHandle(file_handle);
  assert(res);
  (void)res;
}

HANDLE ZMapper::create_shared_awe_section() {
  return ZSyscall::CreateFileMapping2(
    INVALID_HANDLE_VALUE, nullptr,
    SECTION_MAP_READ | SECTION_MAP_WRITE,
    PAGE_READWRITE,
    SEC_RESERVE | SEC_LARGE_PAGES,
    0, nullptr, nullptr, 0);
}

zaddress_unsafe ZMapper::reserve_for_shared_awe(HANDLE awe_section, zaddress_unsafe addr, size_t size) {
  (void)awe_section;
  void* const res = ZSyscall::VirtualAlloc2(
    GetCurrentProcess(), reinterpret_cast<void*>(untype(addr)), size,
    MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE, nullptr, 0);
  return to_zaddress_unsafe(reinterpret_cast<uintptr_t>(res));
}

void ZMapper::unreserve_for_shared_awe(zaddress_unsafe addr, size_t size) {
  (void)size;
  VirtualFree(reinterpret_cast<void*>(untype(addr)), 0, MEM_RELEASE);
}

void ZMapper::split_placeholder(zaddress_unsafe addr, size_t size) {
  VirtualFree(reinterpret_cast<void*>(untype(addr)), size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
}

void ZMapper::coalesce_placeholders(zaddress_unsafe addr, size_t size) {
  VirtualFree(reinterpret_cast<void*>(untype(addr)), size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
}

void ZMapper::map_view_replace_placeholder(HANDLE file_handle, uintptr_t file_offset, zaddress_unsafe addr, size_t size) {
  ZSyscall::MapViewOfFile3(
    file_handle, GetCurrentProcess(), reinterpret_cast<void*>(untype(addr)),
    file_offset, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
}

void ZMapper::unmap_view_preserve_placeholder(zaddress_unsafe addr, size_t size) {
  (void)size;
  ZSyscall::UnmapViewOfFile2(
    GetCurrentProcess(), reinterpret_cast<void*>(untype(addr)), MEM_PRESERVE_PLACEHOLDER);
}

}
