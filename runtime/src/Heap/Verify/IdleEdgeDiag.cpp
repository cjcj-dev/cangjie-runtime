// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/IdleEdgeDiag.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace IdleEdgeDiag {
namespace {

constexpr size_t kSampleLimit = 8;
constexpr size_t kStampCap = 1u << 18; // 262144 open-address slots
constexpr size_t kStampMask = kStampCap - 1;
constexpr size_t kPhaseBuckets = 16;

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return defaultValue;
    }
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) {
        return defaultValue;
    }
    return static_cast<size_t>(parsed);
}

const char* PhaseName(uint8_t p)
{
    switch (static_cast<GCPhase>(p)) {
        case GC_PHASE_UNDEF:
            return "UNDEF";
        case GC_PHASE_IDLE:
            return "IDLE";
        case GC_PHASE_FINISH:
            return "FINISH";
        case GC_PHASE_RECLAIM_SATB_NODE:
            return "RECLAIM_SATB";
        case GC_PHASE_INIT:
            return "INIT";
        case GC_PHASE_ENUM:
            return "ENUM";
        case GC_PHASE_TRACE:
            return "TRACE";
        case GC_PHASE_CLEAR_SATB_BUFFER:
            return "CLEAR_SATB";
        case GC_PHASE_POST_TRACE:
            return "POST_TRACE";
        case GC_PHASE_PREFORWARD:
            return "PREFORWARD";
        case GC_PHASE_FORWARD:
            return "FORWARD";
        default:
            return "OTHER";
    }
}

// Last-write stamp for barrier decisions. No TLS: open-address by field addr.
struct StampSlot {
    std::atomic<uintptr_t> field{ 0 };
    std::atomic<uint8_t> phase{ 0 };
    std::atomic<uint8_t> recorded{ 0 };
};

StampSlot g_stamps[kStampCap] = {};

std::atomic<uint64_t> g_stampNotes{ 0 };
std::atomic<uint64_t> g_stampWraps{ 0 };

// Per-process aggregates (sum over censused minors).
std::atomic<uint64_t> g_minorsCensused{ 0 };
std::atomic<uint64_t> g_edgesTotal{ 0 };
std::atomic<uint64_t> g_remsetHit{ 0 };
std::atomic<uint64_t> g_remsetMiss{ 0 };
std::atomic<uint64_t> g_missBare{ 0 };       // no barrier stamp ⇒ never hit RecordCrossGenEdge
std::atomic<uint64_t> g_missPhaseLe8{ 0 };   // stamp phase ≤ INIT(8)
std::atomic<uint64_t> g_missPhaseGt8{ 0 };   // stamp phase > 8 (other write-side gap)
std::atomic<uint64_t> g_missRecordedLost{ 0 }; // stamp said RECORDED but not in remset
std::atomic<uint64_t> g_missEarly{ 0 };      // stamp said barrier early-return
std::array<std::atomic<uint64_t>, kPhaseBuckets> g_missByPhase{};
std::atomic<uint64_t> g_costNsTotal{ 0 };

size_t HashField(MAddress field)
{
    uintptr_t x = static_cast<uintptr_t>(field) >> 3;
    x ^= x >> 17;
    x *= 0x9e3779b97f4a7c15ull;
    return static_cast<size_t>(x) & kStampMask;
}

void StoreStamp(MAddress fieldAddress, uint8_t phase, bool recorded)
{
    size_t idx = HashField(fieldAddress);
    StampSlot& slot = g_stamps[idx];
    uintptr_t prev = slot.field.load(std::memory_order_relaxed);
    if (prev != 0 && prev != static_cast<uintptr_t>(fieldAddress)) {
        g_stampWraps.fetch_add(1, std::memory_order_relaxed);
    }
    slot.phase.store(phase, std::memory_order_relaxed);
    slot.recorded.store(recorded ? 1 : 0, std::memory_order_relaxed);
    slot.field.store(static_cast<uintptr_t>(fieldAddress), std::memory_order_release);
    g_stampNotes.fetch_add(1, std::memory_order_relaxed);
}

struct StampLookup {
    bool found = false;
    uint8_t phase = 0;
    bool recorded = false;
};

StampLookup LoadStamp(MAddress fieldAddress)
{
    StampLookup r;
    size_t idx = HashField(fieldAddress);
    StampSlot& slot = g_stamps[idx];
    if (slot.field.load(std::memory_order_acquire) != static_cast<uintptr_t>(fieldAddress)) {
        return r;
    }
    r.found = true;
    r.phase = slot.phase.load(std::memory_order_relaxed);
    r.recorded = slot.recorded.load(std::memory_order_relaxed) != 0;
    return r;
}

struct CensusStats {
    size_t holdersScanned = 0;
    size_t edgesTotal = 0;
    size_t remsetHit = 0;
    size_t remsetMiss = 0;
    size_t missBare = 0;
    size_t missPhaseLe8 = 0;
    size_t missPhaseGt8 = 0;
    size_t missRecordedLost = 0;
    size_t missEarly = 0;
    size_t remsetSize = 0;
    uint64_t costNs = 0;
    std::array<size_t, kPhaseBuckets> missByPhase{};
    std::array<MAddress, kSampleLimit> missSamples{};
    size_t missSampleCount = 0;
};

void PushSample(CensusStats& stats, MAddress slot)
{
    if (stats.missSampleCount < kSampleLimit) {
        stats.missSamples[stats.missSampleCount++] = slot;
    }
}

void ClassifyMiss(CensusStats& stats, MAddress fieldAddress)
{
    ++stats.remsetMiss;
    StampLookup st = LoadStamp(fieldAddress);
    if (!st.found) {
        ++stats.missBare;
        // No barrier visit: LLVM phase≤8 bare store (or native write). Bucket 0 = UNDEF/bare.
        ++stats.missByPhase[0];
        PushSample(stats, fieldAddress);
        return;
    }
    if (st.phase < kPhaseBuckets) {
        ++stats.missByPhase[st.phase];
    } else {
        ++stats.missByPhase[0];
    }
    if (st.recorded) {
        ++stats.missRecordedLost;
    } else {
        ++stats.missEarly;
    }
    if (st.phase <= static_cast<uint8_t>(GC_PHASE_INIT)) {
        ++stats.missPhaseLe8;
    } else {
        ++stats.missPhaseGt8;
    }
    PushSample(stats, fieldAddress);
}

} // namespace

bool Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_IDLEEDGE");
    return on;
}

void NoteBarrierDecision(MAddress fieldAddress, GCPhase phase, bool recorded)
{
    if (!Enabled() || fieldAddress == 0) {
        return;
    }
    StoreStamp(fieldAddress, static_cast<uint8_t>(phase), recorded);
}

void CensusPrePinnedStamp(size_t minorRunIndex)
{
    if (!Enabled()) {
        return;
    }
    static std::atomic<size_t> invokeCount{ 0 };
    size_t invoke = invokeCount.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t every = EnvSizeT("MRT_GCV2_IDLEEDGE_EVERY", 1);
    if (every == 0) {
        every = 1;
    }
    if ((invoke - 1) % every != 0) {
        return;
    }

    uint64_t startNs = TimeUtil::NanoSeconds();
    CensusStats stats;
    std::unordered_set<MAddress> remsetSnap = Heap::GetHeap().GetRememberedSet().Snapshot();
    stats.remsetSize = remsetSnap.size();

    Heap::GetHeap().ForEachObj(
        [&stats, &remsetSnap](BaseObject* holder) {
            if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                return;
            }
            RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (holderRegion == nullptr || holderRegion->IsYoungRegion() || holderRegion->IsGarbageRegion() ||
                holderRegion->IsFreeRegion()) {
                return;
            }
            ++stats.holdersScanned;
            holder->ForEachRefField([&stats, &remsetSnap](RefField<>& field) {
                BaseObject* target = to_object(field.GetTargetObject());
                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                    return;
                }
                RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                if (targetRegion == nullptr || !targetRegion->IsYoungRegion()) {
                    return;
                }
                MAddress slot = reinterpret_cast<MAddress>(&field);
                ++stats.edgesTotal;
                if (remsetSnap.count(slot) != 0) {
                    ++stats.remsetHit;
                } else {
                    ClassifyMiss(stats, slot);
                }
            });
        },
        false);

    stats.costNs = TimeUtil::NanoSeconds() - startNs;

    g_minorsCensused.fetch_add(1, std::memory_order_relaxed);
    g_edgesTotal.fetch_add(stats.edgesTotal, std::memory_order_relaxed);
    g_remsetHit.fetch_add(stats.remsetHit, std::memory_order_relaxed);
    g_remsetMiss.fetch_add(stats.remsetMiss, std::memory_order_relaxed);
    g_missBare.fetch_add(stats.missBare, std::memory_order_relaxed);
    g_missPhaseLe8.fetch_add(stats.missPhaseLe8, std::memory_order_relaxed);
    g_missPhaseGt8.fetch_add(stats.missPhaseGt8, std::memory_order_relaxed);
    g_missRecordedLost.fetch_add(stats.missRecordedLost, std::memory_order_relaxed);
    g_missEarly.fetch_add(stats.missEarly, std::memory_order_relaxed);
    g_costNsTotal.fetch_add(stats.costNs, std::memory_order_relaxed);
    for (size_t i = 0; i < kPhaseBuckets; ++i) {
        if (stats.missByPhase[i] != 0) {
            g_missByPhase[i].fetch_add(stats.missByPhase[i], std::memory_order_relaxed);
        }
    }

    double missPct =
        stats.edgesTotal == 0 ? 0.0 : (100.0 * static_cast<double>(stats.remsetMiss) / static_cast<double>(stats.edgesTotal));
    // bare + phase≤8 among misses (Idle window attribution class).
    size_t idleClass = stats.missBare + stats.missPhaseLe8;
    double idlePctOfMiss =
        stats.remsetMiss == 0 ? 0.0 : (100.0 * static_cast<double>(idleClass) / static_cast<double>(stats.remsetMiss));
    double gt8PctOfMiss =
        stats.remsetMiss == 0 ? 0.0 :
                                (100.0 * static_cast<double>(stats.missPhaseGt8) / static_cast<double>(stats.remsetMiss));

    VLOG(REPORT,
         "[GCV2][idleedge] point=pre-pinned-stamp invoke=%zu minorRun=%zu env=MRT_GCV2_IDLEEDGE=1 "
         "remsetSize=%zu holdersScanned=%zu oldToYoungEdges=%zu remsetHit=%zu remsetMiss=%zu "
         "missPct=%.2f missBare=%zu missPhaseLe8=%zu missPhaseGt8=%zu missRecordedLost=%zu missEarly=%zu "
         "idleClassOfMiss=%zu (%.1f%%) gt8OfMiss=%zu (%.1f%%) costNs=%llu stampNotes=%llu stampWraps=%llu "
         "missSamples=[%p,%p,%p,%p]",
         invoke, minorRunIndex, stats.remsetSize, stats.holdersScanned, stats.edgesTotal, stats.remsetHit,
         stats.remsetMiss, missPct, stats.missBare, stats.missPhaseLe8, stats.missPhaseGt8, stats.missRecordedLost,
         stats.missEarly, idleClass, idlePctOfMiss, stats.missPhaseGt8, gt8PctOfMiss,
         static_cast<unsigned long long>(stats.costNs), static_cast<unsigned long long>(g_stampNotes.load()),
         static_cast<unsigned long long>(g_stampWraps.load()), reinterpret_cast<void*>(stats.missSamples[0]),
         reinterpret_cast<void*>(stats.missSamples[1]), reinterpret_cast<void*>(stats.missSamples[2]),
         reinterpret_cast<void*>(stats.missSamples[3]));

    for (size_t i = 0; i < kPhaseBuckets; ++i) {
        if (stats.missByPhase[i] == 0) {
            continue;
        }
        double pct = stats.remsetMiss == 0 ?
            0.0 :
            (100.0 * static_cast<double>(stats.missByPhase[i]) / static_cast<double>(stats.remsetMiss));
        VLOG(REPORT, "[GCV2][idleedge][MISS_BY_PHASE] invoke=%zu phase=%s(%zu) miss=%zu (%.1f%%)", invoke,
             PhaseName(static_cast<uint8_t>(i)), i, stats.missByPhase[i], pct);
    }
}

void DumpProcessTotals(const char* tag)
{
    if (!Enabled()) {
        return;
    }
    uint64_t minors = g_minorsCensused.load(std::memory_order_relaxed);
    uint64_t edges = g_edgesTotal.load(std::memory_order_relaxed);
    uint64_t hit = g_remsetHit.load(std::memory_order_relaxed);
    uint64_t miss = g_remsetMiss.load(std::memory_order_relaxed);
    uint64_t bare = g_missBare.load(std::memory_order_relaxed);
    uint64_t le8 = g_missPhaseLe8.load(std::memory_order_relaxed);
    uint64_t gt8 = g_missPhaseGt8.load(std::memory_order_relaxed);
    uint64_t lost = g_missRecordedLost.load(std::memory_order_relaxed);
    uint64_t early = g_missEarly.load(std::memory_order_relaxed);
    uint64_t cost = g_costNsTotal.load(std::memory_order_relaxed);
    double missPct = edges == 0 ? 0.0 : (100.0 * static_cast<double>(miss) / static_cast<double>(edges));
    double perMinorMiss = minors == 0 ? 0.0 : static_cast<double>(miss) / static_cast<double>(minors);
    double perMinorEdges = minors == 0 ? 0.0 : static_cast<double>(edges) / static_cast<double>(minors);
    uint64_t idleClass = bare + le8;
    double idlePct = miss == 0 ? 0.0 : (100.0 * static_cast<double>(idleClass) / static_cast<double>(miss));
    double gt8Pct = miss == 0 ? 0.0 : (100.0 * static_cast<double>(gt8) / static_cast<double>(miss));

    VLOG(REPORT,
         "[GCV2][idleedge][TOTAL] tag=%s minors=%llu edges=%llu hit=%llu miss=%llu missPct=%.2f "
         "perMinorEdges=%.1f perMinorMiss=%.1f missBare=%llu missPhaseLe8=%llu missPhaseGt8=%llu "
         "missRecordedLost=%llu missEarly=%llu idleClassOfMiss=%llu (%.1f%%) gt8OfMiss=%llu (%.1f%%) "
         "costNsTotal=%llu stampNotes=%llu stampWraps=%llu env=MRT_GCV2_IDLEEDGE=1",
         tag == nullptr ? "?" : tag, static_cast<unsigned long long>(minors), static_cast<unsigned long long>(edges),
         static_cast<unsigned long long>(hit), static_cast<unsigned long long>(miss), missPct, perMinorEdges,
         perMinorMiss, static_cast<unsigned long long>(bare), static_cast<unsigned long long>(le8),
         static_cast<unsigned long long>(gt8), static_cast<unsigned long long>(lost),
         static_cast<unsigned long long>(early), static_cast<unsigned long long>(idleClass), idlePct,
         static_cast<unsigned long long>(gt8), gt8Pct, static_cast<unsigned long long>(cost),
         static_cast<unsigned long long>(g_stampNotes.load()), static_cast<unsigned long long>(g_stampWraps.load()));

    for (size_t i = 0; i < kPhaseBuckets; ++i) {
        uint64_t m = g_missByPhase[i].load(std::memory_order_relaxed);
        if (m == 0) {
            continue;
        }
        double pct = miss == 0 ? 0.0 : (100.0 * static_cast<double>(m) / static_cast<double>(miss));
        VLOG(REPORT, "[GCV2][idleedge][TOTAL_BY_PHASE] tag=%s phase=%s(%zu) miss=%llu (%.1f%%)",
             tag == nullptr ? "?" : tag, PhaseName(static_cast<uint8_t>(i)), i, static_cast<unsigned long long>(m),
             pct);
    }
}

} // namespace IdleEdgeDiag
} // namespace MapleRuntime
