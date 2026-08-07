// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "UnwindStack/StackWatermark.h"

#include <cstdlib>
#include <cstring>

namespace MapleRuntime {

bool StackWatermark::VerifyEnabled()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_STACK_WATERMARK_VERIFY");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

} // namespace MapleRuntime
