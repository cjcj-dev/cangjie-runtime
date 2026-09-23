// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/windows/gc/z/zSyscall_windows.hpp:31-51.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace MapleRuntime {

class ZSyscall {
private:
  typedef HANDLE (*CreateFileMappingWFn)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
  typedef HANDLE (*CreateFileMapping2Fn)(HANDLE, LPSECURITY_ATTRIBUTES, ULONG, ULONG, ULONG, ULONG64, PCWSTR, void*, ULONG);
  typedef PVOID (*VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
  typedef BOOL (*VirtualFreeExFn)(HANDLE, LPVOID, SIZE_T, DWORD);
  typedef PVOID (*MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
  typedef BOOL (*UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);

public:
  static CreateFileMappingWFn CreateFileMappingW;
  static CreateFileMapping2Fn CreateFileMapping2;
  static VirtualAlloc2Fn      VirtualAlloc2;
  static VirtualFreeExFn      VirtualFreeEx;
  static MapViewOfFile3Fn     MapViewOfFile3;
  static UnmapViewOfFile2Fn   UnmapViewOfFile2;

  static void initialize();
  static bool is_supported();
  static bool is_large_pages_supported();
};

}
