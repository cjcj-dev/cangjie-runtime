// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <cstdint>
#include <cstdio>
#include "StackManager.h"

extern "C" int64_t R05UserMonitorScope();
extern "C" int64_t R05PlainScope();
extern "C" __attribute__((noinline)) int64_t MonitorR05Control() { return 9; }

extern "C" int64_t R05Check(bool control)
{
    using MapleRuntime::StackManager;
    // Query the real installed static runtime classifier after its normal init.
    // The @C functions require a qualified public driver that emits
    // individual function sections; do not assign their sections in C++.
    const bool native = StackManager::IsRuntimeFrame(reinterpret_cast<uintptr_t>(&MonitorR05Control) + 1);
    const bool user = StackManager::IsRuntimeFrame(reinterpret_cast<uintptr_t>(&R05UserMonitorScope) + 1);
    const bool plain = StackManager::IsRuntimeFrame(reinterpret_cast<uintptr_t>(&R05PlainScope) + 1);
    std::fprintf(stderr, "R05_CLASSIFIER_ASSERT native=%d user=%d plain=%d control=%d\n",
        native, user, plain, control);
    if (!native || plain) { return 42; }
    return !control && user ? 41 : 0;
}
