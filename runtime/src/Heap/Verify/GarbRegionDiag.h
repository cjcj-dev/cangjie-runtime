// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GARBREGION_DIAG_H
#define MRT_GARBREGION_DIAG_H

#include <cstddef>

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// garbregion: live-slot census into a region at CollectRegion / F3 join.
// Gate: MRT_GCV2_GARBREGION=1 or MRT_GCV2_DIAG token garbregion. Default off.
//
// CensusBeforeForward walks the heap once (ForEachObj, same shape as FysAuditDiag)
// and counts heap slots whose target lands in each region. NoteCollectEnter
// joins that snapshot to the region being marked GARBAGE. NoteF3Join joins
// FixOldTaggedRefField's region_garbage / region_free reasons to the same row.

namespace GarbRegionDiag {

bool Enabled();

void CensusBeforeForward(const char* where);

void NoteCollectEnter(RegionInfo* region);

void NoteF3Join(RegionInfo* latestRegion, BaseObject* latest, const char* reason);

void Report(const char* point);

} // namespace GarbRegionDiag
} // namespace MapleRuntime

#endif // MRT_GARBREGION_DIAG_H
