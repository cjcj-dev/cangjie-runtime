// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HDR_WHO_DIAG_H
#define MRT_HDR_WHO_DIAG_H

#include <cstdint>

namespace MapleRuntime {
namespace HdrWhoDiag {

// Crash-site peek: is rdi an object start, or an interior / leftover payload?
// Gate: MRT_GCV2_HDRWHO=1 or MRT_GCV2_DIAG token hdrwho. Default off.
bool Enabled();
void NoteCrashRdi(uintptr_t rdi);

} // namespace HdrWhoDiag
} // namespace MapleRuntime

#endif // MRT_HDR_WHO_DIAG_H
