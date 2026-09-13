// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zAbort.hpp"

namespace MapleRuntime {
void ZAbort::Request() { requested.store(true, std::memory_order_release); }
}

#include "Heap/z/zAbort.inline.hpp"
