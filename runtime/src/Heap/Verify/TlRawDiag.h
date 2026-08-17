// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_TLRAW_DIAG_H
#define MRT_TLRAW_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// tlraw: uncommitted AllocBuffer tlRawPointerRegions / tlLargeRawPointerRegions
// at minor start, plus crash-rdi region class.
// Gate: MRT_GCV2_TLRAW=1 or MRT_GCV2_DIAG token tlraw. Default off.

namespace TlRawDiag {

bool Enabled();

void NoteMinorEnter(size_t minorRun);

void NoteInitRegion(RegionInfo* region);

void NoteCrashRdi(uintptr_t rdi);

void Report(const char* point);

} // namespace TlRawDiag
} // namespace MapleRuntime

#endif // MRT_TLRAW_DIAG_H
