// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_WHO_PUSH_DIAG_H
#define MRT_WHO_PUSH_DIAG_H

#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// whopush: who pushed a non-start address onto the young work stack.
// Gate: MRT_GCV2_WHOPUSH=1 or MRT_GCV2_DIAG token whopush. Default off.
namespace WhoPushDiag {

bool Enabled();

void NotePush(BaseObject* object, const char* site, const void* slot = nullptr, BaseObject* holder = nullptr);

void NoteCrashRdi(uintptr_t rdi);

void Report(const char* point);

} // namespace WhoPushDiag
} // namespace MapleRuntime

#endif // MRT_WHO_PUSH_DIAG_H
