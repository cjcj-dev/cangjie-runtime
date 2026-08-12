// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_EMPTY_LIVE_DIAG_H
#define MRT_EMPTY_LIVE_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// emptylive: split size-walk marks into epoch-current vs raw-bit-stale on CollectRegion.
// Gate: MRT_GCV2_EMPTYLIVE=1 or MRT_GCV2_DIAG token emptylive. Default off.
namespace EmptyLiveDiag {

bool Enabled();

// Call at CollectRegion enter (after f3why2 NoteCollectEnter is fine).
void NoteCollectEnter(RegionInfo* region);

void Report(const char* point);

} // namespace EmptyLiveDiag
} // namespace MapleRuntime

#endif // MRT_EMPTY_LIVE_DIAG_H
