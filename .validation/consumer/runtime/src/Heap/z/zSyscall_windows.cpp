// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/windows/gc/z/zSyscall_windows.cpp:29-91.

#include "Heap/z/zSyscall_windows.hpp"

#include "Base/Log.h"

namespace MapleRuntime {

ZSyscall::CreateFileMappingWFn ZSyscall::CreateFileMappingW;
ZSyscall::CreateFileMapping2Fn ZSyscall::CreateFileMapping2;
ZSyscall::VirtualAlloc2Fn ZSyscall::VirtualAlloc2;
ZSyscall::VirtualFreeExFn ZSyscall::VirtualFreeEx;
ZSyscall::MapViewOfFile3Fn ZSyscall::MapViewOfFile3;
ZSyscall::UnmapViewOfFile2Fn ZSyscall::UnmapViewOfFile2;

static void* lookup_kernelbase_library() {
  return reinterpret_cast<void*>(LoadLibraryA("KernelBase.dll"));
}

static void* lookup_kernelbase_symbol(const char* name) {
  static void* const handle = lookup_kernelbase_library();
  if (handle == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
}

static bool has_kernelbase_symbol(const char* name) {
  return lookup_kernelbase_symbol(name) != nullptr;
}

template <typename Fn>
static void install_kernelbase_symbol(Fn*& fn, const char* name) {
  fn = reinterpret_cast<Fn*>(lookup_kernelbase_symbol(name));
}

void ZSyscall::initialize() {
  install_kernelbase_symbol(CreateFileMappingW, "CreateFileMappingW");
  install_kernelbase_symbol(VirtualAlloc2,      "VirtualAlloc2");
  install_kernelbase_symbol(VirtualFreeEx,      "VirtualFreeEx");
  install_kernelbase_symbol(MapViewOfFile3,     "MapViewOfFile3");
  install_kernelbase_symbol(UnmapViewOfFile2,   "UnmapViewOfFile2");
  install_kernelbase_symbol(CreateFileMapping2, "CreateFileMapping2");
}

bool ZSyscall::is_supported() {
  return has_kernelbase_symbol("VirtualAlloc2");
}

bool ZSyscall::is_large_pages_supported() {
  return has_kernelbase_symbol("CreateFileMapping2");
}

}
