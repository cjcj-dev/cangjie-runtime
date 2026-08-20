// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// survnode: split markport §6.1 SurvivalNode deadFrom into frame (a) vs (b).
//
// (a) the slot was stored after mark-end, when SATB / TRACE no longer covers it
//     (zBarrier.inline.hpp:59-67 store-good monotonicity; ZGC store after mark-end
//     is store-good and the new referent is already marked).
// (b) the target was painted this cycle then the mark face was cleared without an
//     epoch bump (ZGC flip_mark_start, zGeneration.cpp:1074-1077, is the only
//     reset; CheckAndClearLiveInfo / NullLiveInfoFieldsInRange null liveInfo
//     without bumping snapshot epoch).
//
// Gated with MarkCompleteVerify. Default off. No new MRT_GCV2_ env.

#ifndef MRT_SURV_NODE_DIAG_H
#define MRT_SURV_NODE_DIAG_H

#include <cstdint>

namespace MapleRuntime {

class BaseObject;
class RegionInfo;

namespace SurvNodeDiag {

bool Enabled();

enum StoreSite : uint8_t {
    STORE_WRITE_REF = 1,
    STORE_ATOMIC_WRITE = 2,
    STORE_CAS = 3,
    STORE_SWAP = 4,
    STORE_COPY_REF = 5,
    STORE_WRITE_STATIC = 6,
};

enum ClearSite : uint8_t {
    CLEAR_LIVE_INFO = 1,
    CLEAR_CHECK_AND_CLEAR = 2,
    CLEAR_NULL_IN_RANGE = 3,
    CLEAR_RESET_MARK_BIT = 4,
};

void NoteStore(const void* slot, BaseObject* pre, BaseObject* neu, uint8_t site);
void NotePaint(BaseObject* obj, RegionInfo* region);
void NoteClear(RegionInfo* region, uint8_t site, bool epochBumped);

void ReportOnDeadEdge(BaseObject* holder, void* slot, BaseObject* target, RegionInfo* targetRegion);
void ReportAtMarkEnd(const char* point);

} // namespace SurvNodeDiag
} // namespace MapleRuntime

#endif
