// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_OFFPAST_DIAG_H
#define MRT_OFFPAST_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;
class RegionInfo;

// offpast: same-target pregrant → Route/Compact → Fix face (UNSURE1).
// Gate default off. Product path early-returns before any table work.
//   MRT_GCV2_OFFPAST=1
// Heartbeat: one "[GCV2][offpast] heartbeat" line when first armed.
namespace OffpastDiag {

bool Enabled();

// Every young root seen at pregrant / grant-pass. Caps silently.
void NotePregrant(BaseObject* obj, const char* site);
void NotePregrantSlot(void* slot, BaseObject* obj, const char* site);

// Region about to freeze geometry (RouteOrCompactRegionImpl entry).
void NoteRouteEnter(RegionInfo* region);

// After CompactRegion packed + memset (both overloads).
void NoteCompactDone(RegionInfo* region);

// Fix admit_miss / leave-alone on a root target.
void NoteFixMiss(BaseObject* obj);
void NoteFixMissSlot(void* slot, BaseObject* obj);

} // namespace OffpastDiag
} // namespace MapleRuntime

#endif
