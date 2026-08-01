// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Collector/Collector.h"

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
namespace {
const char* const COLLECTOR_NAME[] = { "No Collector", "Proxy Collector", "Regional-Copying Collector",
                                       "Smooth Collector" };
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
