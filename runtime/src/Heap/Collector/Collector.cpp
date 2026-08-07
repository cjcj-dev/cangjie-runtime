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
#include "Common/StateWord.h"
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

// markfloor: interiors pass IsHeapAddress + IsValidObject (tip word non-null) but tip is
// not a TypeInfo*. 0x200 observed = MArray::length at RawArray+8.
std::atomic<size_t> g_plausibleObjGateReject{ 0 };
std::atomic<size_t> g_plausibleObjGateSample{ 0 };
// interiorsrc2: per-site reject counters (always on when gate accounts).
std::atomic<size_t> g_plausibleObjGateBySite[16]{ {} };

bool MarkGoodHeapGateAccountOn()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_MARKGOOD_HEAP_GATE");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

bool PlausibleObjGateAccountOn()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_MARKFLOOR_OBJ_GATE");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

// Smallest plausible TypeInfo / binary address. Catches length/size/offset words
// (e.g. 0x200) and null-page interiors without touching tip payload.
constexpr uintptr_t kMinPlausibleTypeInfoAddr = 0x10000ULL;

// interiorsrc2: classify tip word without calling IsVaildType (may SEGV on bad tip).
bool TipWordLooksLikeTypeInfo(uintptr_t tipAddr)
{
    if (tipAddr == 0 || tipAddr < kMinPlausibleTypeInfoAddr) {
        return false;
    }
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return false;
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    return true;
}

// If obj is interior into a managed object, return offset (8/16/24/32) else 0.
// Only peeks tip at obj-k; never walks payload.
unsigned ClassifyInteriorOffset(BaseObject* obj)
{
    auto base = reinterpret_cast<uintptr_t>(obj);
    for (unsigned k : { 8u, 16u, 24u, 32u }) {
        if (base < k) {
            continue;
        }
        auto* cand = reinterpret_cast<BaseObject*>(base - k);
        if (!Heap::IsHeapAddress(cand)) {
            continue;
        }
        // Safe: tip is first word; heap address already checked.
        uintptr_t tipAddr = reinterpret_cast<uintptr_t>(cand->GetTypeInfo());
        if (TipWordLooksLikeTypeInfo(tipAddr)) {
            return k;
        }
    }
    return 0;
}

unsigned SiteBucket(const char* site)
{
    if (site == nullptr) {
        return 15;
    }
    if (std::strstr(site, "MarkObject") != nullptr) {
        return 0;
    }
    if (std::strstr(site, "TraceRefField") != nullptr) {
        return 1;
    }
    if (std::strstr(site, "EnumRefField") != nullptr) {
        return 2;
    }
    if (std::strstr(site, "EnumAndTagRawRoot") != nullptr) {
        return 3;
    }
    if (std::strstr(site, "ForwardUpdateRawRef") != nullptr) {
        return 4;
    }
    if (std::strstr(site, "ForwardObjectExclusive") != nullptr) {
        return 7;
    }
    if (std::strstr(site, "TryForward") != nullptr) {
        return 6;
    }
    if (std::strstr(site, "ForwardObject") != nullptr) {
        return 5;
    }
    if (std::strstr(site, "TraceYoungClosure") != nullptr) {
        return 8;
    }
    if (std::strstr(site, "PushYoungObject") != nullptr) {
        return 9;
    }
    return 14;
}
} // namespace

bool Collector::MarkGoodHeapGate(const char* site, BaseObject* target)
{
    if (Heap::IsHeapAddress(target)) {
        return true;
    }
    size_t n = g_markGoodHeapGateReject.fetch_add(1, std::memory_order_relaxed) + 1;
    // Always count; samples use RTLOG_ERROR so they survive default log level and abrupt exit.
    if (MarkGoodHeapGateAccountOn()) {
        size_t s = g_markGoodHeapGateSample.fetch_add(1, std::memory_order_relaxed);
        if (s < 8) {
            LOG(RTLOG_ERROR, "[GCV2][markgood-heap-gate] REJECT site=%s target=%p n=%zu", site, target, n);
        }
    }
    return false;
}

void Collector::ReportMarkGoodHeapGateCounts()
{
    if (!MarkGoodHeapGateAccountOn()) {
        return;
    }
    LOG(RTLOG_ERROR, "[GCV2][markgood-heap-gate] reject=%zu env=MRT_GCV2_MARKGOOD_HEAP_GATE=1",
        g_markGoodHeapGateReject.load(std::memory_order_relaxed));
}

bool Collector::PlausibleManagedObjectGate(const char* site, BaseObject* obj)
{
    if (obj == nullptr) {
        return false;
    }
    if (!Heap::IsHeapAddress(obj)) {
        size_t n = g_plausibleObjGateReject.fetch_add(1, std::memory_order_relaxed) + 1;
        g_plausibleObjGateBySite[SiteBucket(site)].fetch_add(1, std::memory_order_relaxed);
        if (PlausibleObjGateAccountOn()) {
            size_t s = g_plausibleObjGateSample.fetch_add(1, std::memory_order_relaxed);
            if (s < 32) {
                GCPhase phase = Heap::GetHeap().GetGCPhase();
                LOG(RTLOG_ERROR,
                    "[GCV2][markfloor-obj-gate] REJECT site=%s obj=%p reason=non-heap n=%zu phase=%s(%u) "
                    "ra0=%p ra1=%p ra2=%p",
                    site, obj, n, Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase),
                    __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
            }
        }
        return false;
    }
    // Read tip word only (StateWord load). Do not call IsVaildType / GetSize yet.
    TypeInfo* tip = obj->GetTypeInfo();
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    const char* reason = nullptr;
    if (tipAddr == 0) {
        reason = "null-tip";
    } else if (tipAddr < kMinPlausibleTypeInfoAddr) {
        reason = "tip-small-int";
    } else if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        reason = "tip-misaligned";
    } else if (Heap::IsHeapAddress(tipAddr)) {
        // TypeInfo lives in binary / TypeInfoManager mmap, never in managed heap.
        // Heap tip ⇒ interior into another object (classic B-4 shape).
        reason = "tip-in-heap";
    }
    if (reason == nullptr) {
        return true;
    }
    size_t n = g_plausibleObjGateReject.fetch_add(1, std::memory_order_relaxed) + 1;
    g_plausibleObjGateBySite[SiteBucket(site)].fetch_add(1, std::memory_order_relaxed);
    if (PlausibleObjGateAccountOn()) {
        size_t s = g_plausibleObjGateSample.fetch_add(1, std::memory_order_relaxed);
        if (s < 48) {
            unsigned off = ClassifyInteriorOffset(obj);
            GCPhase phase = Heap::GetHeap().GetGCPhase();
            // tip-small-int + off=8 ⇒ classic RawArray+8 / &MArray::length.
            LOG(RTLOG_ERROR,
                "[GCV2][markfloor-obj-gate] REJECT site=%s obj=%p tip=%p reason=%s n=%zu "
                "int_off=%u phase=%s(%u) ra0=%p ra1=%p ra2=%p",
                site, obj, tip, reason, n, off, Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase),
                __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
        }
    }
    return false;
}

void Collector::ReportPlausibleManagedObjectGateCounts()
{
    if (!PlausibleObjGateAccountOn()) {
        return;
    }
    LOG(RTLOG_ERROR, "[GCV2][markfloor-obj-gate] reject=%zu env=MRT_GCV2_MARKFLOOR_OBJ_GATE=1",
        g_plausibleObjGateReject.load(std::memory_order_relaxed));
    LOG(RTLOG_ERROR,
        "[GCV2][markfloor-obj-gate] bysite MarkObject=%zu TraceRefField=%zu EnumRefField=%zu "
        "EnumAndTagRawRoot=%zu ForwardUpdateRawRef=%zu ForwardObject=%zu TryForward=%zu "
        "ForwardObjectExclusive=%zu TraceYoungClosure=%zu PushYoungObject=%zu other=%zu",
        g_plausibleObjGateBySite[0].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[1].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[2].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[3].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[4].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[5].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[6].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[7].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[8].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[9].load(std::memory_order_relaxed),
        g_plausibleObjGateBySite[14].load(std::memory_order_relaxed));
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
