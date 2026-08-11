// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/FlipPromoDiag.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace FlipPromoDiag {
namespace {

constexpr size_t kMaxFlipRegions = 8192;
constexpr size_t kMaxSlotSample = 32;

std::atomic<bool> g_enabledCached{ false };
std::atomic<bool> g_enabledKnown{ false };

bool ComputeEnabled()
{
    return DiagGate::LegacyOrToken("MRT_GCV2_FLIPPROMO", "flippromo");
}

std::mutex g_mu;

// Product slots this promote phase.
std::unordered_set<MAddress> g_productSlots;

// In-place / residual promote regions still young at NotePromotedRegion — same-STW oracle eligible.
std::unordered_set<RegionInfo*> g_oracleEligibleRegions;

// Staged product slots by holder for next-minor broad reconcile.
std::unordered_map<RegionInfo*, std::unordered_set<MAddress>> g_pendingProductByRegion;
std::vector<RegionInfo*> g_pendingRegions;

std::unordered_set<RegionInfo*> g_broadRegionSet;
std::unordered_set<MAddress> g_broadFoundOnPromo;
std::unordered_set<MAddress> g_broadProductUnion;

std::atomic<uint64_t> g_promoSiteNotes{ 0 };
std::atomic<uint64_t> g_productEdges{ 0 };
std::atomic<uint64_t> g_oracleEdges{ 0 };
std::atomic<uint64_t> g_leakOracleMinusProduct{ 0 };
std::atomic<uint64_t> g_extraProductMinusOracle{ 0 };
std::atomic<uint64_t> g_pathRecordPromoted{ 0 };
std::atomic<uint64_t> g_pathForwardInline{ 0 };
std::atomic<uint64_t> g_broadPromoEdges{ 0 };
std::atomic<uint64_t> g_leakBroadMinusProduct{ 0 };
std::atomic<uint64_t> g_extraProductMinusBroad{ 0 };
std::atomic<uint64_t> g_broadRegionsAlive{ 0 };
std::atomic<uint64_t> g_broadRegionsDead{ 0 };
std::atomic<uint64_t> g_phaseDumps{ 0 };
std::atomic<uint64_t> g_sampleLogged{ 0 };
std::atomic<uint64_t> g_broadAllO2Y{ 0 };
std::atomic<uint64_t> g_oracleRegions{ 0 };

void LogLeakSample(const char* kind, MAddress slot, RegionInfo* region)
{
    uint64_t s = g_sampleLogged.fetch_add(1, std::memory_order_relaxed);
    if (s >= kMaxSlotSample) {
        return;
    }
    VLOG(REPORT,
         "[FLIPPROMO][SAMPLE] kind=%s slot=%#zx region=%p",
         kind, static_cast<size_t>(slot), region);
}

// Match product RecordPromotedCrossGenEdges liveness gate (useLiveOnly).
size_t OracleScanRegionMatchProduct(RegionInfo* region, std::unordered_set<MAddress>& out)
{
    if (region == nullptr) {
        return 0;
    }
    bool hasObjectLiveness = region->IsLargeRegion() || region->GetMarkBitmap() != nullptr ||
        region->GetResurrectBitmap() != nullptr;
    bool useLiveOnly = hasObjectLiveness && region->IsLiveCountAuthoritative();
    size_t n = 0;
    region->VisitAllObjects([&out, &n, region, hasObjectLiveness, useLiveOnly](BaseObject* object) {
        if (object == nullptr || !object->HasRefField()) {
            return;
        }
        if (useLiveOnly) {
            bool survived =
                hasObjectLiveness &&
                region->IsSurvivedObject(region->GetAddressOffset(reinterpret_cast<MAddress>(object)));
            if (!survived) {
                return;
            }
        }
        object->ForEachRefField([&out, &n](RefField<>& field) {
            BaseObject* target = to_object(field.GetTargetObject());
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                MAddress slot = reinterpret_cast<MAddress>(&field);
                if (out.insert(slot).second) {
                    ++n;
                }
            }
        });
    });
    return n;
}

// Full VisitAllObjects O→Y with no live filter — detects intentional useLiveOnly skips as "leak".
size_t OracleScanRegionFull(RegionInfo* region, std::unordered_set<MAddress>& out)
{
    if (region == nullptr) {
        return 0;
    }
    size_t n = 0;
    region->VisitAllObjects([&out, &n](BaseObject* object) {
        if (object == nullptr || !object->HasRefField()) {
            return;
        }
        object->ForEachRefField([&out, &n](RefField<>& field) {
            BaseObject* target = to_object(field.GetTargetObject());
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                MAddress slot = reinterpret_cast<MAddress>(&field);
                if (out.insert(slot).second) {
                    ++n;
                }
            }
        });
    });
    return n;
}

void StageProductSlotsLocked()
{
    // Group all product slots by holder region for next-minor broad axis.
    std::unordered_map<RegionInfo*, std::unordered_set<MAddress>> byRegion;
    for (MAddress slot : g_productSlots) {
        RegionInfo* holder = RegionInfo::GetRegionInfoAt(slot);
        if (holder == nullptr) {
            continue;
        }
        byRegion[holder].insert(slot);
    }
    for (auto& kv : byRegion) {
        if (g_pendingRegions.size() >= kMaxFlipRegions) {
            break;
        }
        g_pendingProductByRegion[kv.first] = std::move(kv.second);
        g_pendingRegions.push_back(kv.first);
    }
}

void RunInPlaceOracleLocked()
{
    // Only regions still young when NotePromotedRegion ran (in-place / residual).
    // Forward-inline holders are already old to-space; region-wide oracle would include
    // pre-existing old objects and false-positive "leak".
    for (RegionInfo* region : g_oracleEligibleRegions) {
        if (region == nullptr || region->IsGarbageRegion()) {
            continue;
        }
        g_oracleRegions.fetch_add(1, std::memory_order_relaxed);

        std::unordered_set<MAddress> oracleMatch;
        size_t matchN = OracleScanRegionMatchProduct(region, oracleMatch);
        g_oracleEdges.fetch_add(matchN, std::memory_order_relaxed);

        std::unordered_set<MAddress> productHere;
        for (MAddress slot : g_productSlots) {
            if (RegionInfo::GetRegionInfoAt(slot) == region) {
                productHere.insert(slot);
            }
        }

        // Matched-liveness diff: should be ~0 if product walk is complete.
        for (MAddress slot : oracleMatch) {
            if (productHere.find(slot) == productHere.end()) {
                g_leakOracleMinusProduct.fetch_add(1, std::memory_order_relaxed);
                LogLeakSample("oracleMatch-minus-product", slot, region);
            }
        }
        for (MAddress slot : productHere) {
            if (oracleMatch.find(slot) == oracleMatch.end()) {
                g_extraProductMinusOracle.fetch_add(1, std::memory_order_relaxed);
                LogLeakSample("product-minus-oracleMatch", slot, region);
            }
        }

        // Full oracle (no live filter): reports dead-object O→Y product intentionally skips.
        // Counted only as SAMPLE lines under kind full-minus-product (not mixed into leakO-P).
        std::unordered_set<MAddress> oracleFull;
        (void)OracleScanRegionFull(region, oracleFull);
        for (MAddress slot : oracleFull) {
            if (productHere.find(slot) == productHere.end() &&
                oracleMatch.find(slot) == oracleMatch.end()) {
                LogLeakSample("full-minus-product-deadskip", slot, region);
            }
        }
    }
}

} // namespace

bool Enabled()
{
    if (!g_enabledKnown.load(std::memory_order_acquire)) {
        bool en = ComputeEnabled();
        g_enabledCached.store(en, std::memory_order_relaxed);
        g_enabledKnown.store(true, std::memory_order_release);
        return en;
    }
    return g_enabledCached.load(std::memory_order_relaxed);
}

void NoteProductRecord(MAddress slot, unsigned path)
{
    if (!Enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_productSlots.insert(slot).second) {
        g_productEdges.fetch_add(1, std::memory_order_relaxed);
        if (path == 1) {
            g_pathForwardInline.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_pathRecordPromoted.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void NotePromotedRegion(RegionInfo* region, unsigned path, size_t productRecorded)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    (void)path;
    (void)productRecorded;
    std::lock_guard<std::mutex> lock(g_mu);
    g_promoSiteNotes.fetch_add(1, std::memory_order_relaxed);
    // Caller must invoke while region is still young (in-place / residual path).
    if (region->IsYoungRegion()) {
        g_oracleEligibleRegions.insert(region);
    }
}

void OnBroadScanBegin(size_t minorRunIndex)
{
    if (!Enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_broadRegionSet.clear();
    g_broadFoundOnPromo.clear();
    g_broadProductUnion.clear();

    for (RegionInfo* region : g_pendingRegions) {
        auto it = g_pendingProductByRegion.find(region);
        if (it == g_pendingProductByRegion.end()) {
            continue;
        }
        if (region == nullptr || region->IsGarbageRegion() || region->IsYoungRegion()) {
            g_broadRegionsDead.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        g_broadRegionsAlive.fetch_add(1, std::memory_order_relaxed);
        g_broadRegionSet.insert(region);
        for (MAddress slot : it->second) {
            g_broadProductUnion.insert(slot);
        }
    }
    g_pendingProductByRegion.clear();
    g_pendingRegions.clear();

    VLOG(REPORT,
         "[FLIPPROMO][BROAD-BEGIN] minor=%zu pendingMoved alive=%llu dead=%llu productUnion=%zu",
         minorRunIndex,
         static_cast<unsigned long long>(g_broadRegionsAlive.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_broadRegionsDead.load(std::memory_order_relaxed)),
         g_broadProductUnion.size());
}

void NoteBroadRecord(RegionInfo* holderRegion, MAddress slot)
{
    if (!Enabled()) {
        return;
    }
    g_broadAllO2Y.fetch_add(1, std::memory_order_relaxed);
    if (holderRegion == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_broadRegionSet.find(holderRegion) == g_broadRegionSet.end()) {
        return;
    }
    if (g_broadFoundOnPromo.insert(slot).second) {
        g_broadPromoEdges.fetch_add(1, std::memory_order_relaxed);
    }
}

void OnPromotePhaseEnd(size_t minorRunIndex, size_t promoteReplay, size_t residualPromote)
{
    if (!Enabled()) {
        return;
    }
    size_t leakB = 0;
    size_t extraB = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);

        // Previous minor's broad-vs-product (filled at this minor's begin).
        for (MAddress slot : g_broadFoundOnPromo) {
            if (g_broadProductUnion.find(slot) == g_broadProductUnion.end()) {
                ++leakB;
                LogLeakSample("broad-minus-product", slot, nullptr);
            }
        }
        for (MAddress slot : g_broadProductUnion) {
            if (g_broadFoundOnPromo.find(slot) == g_broadFoundOnPromo.end()) {
                ++extraB;
                LogLeakSample("product-minus-broad", slot, nullptr);
            }
        }
        if (leakB != 0) {
            g_leakBroadMinusProduct.fetch_add(leakB, std::memory_order_relaxed);
        }
        if (extraB != 0) {
            g_extraProductMinusBroad.fetch_add(extraB, std::memory_order_relaxed);
        }
        g_broadRegionSet.clear();
        g_broadFoundOnPromo.clear();
        g_broadProductUnion.clear();

        // In-place same-STW oracle (Forward-inline uses next-minor broad axis only).
        RunInPlaceOracleLocked();
        StageProductSlotsLocked();

        g_productSlots.clear();
        g_oracleEligibleRegions.clear();
    }

    g_phaseDumps.fetch_add(1, std::memory_order_relaxed);
    VLOG(REPORT,
         "[FLIPPROMO][PHASE] minor=%zu promoteReplay=%zu residualPromote=%zu "
         "siteNotes=%llu productEdges=%llu oracleEdges=%llu oracleRegions=%llu "
         "leakO-P=%llu extraP-O=%llu | broadPromoEdges=%llu broadAllO2Y=%llu "
         "leakB-P=%llu extraP-B=%llu pathFwd=%llu pathRec=%llu",
         minorRunIndex, promoteReplay, residualPromote,
         static_cast<unsigned long long>(g_promoSiteNotes.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_productEdges.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_oracleEdges.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_oracleRegions.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_leakOracleMinusProduct.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_extraProductMinusOracle.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_broadPromoEdges.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_broadAllO2Y.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_leakBroadMinusProduct.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_extraProductMinusBroad.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pathForwardInline.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pathRecordPromoted.load(std::memory_order_relaxed)));
}

void DumpProcessTotals(const char* tag)
{
    if (!Enabled()) {
        return;
    }
    VLOG(REPORT,
         "[FLIPPROMO][TOTAL] tag=%s phaseDumps=%llu siteNotes=%llu productEdges=%llu "
         "oracleEdges=%llu oracleRegions=%llu leakO-P=%llu extraP-O=%llu "
         "broadPromoEdges=%llu broadAllO2Y=%llu leakB-P=%llu extraP-B=%llu "
         "pathFwd=%llu pathRec=%llu broadAlive=%llu broadDead=%llu",
         tag == nullptr ? "-" : tag,
         static_cast<unsigned long long>(g_phaseDumps.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_promoSiteNotes.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_productEdges.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_oracleEdges.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_oracleRegions.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_leakOracleMinusProduct.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_extraProductMinusOracle.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_broadPromoEdges.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_broadAllO2Y.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_leakBroadMinusProduct.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_extraProductMinusBroad.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pathForwardInline.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pathRecordPromoted.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_broadRegionsAlive.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_broadRegionsDead.load(std::memory_order_relaxed)));
}

} // namespace FlipPromoDiag
} // namespace MapleRuntime
