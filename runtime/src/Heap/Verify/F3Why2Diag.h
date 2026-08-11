// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_F3WHY2_DIAG_H
#define MRT_F3WHY2_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// livesame / f3why2: CollectRegion enter class + knownEmpty_marked + F3 join.
// Also ORDER samples on ForwardRegion success path (live vs mark timing).

namespace F3Why2Diag {

void NoteCollectEnter(RegionInfo* region);

void NoteF3RegionGarbage(RegionInfo* latestRegion, BaseObject* latest);

// Call on FORWARDED success path: liveBeforeReset, markedBefore, liveAfterReset,
// markedAfterReset, markedAfterInvalidate (after epoch bump).
void NoteForwardOrder(RegionInfo* region, uint64_t liveBefore, size_t markedBefore, uint64_t liveAfterReset,
                      size_t markedAfterReset, size_t markedAfterInvalidate);

void Report(const char* point);

// Size-walk mark count for ORDER samples (same as enter class).
void CountMarks(RegionInfo* region, size_t& validOut, size_t& markedOut);

} // namespace F3Why2Diag
} // namespace MapleRuntime

#endif // MRT_F3WHY2_DIAG_H
