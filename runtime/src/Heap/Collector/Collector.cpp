// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Collector/Collector.h"

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
namespace {
const char* const COLLECTOR_NAME[] = { "No Collector", "Proxy Collector", "Regional-Copying Collector",
                                       "Smooth Collector" };

// nonnormal lane: read-only dump at empty-route reject (does not change the predicate).
void DumpNonNormalForensic(BaseObject* obj, GCPhase phase)
{
    const unsigned st = static_cast<unsigned>(obj->GetObjectState().GetStateCode());
    const int valid = obj->IsValidObject() ? 1 : 0;
    unsigned ghost = 0;
    unsigned rtype = 255;
    unsigned young = 0;
    unsigned route = 255;
    unsigned from = 0;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (RegionInfo::InGhostFromRegion(obj)) {
        ghost = 1;
    }
    RegionInfo* ghostRegion = RegionInfo::GetGhostFromRegionAt(addr);
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(addr);
    if (region != nullptr) {
        rtype = static_cast<unsigned>(region->GetRegionType());
        young = region->IsYoungRegion() ? 1 : 0;
        route = static_cast<unsigned>(region->GetRouteState());
        from = region->IsFromRegion() ? 1 : 0;
    }
    // st: 0=NORMAL 1=LOCKED 2=FORWARDING 3=FORWARDED
    LOG(RTLOG_ERROR,
        "NONNORMAL_FORENSIC obj=%p st=%u valid=%d phase=%u ghost=%u ghostReg=%p rtype=%u young=%u "
        "from=%u route=%u",
        obj, st, valid, static_cast<unsigned>(phase), ghost, ghostRegion, rtype, young, from, route);
}
}

// F5: when FindToVersion returns null, never silently hand back a dead/zeroed from.
// Legal null (high-live / raw-pin survivor still at from, ghost=0) keeps returning obj.
// Illegal null (D: old tag + ghost already dispelled + from cleared) fails loudly here.
// See reports/REPORT-nullenum.md LEGAL_NULL_SET; reports/REPORT-tagaba.md F5.
// fixvalid F1: IsValidObject ≡ TypeInfo!=nullptr only (StateWord.h:116) — it does not
// reject FORWARDED/LOCKED/FORWARDING. Empty-route fallback must also require NORMAL
// (LEGAL_NULL_SET: in-place survivor). Do not change IsValidObject itself.
BaseObject* Collector::FindLatestVersion(BaseObject* obj) const
{
    if (obj == nullptr) {
        return nullptr;
    }

    BaseObject* to = FindToVersion(obj);
    if (to != nullptr) {
        return to;
    }
    if (!(obj->IsValidObject() &&
          obj->GetObjectState().GetStateCode() == ObjectState::NORMAL)) {
        DumpNonNormalForensic(obj, GetGCPhase());
    }
    CHECK_DETAIL(obj->IsValidObject() &&
                     obj->GetObjectState().GetStateCode() == ObjectState::NORMAL,
                 "FindLatestVersion: no to-version for non-NORMAL/invalid from-object %p "
                 "(stale old-tag after ghost dispel or mid-forward; do not fall back to from)",
                 obj);
    return obj;
}

const char* Collector::GetGCPhaseName(GCPhase phase)
{
    static const char* phaseNames[] = {
        "undefined phase", // 0
        "idle phase",      // 1
        "finish phase",    "reclaim satb phase",
        "stub phase",      "stub phase",
        "stub phase",      "stub phase",
        "init phase",      "enum phase",
        "trace phase",     "clear satb phase",
        "forward phase",   "enum fix phase",
        "trace fix phase", "clear trace fix phase",
        "fix stack phase", "preforward phase",
    };
    return phaseNames[phase];
}

Collector::Collector() {}

const char* Collector::GetCollectorName() const { return COLLECTOR_NAME[collectorType]; }

void Collector::RequestGC(GCReason reason, bool async) { RequestGCInternal(reason, async); }
} // namespace MapleRuntime.
