// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zAbort.hpp"

namespace MapleRuntime {
bool ZAbort::IsRequested() const { return requested.load(std::memory_order_acquire); }
}

namespace MapleRuntime {
bool ZAbort::Poll() const { return IsRequested(); }
}
