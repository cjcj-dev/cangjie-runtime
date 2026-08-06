// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Collector/Collector.h"

#include <atomic>
#include <cstring>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
namespace {
const char* const COLLECTOR_NAME[] = { "No Collector", "Proxy Collector", "Regional-Copying Collector",
                                       "Smooth Collector" };

// zc7fix: is_mark_good fast path may admit plain non-heap slots (g_cjMarkBadMask all-zero on
// uncoloured non-null). Count rejects before IsValidObject/IsMarkedObject.
std::atomic<size_t> g_markGoodHeapGateReject{ 0 };
std::atomic<size_t> g_markGoodHeapGateSample{ 0 };

bool MarkGoodHeapGateAccountOn()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_MARKGOOD_HEAP_GATE");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}
} // namespace

bool Collector::MarkGoodHeapGate(const char* site, BaseObject* target)
{
    if (Heap::IsHeapAddress(target)) {
        return true;
    }
    size_t n = g_markGoodHeapGateReject.fetch_add(1, std::memory_order_relaxed) + 1;
    if (MarkGoodHeapGateAccountOn()) {
        size_t s = g_markGoodHeapGateSample.fetch_add(1, std::memory_order_relaxed);
        if (s < 8) {
            VLOG(REPORT, "[GCV2][markgood-heap-gate] REJECT site=%s target=%p n=%zu", site, target, n);
        }
    }
    return false;
}

void Collector::ReportMarkGoodHeapGateCounts()
{
    if (!MarkGoodHeapGateAccountOn()) {
        return;
    }
    VLOG(REPORT, "[GCV2][markgood-heap-gate] reject=%zu env=MRT_GCV2_MARKGOOD_HEAP_GATE=1",
         g_markGoodHeapGateReject.load(std::memory_order_relaxed));
}

// F5: when FindToVersion returns null, never silently hand back a dead/zeroed from.
// Legal null (high-live / raw-pin survivor still at from, ghost=0) keeps returning obj.
// Illegal null (D: old tag + ghost already dispelled + from cleared) fails loudly here.
// See reports/REPORT-nullenum.md LEGAL_NULL_SET; reports/REPORT-tagaba.md F5.
// Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94.
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
