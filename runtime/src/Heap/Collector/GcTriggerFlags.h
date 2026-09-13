// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_TRIGGER_FLAGS_H
#define MRT_GC_TRIGGER_FLAGS_H

#include <atomic>
#include <cstdint>

namespace MapleRuntime {

extern std::atomic<uint32_t> g_gcTriggerYoungWorkers;
extern std::atomic<uint32_t> g_gcTriggerOldWorkers;

} // namespace MapleRuntime
#endif // MRT_GC_TRIGGER_FLAGS_H
