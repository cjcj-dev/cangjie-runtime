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
#include <unordered_set>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Heap.h"
#include "Heap/Verify/RemsetPhaseProbe.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {
constexpr size_t kSampleLimit = 8;

struct RemsetVerifyStats {
    size_t holdersScanned = 0;
    size_t oldToYoungEdges = 0;
    size_t missing = 0;
    size_t missingArrayHolder = 0;
    size_t missingNonArray = 0;
    size_t stale = 0;
    size_t dangling = 0;
    size_t remsetCovered = 0; // remset slots that matched a live non-young field address
    size_t remsetSize = 0;
    uint64_t costNs = 0;
    std::array<MAddress, kSampleLimit> missingSamples{};
    std::array<MAddress, kSampleLimit> staleSamples{};
    std::array<MAddress, kSampleLimit> danglingSamples{};
    size_t missingSampleCount = 0;
    size_t staleSampleCount = 0;
    size_t danglingSampleCount = 0;
};

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

// Build field-address index of every ref slot on non-young live holders.
// Independence: enumeration is full-heap VisitAllObjects, not minor closure / remset.
void CollectNonYoungFieldSlots(std::unordered_set<MAddress>& fieldSlots, RemsetVerifyStats& stats,
                               const std::unordered_set<MAddress>& remsetSnapshot, const char* point, size_t invoke,
                               size_t maxFailures)
{
    Heap::GetHeap().ForEachObj(
        [&fieldSlots, &stats, &remsetSnapshot, point, invoke, maxFailures](BaseObject* holder) {
            if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                return;
            }
            RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (holderRegion == nullptr || holderRegion->IsYoungRegion() || holderRegion->IsGarbageRegion() ||
                holderRegion->IsFreeRegion()) {
                return;
            }
            ++stats.holdersScanned;
            holder->ForEachRefField([&fieldSlots, &stats, &remsetSnapshot, holder, holderRegion, point, invoke,
                                     maxFailures](RefField<>& field) {
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

                // Direct old→young edge (field-level; no cascade).
                ++stats.oldToYoungEdges;
                if (remsetSnapshot.count(slot) == 0) {
                    ++stats.missing;
                    TypeInfo* typeInfo = holder->GetTypeInfo();
                    if (typeInfo != nullptr && typeInfo->IsArrayType()) {
                        ++stats.missingArrayHolder;
                    } else {
                        ++stats.missingNonArray;
                    }
                    PushSample(stats.missingSamples, stats.missingSampleCount, slot);
                    RemsetPhaseProbe::NoteMissing(slot);
                    if (stats.missing <= maxFailures) {
                        bool targetValid = target->IsValidObject();
                        TypeInfo* targetTypeInfo = targetValid ? target->GetTypeInfo() : nullptr;
                        VLOG(REPORT,
                             "[GCV2][verify][remset][MISSING_EDGE] point=%s invoke=%zu failure=%zu max=%zu "
                             "holder=%p holderType=%s holderRegion=%p holderRegionType=%s holderYoungAge=%u "
                             "holderRouteState=%u holderRegionStart=%p holderRegionOffset=0x%zx "
                             "slot=%p fieldOffset=0x%zx slotRegionOffset=0x%zx "
                             "target=%p targetValid=%u targetType=%s targetRegion=%p targetRegionType=%s "
                             "targetYoungAge=%u targetRouteState=%u targetRegionStart=%p targetRegionOffset=0x%zx",
                             point == nullptr ? "?" : point, invoke, stats.missing, maxFailures, holder,
                             typeInfo->GetName() == nullptr ? "?" : typeInfo->GetName(), holderRegion,
                             holderRegion->GetTypeName(), static_cast<unsigned int>(holderRegion->GetYoungAge()),
                             static_cast<unsigned int>(holderRegion->GetRouteState()),
                             reinterpret_cast<void*>(holderRegion->GetRegionStart()),
                             static_cast<size_t>(reinterpret_cast<MAddress>(holder) - holderRegion->GetRegionStart()),
                             reinterpret_cast<void*>(slot), static_cast<size_t>(BaseObject::FieldOffset(holder, &field)),
                             static_cast<size_t>(slot - holderRegion->GetRegionStart()), target,
                             static_cast<unsigned int>(targetValid),
                             targetTypeInfo == nullptr || targetTypeInfo->GetName() == nullptr ? "?" :
                                                                                               targetTypeInfo->GetName(),
                             targetRegion, targetRegion->GetTypeName(),
                             static_cast<unsigned int>(targetRegion->GetYoungAge()),
                             static_cast<unsigned int>(targetRegion->GetRouteState()),
                             reinterpret_cast<void*>(targetRegion->GetRegionStart()),
                             static_cast<size_t>(reinterpret_cast<MAddress>(target) - targetRegion->GetRegionStart()));
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
        // Slot address is not a ref field of any live non-young holder we walked.
        ++stats.dangling;
        PushSample(stats.danglingSamples, stats.danglingSampleCount, slot);
    }
}
} // namespace

void VerifyRememberedSetInvariant(const char* point, const std::unordered_set<MAddress>& remsetSnapshot, bool force)
{
    // Default off — HotSpot VerifyBeforeGC/VerifyAfterGC pattern (gc_globals DIAGNOSTIC false).
    // force=true lets post-evac run without enabling the global pre-evacuate gate.
    if (!force && !EnvEnabled("MRT_GCV2_VERIFY_REMSET")) {
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
    size_t maxFailures = EnvSizeT("MRT_GCV2_VERIFY_REMSET_MAX_FAILURES", 20);

    std::unordered_set<MAddress> fieldSlots;
    CollectNonYoungFieldSlots(fieldSlots, stats, remsetSnapshot, point, invoke, maxFailures);
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

    RemsetPhaseProbe::DumpSummary(point == nullptr ? "?" : point);

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
