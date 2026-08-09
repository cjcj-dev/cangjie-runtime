// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FLOOR_ENUM_DIAG_H
#define MRT_FLOOR_ENUM_DIAG_H

// floorenum + floortarget: four-column host recon + target lifecycle on nullroute.
// Gate: MRT_GCV2_FLOORENUM_DIAG=1 (default off).
// Indep full-root: MRT_GCV2_FLOORENUM_INDEP=1 every MRT_GCV2_FLOORENUM_EVERY (default 4).
// Target grant recon always on when FLOORENUM_DIAG=1 (no product path change).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

#include "Common/BaseObject.h"
#include "Common/TypeDef.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace FloorEnumDiag {

bool DiagEnabled();
bool IndepEnabled();
size_t EveryN();

void ClearSnap();

void CapturePreEvacuate(
    size_t minorIndex, const std::vector<BaseObject*>& reachableVec,
    const std::unordered_set<MAddress>& remsetSlots,
    const std::function<void(const std::function<void(BaseObject*)>&)>& visitRoots,
    const std::function<BaseObject*(RefField<>&)>& resolveField);

void NoteCrossGen(bool recorded);

// Optional: call just before PrepareForwardableRegion walks from-list (not required).
void NotePreForwardSnap(size_t fromRegions, size_t markedYoungSample);

void LogNullRouteSample(BaseObject* fromObj, BaseObject* hostObj, uintptr_t slotAddr,
                        const char* edgeSrc, const char* caller);

} // namespace FloorEnumDiag
} // namespace MapleRuntime

#endif // MRT_FLOOR_ENUM_DIAG_H
