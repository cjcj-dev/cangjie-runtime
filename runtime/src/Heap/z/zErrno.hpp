// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zErrno.hpp:29-41.

#pragma once

namespace MapleRuntime {

class ZErrno {
private:
  const int _error;

public:
  ZErrno();
  ZErrno(int error);

  operator bool() const;
  bool operator==(int error) const;
  bool operator!=(int error) const;
  const char* to_string() const;
};

} // namespace MapleRuntime
