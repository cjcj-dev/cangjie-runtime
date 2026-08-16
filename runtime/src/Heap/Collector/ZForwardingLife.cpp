// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/ZForwardingLife.h"

namespace MapleRuntime {

std::atomic<uint64_t> ZForwardingLife::g_retainRefusedReleased{ 0 };
std::atomic<uint64_t> ZForwardingLife::g_retainRefusedClaimed{ 0 };
std::atomic<uint64_t> ZForwardingLife::g_detachWaited{ 0 };

} // namespace MapleRuntime
