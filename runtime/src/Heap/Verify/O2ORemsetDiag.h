// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_O2O_REMSET_DIAG_H
#define MRT_O2O_REMSET_DIAG_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class BaseObject;
class RegionInfo;

// o2oremset: observe old→old relocation vs remset fate (C.5 / ZGC update_remset_old_to_old).
//
// Gate (default off; product early-return):
//   MRT_GCV2_O2OREMSET=1  OR  MRT_GCV2_DIAG contains o2oremset|all
//
// Answers:
//   (1) How often do non-young from-regions successfully forward live objects?
//   (2) How many remset bits sit in those from-ranges just before CollectRegion scrub?
//   (3) Does ForwardRegion re-record any remset at to-object slots for old holders? (code: no)

namespace O2ORemsetDiag {

bool Enabled();

// Called from ForwardRegion after a successful object copy on a non-young from-region.
void NoteOldObjectForward(BaseObject* fromObj, BaseObject* toObj, size_t size);

// Positive control: young from-region object copy (same VisitLiveObjects arm, young path).
void NoteYoungObjectForward();

// Called once at end of ForwardRegion success path for a non-young region, before CollectRegion.
// remsetInFrom: count of remset slots whose addresses fall in [regionStart, regionEnd).
void NoteOldRegionForwarded(RegionInfo* region, size_t remsetInFrom, size_t liveObjectsForwarded,
                            size_t o2yEdgesOnToObj);

// Called from ScrubRememberedSetForRegion when probe on and region is non-young.
void NoteScrubNonYoung(RegionInfo* region, size_t scrubbed);

// Emit + optionally reset process counters (call at GC cycle boundaries).
void DumpAndMaybeReset(const char* point, bool reset);

} // namespace O2ORemsetDiag
} // namespace MapleRuntime

#endif // MRT_O2O_REMSET_DIAG_H
