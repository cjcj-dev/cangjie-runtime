// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zErrno.cpp:30-50.

#include "Heap/z/zErrno.hpp"

#include <errno.h>
#include <string.h>

namespace MapleRuntime {

ZErrno::ZErrno()
  : _error(errno) {}

ZErrno::ZErrno(int error)
  : _error(error) {}

ZErrno::operator bool() const {
  return _error != 0;
}

bool ZErrno::operator==(int error) const {
  return _error == error;
}

bool ZErrno::operator!=(int error) const {
  return _error != error;
}

const char* ZErrno::to_string() const {
  return strerror(_error);
}

} // namespace MapleRuntime
