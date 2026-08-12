// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/PromotedRegionDomain.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/GcRequest.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace PromotedRegionDomain {
namespace {

struct Entry {
    RegionInfo* region = nullptr;
    RegisterPath path = RegisterPath::InPlace;
    bool discharged = false;
};

std::mutex g_mu;
std::vector<Entry> g_entries;
std::unordered_set<RegionInfo*> g_registered;

// Dual-run edge sets for one promote phase (cleared at discharge / reset).
std::unordered_set<MAddress> g_oldSlots;
std::unordered_set<MAddress> g_domainSlots;

std::atomic<uint64_t> g_registerCalls{ 0 };
std::atomic<uint64_t> g_registerDup{ 0 };
std::atomic<uint64_t> g_dischargeCalls{ 0 };
std::atomic<uint64_t> g_dischargeEdges{ 0 };
std::atomic<uint64_t> g_storeGoodEarly{ 0 };
std::atomic<uint64_t> g_lastDischargeNs{ 0 };
std::atomic<uint64_t> g_lastOld{ 0 };
std::atomic<uint64_t> g_lastDomain{ 0 };
std::atomic<uint64_t> g_lastOldOnly{ 0 };
std::atomic<uint64_t> g_lastDomainOnly{ 0 };
std::atomic<uint64_t> g_reconcileMismatchCycles{ 0 };
std::atomic<uint64_t> g_skipOneFired{ 0 };
std::atomic<uint64_t> g_injectUndischarged{ 0 };

// domainon coverage: per-reason Record/Register (site 0..3).
constexpr size_t kReasonN = static_cast<size_t>(GC_REASON_MAX);
constexpr size_t kSiteN = 4;
std::atomic<uint64_t> g_recCalls[kReasonN][kSiteN]{};
std::atomic<uint64_t> g_recEdges[kReasonN][kSiteN]{};
std::atomic<uint64_t> g_regYes[kReasonN][kSiteN]{};
std::atomic<uint64_t> g_regNo[kReasonN][kSiteN]{};

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

const char* PathName(RegisterPath p)
{
    switch (p) {
        case RegisterPath::InPlace:
            return "inplace";
        case RegisterPath::Abandon:
            return "abandon";
        case RegisterPath::Residual:
            return "residual";
        default:
            return "unknown";
    }
}

// Match RecordPromotedCrossGenEdges liveness gate (RegionManager.cpp:266-289).
bool UseLiveOnly(RegionInfo* region)
{
    bool hasObjectLiveness = region->IsLargeRegion() || region->GetMarkBitmap() != nullptr ||
        region->GetResurrectBitmap() != nullptr;
    return hasObjectLiveness && region->IsLiveCountAuthoritative();
}

bool ObjectSurvived(RegionInfo* region, BaseObject* object, bool hasObjectLiveness)
{
    return hasObjectLiveness &&
        region->IsSurvivedObject(region->GetAddressOffset(reinterpret_cast<MAddress>(object)));
}

size_t DischargedCountUnlocked()
{
    size_t n = 0;
    for (const Entry& e : g_entries) {
        if (e.discharged) {
            ++n;
        }
    }
    return n;
}

void DiffAndStore(size_t minorRunIndex, const char* tag)
{
    // Caller holds g_mu.
    size_t oldOnly = 0;
    size_t domainOnly = 0;
    for (MAddress s : g_oldSlots) {
        if (g_domainSlots.count(s) == 0) {
            ++oldOnly;
        }
    }
    for (MAddress s : g_domainSlots) {
        if (g_oldSlots.count(s) == 0) {
            ++domainOnly;
        }
    }
    g_lastOld.store(g_oldSlots.size(), std::memory_order_relaxed);
    g_lastDomain.store(g_domainSlots.size(), std::memory_order_relaxed);
    g_lastOldOnly.store(oldOnly, std::memory_order_relaxed);
    g_lastDomainOnly.store(domainOnly, std::memory_order_relaxed);
    if (oldOnly != 0 || domainOnly != 0) {
        g_reconcileMismatchCycles.fetch_add(1, std::memory_order_relaxed);
    }
    size_t discharged = DischargedCountUnlocked();
    VLOG(REPORT,
         "[PROMODOMAIN][RECONCILE] tag=%s run=%zu old=%zu domain=%zu oldOnly=%zu domainOnly=%zu "
         "registered=%zu discharged=%zu skipOne=%llu inject=%llu env=MRT_GCV2_PROMO_DOMAIN_RECONCILE",
         tag, minorRunIndex, g_oldSlots.size(), g_domainSlots.size(), oldOnly, domainOnly, g_entries.size(),
         discharged, static_cast<unsigned long long>(g_skipOneFired.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_injectUndischarged.load(std::memory_order_relaxed)));
    if (FatalOnMismatch() && (oldOnly != 0 || domainOnly != 0)) {
        CHECK_DETAIL(false,
                     "[PROMODOMAIN] reconcile mismatch oldOnly=%zu domainOnly=%zu (MRT_GCV2_PROMO_DOMAIN_FATAL=1)",
                     oldOnly, domainOnly);
    }
}

} // namespace

bool Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_PROMO_DOMAIN");
    return on;
}

bool ReconcileEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_PROMO_DOMAIN_RECONCILE");
    return on;
}

bool FatalOnMismatch()
{
    static const bool on = EnvIsOne("MRT_GCV2_PROMO_DOMAIN_FATAL");
    return on;
}

// Dual-run snapshot: capture domain edge set at Register (still young), same moment as
// RecordPromotedCrossGenEdges. Discharge may run after demote when targets no longer
// look young — set equivalence must use promote-time slots (ZGC flip-promote remset
// also sees the page before age flip completes).
void SnapshotDomainEdgesAtRegister(RegionInfo* region)
{
    // Caller holds g_mu when ReconcileEnabled.
    if (!ReconcileEnabled() || region == nullptr || region->IsSafeKnownEmpty()) {
        return;
    }
    static const bool skipOne = EnvIsOne("MRT_GCV2_PROMO_DOMAIN_SKIP_ONE");
    bool hasObjectLiveness = region->IsLargeRegion() || region->GetMarkBitmap() != nullptr ||
        region->GetResurrectBitmap() != nullptr;
    bool useLiveOnly = UseLiveOnly(region);
    region->VisitAllObjects([&](BaseObject* object) {
        if (object == nullptr || !object->HasRefField()) {
            return;
        }
        if (useLiveOnly && !ObjectSurvived(region, object, hasObjectLiveness)) {
            return;
        }
        object->ForEachRefField([&](RefField<>& field) {
            BaseObject* target = to_object(field.GetTargetObject());
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                return;
            }
            RegionInfo* tr = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (tr == nullptr || !tr->IsYoungRegion()) {
                return;
            }
            MAddress slot = reinterpret_cast<MAddress>(&field);
            if (skipOne && g_skipOneFired.load(std::memory_order_relaxed) == 0) {
                g_skipOneFired.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            g_domainSlots.insert(slot);
        });
    });
}

void Register(RegionInfo* region, RegisterPath path)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    g_registerCalls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lg(g_mu);
    // zRelocationSet.cpp:205-211 shape: reject duplicate page.
    if (g_registered.count(region) != 0) {
        g_registerDup.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Entry e;
    e.region = region;
    e.path = path;
    e.discharged = false;
    g_entries.push_back(e);
    g_registered.insert(region);
    // Promote-time domain edge snapshot for dual-run (before demote clears young).
    SnapshotDomainEdgesAtRegister(region);
    DLOG(REGION, "[PROMODOMAIN] register region=%p path=%s n=%zu", region, PathName(path), g_entries.size());
}

bool IsRegisteredUndischargedUnlocked(const RegionInfo* region)
{
    if (region == nullptr || g_registered.count(const_cast<RegionInfo*>(region)) == 0) {
        return false;
    }
    for (const Entry& e : g_entries) {
        if (e.region == region && !e.discharged) {
            return true;
        }
    }
    return false;
}

bool IsRegisteredUndischarged(const RegionInfo* region)
{
    if (!Enabled() || region == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lg(g_mu);
    return IsRegisteredUndischargedUnlocked(region);
}

void CheckNotUndischargedForReuse(const RegionInfo* region, const char* site)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lg(g_mu);
        if (!IsRegisteredUndischargedUnlocked(region)) {
            return;
        }
    }
    LOG(RTLOG_FATAL,
        "[PROMODOMAIN] undischarged region reused site=%s region=%p — obligation① "
        "(registered must discharge before TakeRegion/ClearUnits)",
        site != nullptr ? site : "?", region);
}

void NoteOldProductRecord(MAddress slot)
{
    if (!Enabled() || !ReconcileEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lg(g_mu);
    g_oldSlots.insert(slot);
}

void NoteRecordCall(uint32_t reason, uint8_t site, size_t edges)
{
    if (reason >= kReasonN || site >= kSiteN) {
        return;
    }
    g_recCalls[reason][site].fetch_add(1, std::memory_order_relaxed);
    g_recEdges[reason][site].fetch_add(static_cast<uint64_t>(edges), std::memory_order_relaxed);
}

void NoteRegisterGate(uint32_t reason, uint8_t site, bool registered)
{
    if (reason >= kReasonN || site >= kSiteN) {
        return;
    }
    if (registered) {
        g_regYes[reason][site].fetch_add(1, std::memory_order_relaxed);
    } else {
        g_regNo[reason][site].fetch_add(1, std::memory_order_relaxed);
    }
}

size_t DischargeAll(const std::function<BaseObject*(RefField<>&)>& resolve,
                    const std::function<bool(RefField<>&)>& isStoreGood,
                    const std::function<void(RefField<>&, BaseObject*)>& colorStoreGood,
                    const std::function<void(MAddress)>& recordSlot)
{
    if (!Enabled()) {
        return 0;
    }
    g_dischargeCalls.fetch_add(1, std::memory_order_relaxed);
    static const bool injectUndischarged = EnvIsOne("MRT_GCV2_PROMO_DOMAIN_INJECT_UNDISCHARGED");

    auto t0 = std::chrono::steady_clock::now();
    size_t recorded = 0;

    std::lock_guard<std::mutex> lg(g_mu);
    for (Entry& e : g_entries) {
        if (e.discharged || e.region == nullptr) {
            continue;
        }
        RegionInfo* region = e.region;
        if (region->IsSafeKnownEmpty()) {
            e.discharged = true;
            continue;
        }
        bool hasObjectLiveness = region->IsLargeRegion() || region->GetMarkBitmap() != nullptr ||
            region->GetResurrectBitmap() != nullptr;
        bool useLiveOnly = UseLiveOnly(region);

        // Product remset heal (ZGC remap_and_maybe_add_remset). Dual-run edge sets were
        // snapshotted at Register; here we still walk for Record + store-good colour.
        region->VisitAllObjects([&](BaseObject* object) {
            if (object == nullptr || !object->HasRefField()) {
                return;
            }
            if (useLiveOnly && !ObjectSurvived(region, object, hasObjectLiveness)) {
                return;
            }
            object->ForEachRefField([&](RefField<>& field) {
                MAddress slot = reinterpret_cast<MAddress>(&field);
                if (isStoreGood(field)) {
                    g_storeGoodEarly.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                BaseObject* target = resolve(field);
                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                    return;
                }
                RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                if (targetRegion == nullptr || !targetRegion->IsYoungRegion()) {
                    return;
                }
                recordSlot(slot);
                colorStoreGood(field, target);
                ++recorded;
            });
        });
        if (!(injectUndischarged && e.region == g_entries.front().region)) {
            e.discharged = true;
        } else {
            g_injectUndischarged.fetch_add(1, std::memory_order_relaxed);
            VLOG(REPORT,
                 "[PROMODOMAIN][INJECT] left undischarged region=%p path=%s (positive control)",
                 e.region, PathName(e.path));
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    g_lastDischargeNs.store(ns, std::memory_order_relaxed);
    g_dischargeEdges.fetch_add(recorded, std::memory_order_relaxed);

    if (ReconcileEnabled()) {
        DiffAndStore(0, "post-discharge");
    }
    size_t tableBytes = g_entries.size() * sizeof(Entry) + g_registered.size() * sizeof(void*) +
        (g_oldSlots.size() + g_domainSlots.size()) * sizeof(MAddress);
    VLOG(REPORT,
         "[PROMODOMAIN][DISCHARGE] edges=%zu storeGoodEarly=%llu ns=%llu registered=%zu "
         "tableBytes≈%zu",
         recorded, static_cast<unsigned long long>(g_storeGoodEarly.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(ns), g_entries.size(), tableBytes);
    return recorded;
}

void ResetForNextMinor(size_t minorRunIndex)
{
    if (!Enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lg(g_mu);
    size_t registered = g_entries.size();
    size_t discharged = 0;
    for (const Entry& e : g_entries) {
        if (e.discharged) {
            ++discharged;
        }
    }
    // Obligation ②: registered == discharged before clear (zGeneration reset_relocation_set).
    if (registered != discharged) {
        LOG(RTLOG_ERROR,
            "[PROMODOMAIN][INVARIANT] run=%zu registered=%zu discharged=%zu — not equal before reset",
            minorRunIndex, registered, discharged);
        if (FatalOnMismatch() || !EnvIsOne("MRT_GCV2_PROMO_DOMAIN_INJECT_UNDISCHARGED")) {
            CHECK_DETAIL(registered == discharged,
                         "[PROMODOMAIN] registered==discharged failed run=%zu reg=%zu dis=%zu",
                         minorRunIndex, registered, discharged);
        } else {
            VLOG(REPORT,
                 "[PROMODOMAIN][INVARIANT] inject arm: non-fatal mismatch run=%zu reg=%zu dis=%zu",
                 minorRunIndex, registered, discharged);
        }
    } else if (registered != 0) {
        VLOG(REPORT,
             "[PROMODOMAIN][INVARIANT] run=%zu registered=%zu discharged=%zu OK",
             minorRunIndex, registered, discharged);
    }
    g_entries.clear();
    g_registered.clear();
    g_oldSlots.clear();
    g_domainSlots.clear();
}

void DumpReconcile(size_t minorRunIndex, const char* tag)
{
    if (!Enabled() || !ReconcileEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lg(g_mu);
    DiffAndStore(minorRunIndex, tag != nullptr ? tag : "dump");
}

void DumpProcessTotals(const char* tag)
{
    if (!Enabled()) {
        return;
    }
    VLOG(REPORT,
         "[PROMODOMAIN][TOTALS] tag=%s regCalls=%llu regDup=%llu dischargeCalls=%llu "
         "dischargeEdges=%llu storeGoodEarly=%llu lastNs=%llu lastOld=%llu lastDomain=%llu "
         "lastOldOnly=%llu lastDomainOnly=%llu mismatchCycles=%llu skipOne=%llu inject=%llu",
         tag != nullptr ? tag : "?",
         static_cast<unsigned long long>(g_registerCalls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_registerDup.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_dischargeCalls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_dischargeEdges.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_storeGoodEarly.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_lastDischargeNs.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_lastOld.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_lastDomain.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_lastOldOnly.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_lastDomainOnly.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_reconcileMismatchCycles.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipOneFired.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_injectUndischarged.load(std::memory_order_relaxed)));
    DumpCoverageByReason(tag);
}

void DumpCoverageByReason(const char* tag)
{
    // Always-on counters (cheap atomic); dump even when domain off so product path
    // can prove Record site×reason without forcing PROMO_DOMAIN=1.
    static const char* kSiteName[kSiteN] = { "inplace", "abandon", "residual", "other" };
    uint64_t recNonYoung = 0;
    uint64_t regYesNonYoung = 0;
    uint64_t regNoAny = 0;
    for (size_t r = 0; r < kReasonN; ++r) {
        uint64_t recC = 0;
        uint64_t recE = 0;
        uint64_t yes = 0;
        uint64_t no = 0;
        for (size_t s = 0; s < kSiteN; ++s) {
            uint64_t c = g_recCalls[r][s].load(std::memory_order_relaxed);
            uint64_t e = g_recEdges[r][s].load(std::memory_order_relaxed);
            uint64_t y = g_regYes[r][s].load(std::memory_order_relaxed);
            uint64_t n = g_regNo[r][s].load(std::memory_order_relaxed);
            if (c == 0 && e == 0 && y == 0 && n == 0) {
                continue;
            }
            const char* rname = r < GC_REASON_MAX ? g_gcRequests[r].name : "invalid";
            VLOG(REPORT,
                 "[PROMODOMAIN][COVER] tag=%s reason=%s(%zu) site=%s recCalls=%llu recEdges=%llu "
                 "regYes=%llu regNo=%llu",
                 tag != nullptr ? tag : "?", rname, r, kSiteName[s],
                 static_cast<unsigned long long>(c), static_cast<unsigned long long>(e),
                 static_cast<unsigned long long>(y), static_cast<unsigned long long>(n));
            recC += c;
            recE += e;
            yes += y;
            no += n;
        }
        if (r != static_cast<size_t>(GC_REASON_YOUNG)) {
            recNonYoung += recC;
            regYesNonYoung += yes;
        }
        regNoAny += no;
    }
    VLOG(REPORT,
         "[PROMODOMAIN][COVERSUM] tag=%s recCallsNonYoung=%llu regYesNonYoung=%llu regNoAny=%llu "
         "gap=(rec without reg gate) see COVER lines",
         tag != nullptr ? tag : "?",
         static_cast<unsigned long long>(recNonYoung),
         static_cast<unsigned long long>(regYesNonYoung),
         static_cast<unsigned long long>(regNoAny));
}

size_t RegisteredCount()
{
    std::lock_guard<std::mutex> lg(g_mu);
    return g_entries.size();
}

size_t DischargedCount()
{
    std::lock_guard<std::mutex> lg(g_mu);
    return DischargedCountUnlocked();
}

size_t LastDischargeNs() { return static_cast<size_t>(g_lastDischargeNs.load(std::memory_order_relaxed)); }

size_t TableBytesEstimate()
{
    std::lock_guard<std::mutex> lg(g_mu);
    return g_entries.size() * sizeof(Entry) + g_registered.size() * sizeof(void*) +
        (g_oldSlots.size() + g_domainSlots.size()) * sizeof(MAddress);
}

size_t LastOldEdgeCount() { return static_cast<size_t>(g_lastOld.load(std::memory_order_relaxed)); }
size_t LastDomainEdgeCount() { return static_cast<size_t>(g_lastDomain.load(std::memory_order_relaxed)); }
size_t LastOldOnlyCount() { return static_cast<size_t>(g_lastOldOnly.load(std::memory_order_relaxed)); }
size_t LastDomainOnlyCount() { return static_cast<size_t>(g_lastDomainOnly.load(std::memory_order_relaxed)); }

} // namespace PromotedRegionDomain
} // namespace MapleRuntime
