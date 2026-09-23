// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zLargePages.hpp:29-50.

#pragma once

namespace MapleRuntime {

class ZLargePages {
private:
  enum State {
    Disabled,
    Explicit,
    Transparent
  };

  static State _state;
  static bool  _os_enforced_transparent_mode;

  static void pd_initialize();

public:
  static void initialize();

  static bool is_enabled();
  static bool is_explicit();
  static bool is_transparent();

  static const char* to_string();
};

} // namespace MapleRuntime
