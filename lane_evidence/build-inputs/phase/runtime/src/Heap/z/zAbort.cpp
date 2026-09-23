// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/z/zAbort.hpp"

namespace MapleRuntime {
std::atomic<bool> ZAbort::_should_abort{ false };

bool ZAbort::should_abort() { return _should_abort.load(std::memory_order_relaxed); }

void ZAbort::abort() { _should_abort.store(true, std::memory_order_relaxed); }
void ZAbort::reset() { _should_abort.store(false, std::memory_order_relaxed); }
} // namespace MapleRuntime
