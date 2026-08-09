// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FLOOR_ENUM_DIAG_H
#define MRT_FLOOR_ENUM_DIAG_H

// floorenum: four-column mark/fix host reconciliation on nullroute.
// Gate: MRT_GCV2_FLOORENUM_DIAG=1 (default off).
// Indep full-root: MRT_GCV2_FLOORENUM_INDEP=1 every MRT_GCV2_FLOORENUM_EVERY (default 4).

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

void LogNullRouteSample(BaseObject* fromObj, BaseObject* hostObj, uintptr_t slotAddr,
                        const char* edgeSrc, const char* caller);

} // namespace FloorEnumDiag
} // namespace MapleRuntime

#endif // MRT_FLOOR_ENUM_DIAG_H
