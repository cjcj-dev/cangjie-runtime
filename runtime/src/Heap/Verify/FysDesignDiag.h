// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FYS_DESIGN_DIAG_H
#define MRT_FYS_DESIGN_DIAG_H

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "Common/TypeDef.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class BaseObject;

// fysdesign: census edges FYS mark discovers on claimed holders that remset does not
// contain. Answers "what is FYS buying vs remset".
//
// Gate (default off; product early-return before any walk):
//   MRT_GCV2_FYSDESIGN=1  OR  MRT_GCV2_DIAG contains fysdesign|all
// Sample log cap: MRT_GCV2_FYSDESIGN_SAMPLES=<N> (default 8)
//
// Call after first TraceYoungClosure (roots pass) while remset drain set is still
// the pre-mark rememberedSlots. Under FYS=0 the census still runs but claim_old=0
// so FYS-only O→Y should be ~0 by construction.

namespace FysDesignDiag {

bool Enabled();

void OnMinorBegin(size_t minorRunIndex);

// Walk claimed holders; classify ref edges vs remset membership.
// resolve: soft field resolve (ResolveMinorReference) — must not throw.
void Census(const std::vector<BaseObject*>& reachableVec,
            const std::unordered_set<MAddress>& rememberedSlots, bool fullYoungScan,
            BaseObject* (*resolve)(RefField<>& field));

void Report(const char* tag);

} // namespace FysDesignDiag
} // namespace MapleRuntime

#endif // MRT_FYS_DESIGN_DIAG_H
