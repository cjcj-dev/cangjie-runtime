// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/VerifyRememberedSet.h"

#include <array>
#include <atomic>
#include <cstdio>
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

// Safe copy of TypeInfo name: never strlen on potentially non-C-string garbage targets.
void SafeTypeName(BaseObject* obj, char* buf, size_t bufSize, bool shortTail)
{
    if (bufSize == 0) {
        return;
    }
    std::memset(buf, 0, bufSize);
    if (bufSize == 1) {
        return;
    }
    buf[0] = '?';
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return;
    }
    // Avoid IsValidObject on targets that may already be mid-corruption; still try TypeInfo.
    // Holders already passed IsValidObject in the collector walk; targets may be
    // mid-evacuate so skip validity on targets and only require a TypeInfo pointer.
    TypeInfo* ti = nullptr;
    if (obj->IsValidObject()) {
        ti = obj->GetTypeInfo();
    } else {
        // Still try header TypeInfo for clustering when object looks heap-like.
        ti = obj->GetTypeInfo();
    }
    if (ti == nullptr) {
        return;
    }
    const char* name = ti->GetName();
    // Type names live in static/rodata, NOT the managed heap — do not IsHeapAddress them.
    if (name == nullptr) {
        return;
    }
    // Bounded printable scan (no strlen).
    char raw[192];
    size_t n = 0;
    for (; n + 1 < sizeof(raw); ++n) {
        char c = name[n];
        if (c == '\0') {
            break;
        }
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) {
            raw[n] = '?';
        } else {
            raw[n] = c;
        }
    }
    raw[n] = '\0';
    if (n == 0) {
        return;
    }
    const char* use = raw;
    if (shortTail) {
        const char* slash = std::strrchr(raw, ':');
        if (slash != nullptr && slash[1] != '\0') {
            use = slash + 1;
        }
    }
    size_t m = std::strlen(use);
    if (m >= bufSize) {
        m = bufSize - 1;
    }
    std::memcpy(buf, use, m);
    buf[m] = '\0';
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

    // File dump avoids VLOG vsprintf limits and survives process crash after verify.
    char path[256];
    std::snprintf(path, sizeof(path), "/root/gcresid45-run/results/residual_miss_inv%zu.tsv", invoke);
    FILE* out = std::fopen(path, "w");
    if (out != nullptr) {
        std::fprintf(out,
                     "i\tfield\toff\tholder\thType\thArr\thReg\ttarget\ttType\ttReg\thist\tapi\toutcome\tcause\n");
    }

    size_t dumped = 0;
    for (const MissingEdge& e : missingEdges) {
        WriteHistEntry wh {};
        bool found = hist.Lookup(e.field, wh);
        char holderName[160];
        char targetName[96];
        SafeTypeName(e.holder, holderName, sizeof(holderName), false);
        SafeTypeName(e.target, targetName, sizeof(targetName), true);
        byHolderType[holderName]++;

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

        if (out != nullptr) {
            std::fprintf(out, "%zu\t%p\t%zu\t%p\t%s\t%u\t%s\t%p\t%s\t%s\t%s\t%s\t%s\t%s\n", dumped,
                         reinterpret_cast<void*>(e.field), e.fieldOffset, e.holder, holderName,
                         static_cast<unsigned>(e.holderIsArray), holderReg, e.target, targetName, targetReg,
                         found ? "HIT" : "NONE", apiName, outName, causeTag);
        }
        ++dumped;
    }
    if (out != nullptr) {
        std::fprintf(out, "# RESIDUAL_BY_CAUSE a=%zu b=%zu c=%zu o=%zu hits=%zu miss=%zu "
                          "ey=%zu en=%zu et=%zu es=%zu tot=%llu wrap=%llu dumped=%zu point=%s\n",
                     causeBare, causeEarly, causeConsumed, causeOther, histHits, histMiss,
                     earlyByOutcome[static_cast<uint8_t>(CrossGenRecordOutcome::EARLY_NO_YOUNG_REGIONS)],
                     earlyByOutcome[static_cast<uint8_t>(CrossGenRecordOutcome::EARLY_NULL_OR_NON_HEAP)],
                     earlyByOutcome[static_cast<uint8_t>(CrossGenRecordOutcome::EARLY_TARGET_NOT_YOUNG)],
                     earlyByOutcome[static_cast<uint8_t>(CrossGenRecordOutcome::EARLY_SOURCE_IS_YOUNG)],
                     static_cast<unsigned long long>(hist.Total()), static_cast<unsigned long long>(hist.Wraps()),
                     dumped, point == nullptr ? "?" : point);
        for (const auto& kv : byHolderType) {
            std::fprintf(out, "# HOLDER %s\t%zu\n", kv.first.c_str(), kv.second);
        }
        std::fflush(out);
        std::fclose(out);
    }

    VLOG(REPORT,
         "[GCV2][RESIDUAL_BY_CAUSE] a=%zu b=%zu c=%zu o=%zu hits=%zu miss=%zu dumped=%zu path=%s",
         causeBare, causeEarly, causeConsumed, causeOther, histHits, histMiss, dumped, path);
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
