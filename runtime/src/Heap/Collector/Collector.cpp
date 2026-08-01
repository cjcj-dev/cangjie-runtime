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
BaseObject* Collector::FindLatestVersion(BaseObject* obj) const
{
    if (obj == nullptr) {
        return nullptr;
    }

    BaseObject* to = FindToVersion(obj);
    if (to != nullptr) {
        return to;
    }
    CHECK_DETAIL(obj->IsValidObject(),
                 "FindLatestVersion: no to-version for invalid from-object %p "
                 "(stale old-tag after ghost dispel; do not fall back to from)",
                 obj);
    return obj;
}

const char* Collector::GetGCPhaseName(GCPhase phase)
{
    static const char* phaseNames[] = {
        "undefined phase", // 0
        "idle phase",      // 1
        "finish phase",    // 2
        "reclaim satb phase", // 3
        "stub phase",      // 4
        "stub phase",      // 5
        "stub phase",      // 6
        "stub phase",      // 7
        "init phase",      // 8
        "enum phase",      // 9
        "trace phase",     // 10
        "clear satb phase", // 11
        "post trace phase", // 12
        "preforward phase", // 13
        "forward phase",   // 14
        "force barrier phase", // 15 GC_PHASE_FORCE_BARRIER
    };
    return phaseNames[phase];
}

Collector::Collector() {}

const char* Collector::GetCollectorName() const { return COLLECTOR_NAME[collectorType]; }

void Collector::RequestGC(GCReason reason, bool async) { RequestGCInternal(reason, async); }
} // namespace MapleRuntime.
