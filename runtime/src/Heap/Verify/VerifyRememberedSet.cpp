// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/VerifyRememberedSet.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Barrier/WriteHist.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {
constexpr size_t kSampleLimit = 8;
// Dump every residual MISSING edge (task gcresid45). Cap guards log flood if counts explode.
constexpr size_t kMissingDumpCap = 512;

struct RemsetVerifyStats {
    size_t holdersScanned = 0;
    size_t oldToYoungEdges = 0;
    size_t missing = 0;
    size_t missingArrayHolder = 0;
    size_t missingNonArray = 0;
    size_t stale = 0;
    size_t dangling = 0;
    size_t remsetCovered = 0;
    size_t remsetSize = 0;
    uint64_t costNs = 0;
    std::array<MAddress, kSampleLimit> missingSamples{};
    std::array<MAddress, kSampleLimit> staleSamples{};
    std::array<MAddress, kSampleLimit> danglingSamples{};
    size_t missingSampleCount = 0;
    size_t staleSampleCount = 0;
    size_t danglingSampleCount = 0;
};

struct MissingEdge {
    MAddress field = 0;
    BaseObject* holder = nullptr;
    BaseObject* target = nullptr;
    size_t fieldOffset = 0;
    bool holderIsArray = false;
};

const char* RegionTypeName(RegionInfo* region)
{
    if (region == nullptr) {
        return "null";
    }
    switch (region->GetRegionType()) {
        case RegionInfo::RegionType::FREE_REGION:
            return "FREE";
        case RegionInfo::RegionType::THREAD_LOCAL_REGION:
            return "THREAD_LOCAL";
        case RegionInfo::RegionType::RECENT_FULL_REGION:
            return "RECENT_FULL";
        case RegionInfo::RegionType::FROM_REGION:
            return "FROM";
        case RegionInfo::RegionType::LONE_FROM_REGION:
            return "LONE_FROM";
        case RegionInfo::RegionType::UNMOVABLE_FROM_REGION:
            return "UNMOVABLE_FROM";
        case RegionInfo::RegionType::TO_REGION:
            return "TO";
        case RegionInfo::RegionType::FULL_PINNED_REGION:
            return "FULL_PINNED";
        case RegionInfo::RegionType::RECENT_PINNED_REGION:
            return "RECENT_PINNED";
        case RegionInfo::RegionType::RAW_POINTER_PINNED_REGION:
            return "RAW_POINTER_PINNED";
        case RegionInfo::RegionType::TL_RAW_POINTER_REGION:
            return "TL_RAW_POINTER";
        case RegionInfo::RegionType::TL_LARGE_RAW_POINTER_REGION:
            return "TL_LARGE_RAW_POINTER";
        case RegionInfo::RegionType::LARGE_REGION:
            return "LARGE";
        case RegionInfo::RegionType::RECENT_LARGE_REGION:
            return "RECENT_LARGE";
        case RegionInfo::RegionType::GARBAGE_REGION:
            return "GARBAGE";
        default:
            return "OTHER";
    }
}

void ShortTypeName(BaseObject* obj, char* buf, size_t bufSize)
{
    if (bufSize == 0) {
        return;
    }
    buf[0] = '?';
    if (bufSize == 1) {
        buf[0] = '\0';
        return;
    }
    buf[1] = '\0';
    if (obj == nullptr || !obj->IsValidObject()) {
        return;
    }
    TypeInfo* ti = obj->GetTypeInfo();
    if (ti == nullptr) {
        return;
    }
    const char* name = ti->GetName();
    if (name == nullptr) {
        return;
    }
    // Prefer last path segment after ':' for short identity; still cap length.
    const char* slash = std::strrchr(name, ':');
    const char* use = (slash != nullptr && slash[1] != '\0') ? slash + 1 : name;
    size_t n = std::strlen(use);
    if (n >= bufSize) {
        n = bufSize - 1;
    }
    std::memcpy(buf, use, n);
    buf[n] = '\0';
}

bool EnvEnabled(const char* name)
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

void PushSample(std::array<MAddress, kSampleLimit>& samples, size_t& count, MAddress slot)
{
    if (count < kSampleLimit) {
        samples[count++] = slot;
    }
}

void CollectNonYoungFieldSlots(std::unordered_set<MAddress>& fieldSlots, RemsetVerifyStats& stats,
                               const std::unordered_set<MAddress>& remsetSnapshot,
                               std::vector<MissingEdge>& missingEdges)
{
    Heap::GetHeap().ForEachObj(
        [&fieldSlots, &stats, &remsetSnapshot, &missingEdges](BaseObject* holder) {
            if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                return;
            }
            RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (holderRegion == nullptr || holderRegion->IsYoungRegion() || holderRegion->IsGarbageRegion() ||
                holderRegion->IsFreeRegion()) {
                return;
            }
            ++stats.holdersScanned;
            TypeInfo* typeInfo = holder->GetTypeInfo();
            bool isArray = typeInfo != nullptr && typeInfo->IsArrayType();
            holder->ForEachRefField(
                [&fieldSlots, &stats, &remsetSnapshot, &missingEdges, holder, isArray](RefField<>& field) {
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    fieldSlots.insert(slot);

                    BaseObject* target = field.GetTargetObject();
                    if (target == nullptr || !Heap::IsHeapAddress(target)) {
                        if (remsetSnapshot.count(slot) != 0) {
                            ++stats.stale;
                            PushSample(stats.staleSamples, stats.staleSampleCount, slot);
                        }
                        return;
                    }
                    RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (targetRegion == nullptr || !targetRegion->IsYoungRegion()) {
                        if (remsetSnapshot.count(slot) != 0) {
                            ++stats.stale;
                            PushSample(stats.staleSamples, stats.staleSampleCount, slot);
                        }
                        return;
                    }

                    ++stats.oldToYoungEdges;
                    if (remsetSnapshot.count(slot) == 0) {
                        ++stats.missing;
                        if (isArray) {
                            ++stats.missingArrayHolder;
                        } else {
                            ++stats.missingNonArray;
                        }
                        PushSample(stats.missingSamples, stats.missingSampleCount, slot);
                        if (missingEdges.size() < kMissingDumpCap) {
                            MissingEdge edge;
                            edge.field = slot;
                            edge.holder = holder;
                            edge.target = target;
                            edge.fieldOffset = static_cast<size_t>(slot - reinterpret_cast<MAddress>(holder));
                            edge.holderIsArray = isArray;
                            missingEdges.push_back(edge);
                        }
                    }
                });
        },
        false);
}

void ClassifyRemsetOnlySlots(const std::unordered_set<MAddress>& remsetSnapshot,
                             const std::unordered_set<MAddress>& fieldSlots, RemsetVerifyStats& stats)
{
    for (MAddress slot : remsetSnapshot) {
        if (fieldSlots.count(slot) != 0) {
            ++stats.remsetCovered;
            continue;
        }
        ++stats.dangling;
        PushSample(stats.danglingSamples, stats.danglingSampleCount, slot);
    }
}

void DumpMissingEdges(const char* point, size_t invoke, const std::vector<MissingEdge>& missingEdges)
{
    WriteHist& hist = WriteHist::Instance();
    size_t causeBare = 0;
    size_t causeEarly = 0;
    size_t causeConsumed = 0;
    size_t causeOther = 0;
    size_t histHits = 0;
    size_t histMiss = 0;
    std::unordered_map<std::string, size_t> byHolderType;
    std::array<size_t, 16> earlyByOutcome{};

    size_t dumped = 0;
    for (const MissingEdge& e : missingEdges) {
        WriteHistEntry wh {};
        bool found = hist.Lookup(e.field, wh);
        char holderName[96];
        char targetName[96];
        ShortTypeName(e.holder, holderName, sizeof(holderName));
        ShortTypeName(e.target, targetName, sizeof(targetName));

        // Full name for clustering map (short name can collide across modules).
        const char* fullHolder = "?";
        if (e.holder != nullptr && e.holder->IsValidObject()) {
            TypeInfo* ti = e.holder->GetTypeInfo();
            if (ti != nullptr && ti->GetName() != nullptr) {
                fullHolder = ti->GetName();
            }
        }
        byHolderType[fullHolder]++;

        RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(e.holder));
        RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(e.target));
        const char* holderReg = RegionTypeName(holderRegion);
        const char* targetReg = RegionTypeName(targetRegion);

        const char* causeTag = "other";
        const char* apiName = "none";
        const char* outName = "NONE";

        if (!found) {
            ++histMiss;
            ++causeBare;
            causeTag = "a_bare_store";
        } else {
            ++histHits;
            apiName = WriteHist::ApiName(wh.api);
            outName = WriteHist::OutcomeName(wh.outcome);
            if (wh.outcome == static_cast<uint8_t>(CrossGenRecordOutcome::RECORDED)) {
                ++causeConsumed;
                causeTag = "c_consumed_lost";
            } else if (wh.outcome == static_cast<uint8_t>(CrossGenRecordOutcome::NONE)) {
                ++causeOther;
                causeTag = "other";
            } else {
                ++causeEarly;
                if (wh.outcome < 16) {
                    ++earlyByOutcome[wh.outcome];
                }
                causeTag = "b_early_return";
            }
        }

        // Short lines: long generic type names + many %s overflow vsprintf_s (seen as WriteLogImpl fail).
        VLOG(REPORT,
             "[GCV2][MISS] i=%zu f=%p off=%zu h=%p ht=%s ha=%u hr=%s t=%p tt=%s tr=%s hist=%s api=%s out=%s cause=%s",
             dumped, reinterpret_cast<void*>(e.field), e.fieldOffset, e.holder, holderName,
             static_cast<unsigned>(e.holderIsArray), holderReg, e.target, targetName, targetReg,
             found ? "HIT" : "NONE", apiName, outName, causeTag);
        ++dumped;
    }

    VLOG(REPORT,
         "[GCV2][RESIDUAL_BY_CAUSE] a=%zu b=%zu c=%zu o=%zu hits=%zu miss=%zu "
         "ey=%zu en=%zu et=%zu es=%zu tot=%llu wrap=%llu dumped=%zu",
         causeBare, causeEarly, causeConsumed, causeOther, histHits, histMiss,
         earlyByOutcome[static_cast<uint8_t>(CrossGenRecordOutcome::EARLY_NO_YOUNG_REGIONS)],
         earlyByOutcome[static_cast<uint8_t>(CrossGenRecordOutcome::EARLY_NULL_OR_NON_HEAP)],
         earlyByOutcome[static_cast<uint8_t>(CrossGenRecordOutcome::EARLY_TARGET_NOT_YOUNG)],
         earlyByOutcome[static_cast<uint8_t>(CrossGenRecordOutcome::EARLY_SOURCE_IS_YOUNG)],
         static_cast<unsigned long long>(hist.Total()), static_cast<unsigned long long>(hist.Wraps()), dumped);

    size_t typeIdx = 0;
    for (const auto& kv : byHolderType) {
        VLOG(REPORT, "[GCV2][RESIDUAL_BY_HOLDER] i=%zu type=%s n=%zu", typeIdx, kv.first.c_str(), kv.second);
        ++typeIdx;
    }
    VLOG(REPORT, "[GCV2][RESIDUAL_HOLDER_N] types=%zu dumped=%zu", byHolderType.size(), dumped);
}
} // namespace

void VerifyRememberedSetInvariant(const char* point, const std::unordered_set<MAddress>& remsetSnapshot)
{
    // Default off — HotSpot VerifyBeforeGC/VerifyAfterGC pattern (gc_globals DIAGNOSTIC false).
    if (!EnvEnabled("MRT_GCV2_VERIFY_REMSET")) {
        return;
    }

    static std::atomic<size_t> invokeCount{0};
    size_t invoke = invokeCount.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t startAt = EnvSizeT("MRT_GCV2_VERIFY_REMSET_START_AT", 0);
    if (startAt != 0 && invoke < startAt) {
        return;
    }
    size_t every = EnvSizeT("MRT_GCV2_VERIFY_REMSET_EVERY", 1);
    if (every == 0) {
        every = 1;
    }
    if (startAt != 0) {
        if ((invoke - startAt) % every != 0) {
            return;
        }
    } else if ((invoke - 1) % every != 0) {
        return;
    }

    uint64_t startNs = TimeUtil::NanoSeconds();
    RemsetVerifyStats stats;
    stats.remsetSize = remsetSnapshot.size();

    std::unordered_set<MAddress> fieldSlots;
    std::vector<MissingEdge> missingEdges;
    CollectNonYoungFieldSlots(fieldSlots, stats, remsetSnapshot, missingEdges);
    ClassifyRemsetOnlySlots(remsetSnapshot, fieldSlots, stats);
    stats.costNs = TimeUtil::NanoSeconds() - startNs;

    VLOG(REPORT,
         "[GCV2][verify][remset] point=%s invoke=%zu env=MRT_GCV2_VERIFY_REMSET=1 "
         "remsetSize=%zu holdersScanned=%zu oldToYoungEdges=%zu "
         "MISSING=%zu (arrayHolder=%zu nonArray=%zu) STALE=%zu DANGLING=%zu "
         "remsetCovered=%zu costNs=%llu directOnly=1 "
         "missingSamples=[%p,%p,%p,%p] staleSamples=[%p,%p,%p,%p] danglingSamples=[%p,%p,%p,%p]",
         point == nullptr ? "?" : point, invoke, stats.remsetSize, stats.holdersScanned, stats.oldToYoungEdges,
         stats.missing, stats.missingArrayHolder, stats.missingNonArray, stats.stale, stats.dangling,
         stats.remsetCovered, static_cast<unsigned long long>(stats.costNs),
         reinterpret_cast<void*>(stats.missingSamples[0]), reinterpret_cast<void*>(stats.missingSamples[1]),
         reinterpret_cast<void*>(stats.missingSamples[2]), reinterpret_cast<void*>(stats.missingSamples[3]),
         reinterpret_cast<void*>(stats.staleSamples[0]), reinterpret_cast<void*>(stats.staleSamples[1]),
         reinterpret_cast<void*>(stats.staleSamples[2]), reinterpret_cast<void*>(stats.staleSamples[3]),
         reinterpret_cast<void*>(stats.danglingSamples[0]), reinterpret_cast<void*>(stats.danglingSamples[1]),
         reinterpret_cast<void*>(stats.danglingSamples[2]), reinterpret_cast<void*>(stats.danglingSamples[3]));

    // Per-edge residual dump (gcresid45): every MISSING with holder/field/target + WriteHist cause.
    if (!missingEdges.empty()) {
        DumpMissingEdges(point, invoke, missingEdges);
    }

    // Report-only by default. Separate stricter switch aborts (never default).
    if (EnvEnabled("MRT_GCV2_VERIFY_REMSET_FATAL") &&
        (stats.missing != 0 || stats.stale != 0 || stats.dangling != 0)) {
        CHECK_DETAIL(false,
                     "remset invariant R broken: point=%s MISSING=%zu STALE=%zu DANGLING=%zu oldToYoung=%zu remset=%zu",
                     point == nullptr ? "?" : point, stats.missing, stats.stale, stats.dangling, stats.oldToYoungEdges,
                     stats.remsetSize);
    }
}
} // namespace MapleRuntime
