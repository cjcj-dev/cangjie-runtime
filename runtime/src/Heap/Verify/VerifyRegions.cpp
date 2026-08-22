// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
// Anchors (HotSpot, read-only reference):
//   g1HeapVerifier.cpp:424 verify_region_sets — list membership vs heap accounting
//   g1HeapVerifier.cpp:382-421 VerifyRegionListsClosure — type→set + length match
//   g1HeapRegionManager.cpp:661 verify — committed/index/address continuity
//   g1HeapRegionSet.cpp:43-53 / :330 verify_list — length, prev/next, no cycle
//
// Our heap is region + precise StackMap (not card table). Only invariants + structure are mirrored.

#include "Verify/VerifyRegions.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Allocator/RegionList.h"
#include "Allocator/RegionManager.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"

namespace MapleRuntime {
namespace {
constexpr size_t kListNameCount = 13;

const char* const kListNames[kListNameCount] = {
    "tlRegionList",
    "recentFullRegionList",
    "fromRegionList",
    "ghostFromRegionList",
    "unmovableFromRegionList",
    "garbageRegionList",
    "recentPinnedRegionList",
    "oldPinnedRegionList",
    "rawPointerPinnedRegionList",
    "oldLargeRegionList",
    "recentLargeRegionList",
    "fullTraceRegions",
    "largeTraceRegions",
};

// PrepareYoungGarbageCandidates walks these after draining the old from-list
// (RegionManager.cpp, PrepareYoungGarbageCandidates). Young on these lists is in the
// structural input to candidate selection. Route-destination-held regions are deliberately
// outside the collection domain and must be accounted separately below.
bool IsMustCoverList(const char* name)
{
    return std::strcmp(name, "fromRegionList") == 0 || std::strcmp(name, "unmovableFromRegionList") == 0 ||
        std::strcmp(name, "recentFullRegionList") == 0;
}

// Still-allocating TL young: intentionally not candidates (see PrepareYoung — does not walk tl).
bool IsActiveYoungExemptList(const char* name)
{
    return std::strcmp(name, "tlRegionList") == 0;
}

struct ListWalkStats {
    size_t regionCount = 0;
    size_t youngCount = 0;
    size_t linkBroken = 0;
    size_t typeMismatch = 0;
    size_t freeOnList = 0;
};

struct RegionMembership {
    const char* listName = nullptr;
    size_t hits = 0;
};

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

size_t EnvAsSize(const char* name, size_t defaultValue)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return defaultValue;
    }
    char* end = nullptr;
    unsigned long parsed = std::strtoul(v, &end, 10);
    if (end == v) {
        return defaultValue;
    }
    return static_cast<size_t>(parsed);
}

// Expected RegionType for list membership (RegionInfo.h RegionType ↔ RegionManager list comments).
// Returns true if type is acceptable for the named list.
bool RegionTypeMatchesList(const char* listName, RegionInfo::RegionType type)
{
    using RT = RegionInfo::RegionType;
    if (std::strcmp(listName, "tlRegionList") == 0) {
        return type == RT::THREAD_LOCAL_REGION;
    }
    if (std::strcmp(listName, "recentFullRegionList") == 0) {
        return type == RT::RECENT_FULL_REGION || type == RT::FROM_REGION;
    }
    if (std::strcmp(listName, "fromRegionList") == 0) {
        return type == RT::FROM_REGION;
    }
    if (std::strcmp(listName, "ghostFromRegionList") == 0) {
        // Ghost list may retain from/lone-from metadata during reassembly windows.
        return type == RT::FROM_REGION || type == RT::LONE_FROM_REGION || type == RT::GARBAGE_REGION;
    }
    if (std::strcmp(listName, "unmovableFromRegionList") == 0) {
        return type == RT::UNMOVABLE_FROM_REGION || type == RT::FROM_REGION;
    }
    if (std::strcmp(listName, "garbageRegionList") == 0) {
        return type == RT::GARBAGE_REGION;
    }
    if (std::strcmp(listName, "recentPinnedRegionList") == 0) {
        return type == RT::RECENT_PINNED_REGION;
    }
    if (std::strcmp(listName, "oldPinnedRegionList") == 0) {
        return type == RT::FULL_PINNED_REGION;
    }
    if (std::strcmp(listName, "rawPointerPinnedRegionList") == 0) {
        return type == RT::RAW_POINTER_PINNED_REGION;
    }
    if (std::strcmp(listName, "oldLargeRegionList") == 0) {
        return type == RT::LARGE_REGION;
    }
    if (std::strcmp(listName, "recentLargeRegionList") == 0) {
        return type == RT::RECENT_LARGE_REGION || type == RT::LARGE_REGION;
    }
    if (std::strcmp(listName, "fullTraceRegions") == 0) {
        return type == RT::RECENT_FULL_REGION || type == RT::THREAD_LOCAL_REGION;
    }
    if (std::strcmp(listName, "largeTraceRegions") == 0) {
        return type == RT::RECENT_LARGE_REGION || type == RT::LARGE_REGION;
    }
    return true;
}

void RecordRegion(const char* listName, RegionInfo* region, ListWalkStats& stats,
                  std::unordered_map<RegionInfo*, RegionMembership>& membership,
                  std::vector<RegionInfo*>* youngOut)
{
    ++stats.regionCount;
    if (region->IsFreeRegion()) {
        ++stats.freeOnList;
    }
    if (!RegionTypeMatchesList(listName, region->GetRegionType())) {
        ++stats.typeMismatch;
    }
    auto& mem = membership[region];
    if (mem.listName == nullptr) {
        mem.listName = listName;
    }
    ++mem.hits;
    if (region->IsYoungRegion()) {
        ++stats.youngCount;
        if (youngOut != nullptr) {
            youngOut->push_back(region);
        }
    }
}

// Normal doubly-linked RegionList (GetNextRegion / GetPrevRegion).
void WalkList(const char* listName, RegionList& list, ListWalkStats& stats,
              std::unordered_map<RegionInfo*, RegionMembership>& membership,
              std::vector<RegionInfo*>* youngOut)
{
    RegionInfo* prev = nullptr;
    size_t walked = 0;
    for (RegionInfo* region = list.GetHeadRegion(); region != nullptr; region = region->GetNextRegion()) {
        ++walked;
        if (region->GetPrevRegion() != prev) {
            ++stats.linkBroken;
        }
        RegionInfo* next = region->GetNextRegion();
        if (next != nullptr && next->GetPrevRegion() != region) {
            ++stats.linkBroken;
        }
        RecordRegion(listName, region, stats, membership, youngOut);
        prev = region;
        // Cycle guard (HotSpot free-list unrealistically_long_length intent).
        if (walked > list.GetRegionCount() + 8 && list.GetRegionCount() > 0) {
            ++stats.linkBroken;
            break;
        }
    }
    if (walked != list.GetRegionCount()) {
        stats.linkBroken += (walked > list.GetRegionCount()) ? (walked - list.GetRegionCount())
                                                             : (list.GetRegionCount() - walked);
    }
}

// ghostFromRegionList is a shadow of from-space linked via nextRegionIdx0 (RegionInfo.h:681,951).
// It deliberately aliases the same RegionInfo* as fromRegionList (CopyListTo); do NOT count it
// toward R1 multi-list membership — only walk for young stats / link health.
void WalkGhostList(const char* listName, RegionList& list, ListWalkStats& stats,
                   std::unordered_map<RegionInfo*, RegionMembership>& /*membership*/,
                   std::vector<RegionInfo*>* youngOut)
{
    size_t walked = 0;
    for (RegionInfo* region = list.GetHeadRegion(); region != nullptr; region = region->GetNextGhostRegion()) {
        ++walked;
        ++stats.regionCount;
        if (region->IsYoungRegion()) {
            ++stats.youngCount;
            if (youngOut != nullptr) {
                youngOut->push_back(region);
            }
        }
        if (walked > list.GetRegionCount() + 8 && list.GetRegionCount() > 0) {
            ++stats.linkBroken;
            break;
        }
    }
}
} // namespace

bool VerifyRegions::IsEnabled()
{
    return EnvIsOne("MRT_GCV2_VERIFY_REGIONS");
}

bool VerifyRegions::IsFatal()
{
    return EnvIsOne("MRT_GCV2_VERIFY_REGIONS_FATAL");
}

bool VerifyRegions::ShouldRunAt(size_t youngRunIndex)
{
    // youngRunIndex is 1-based invocation counter for this young collection (minorTotalRuns+1).
    size_t startAt = EnvAsSize("MRT_GCV2_VERIFY_REGIONS_START_AT", 0);
    if (startAt == 0) {
        return true;
    }
    return youngRunIndex >= startAt;
}

void VerifyRegions::ReportAndMaybeAbort(bool failed, const char* detail)
{
    if (!failed) {
        return;
    }
    VLOG(REPORT, "[GCV2][verify][regions] FAIL %s", detail);
    if (IsFatal()) {
        CHECK_DETAIL(false, "MRT_GCV2_VERIFY_REGIONS_FATAL: %s", detail);
    }
}

void VerifyRegions::VerifyAfterPrepareYoung(RegionManager& manager, const CandidateSet& candidates,
                                            size_t youngRunIndex, const char* point)
{
    if (!IsEnabled() || !ShouldRunAt(youngRunIndex)) {
        return;
    }
    uint64_t t0 = TimeUtil::NanoSeconds();

    ListWalkStats listStats[kListNameCount]{};
    std::unordered_map<RegionInfo*, RegionMembership> membership;
    membership.reserve(4096);
    std::vector<RegionInfo*> youngByList[kListNameCount];

    RegionList* lists[kListNameCount] = {
        &manager.tlRegionList,
        &manager.recentFullRegionList,
        &manager.fromRegionList,
        &manager.ghostFromRegionList,
        &manager.unmovableFromRegionList,
        &manager.garbageRegionList,
        &manager.recentPinnedRegionList,
        &manager.oldPinnedRegionList,
        &manager.rawPointerPinnedRegionList,
        &manager.oldLargeRegionList,
        &manager.recentLargeRegionList,
        &manager.fullTraceRegions,
        &manager.largeTraceRegions,
    };

    for (size_t i = 0; i < kListNameCount; ++i) {
        if (std::strcmp(kListNames[i], "ghostFromRegionList") == 0) {
            WalkGhostList(kListNames[i], *lists[i], listStats[i], membership, &youngByList[i]);
        } else {
            WalkList(kListNames[i], *lists[i], listStats[i], membership, &youngByList[i]);
        }
    }

    // R1: each listed region appears in exactly one list (hits==1). Free/unlisted units are outside these lists.
    size_t multiList = 0;
    size_t multiListSamples = 0;
    for (const auto& entry : membership) {
        if (entry.second.hits > 1) {
            ++multiList;
            if (multiListSamples < 3) {
                VLOG(REPORT,
                     "[GCV2][verify][regions] R1_MULTI region=%p hits=%zu firstList=%s type=%u young=%u",
                     entry.first, entry.second.hits, entry.second.listName,
                     static_cast<unsigned>(entry.first->GetRegionType()),
                     entry.first->IsYoungRegion() ? 1u : 0u);
                ++multiListSamples;
            }
        }
    }

    size_t linkBrokenTotal = 0;
    size_t typeMismatchTotal = 0;
    size_t freeOnListTotal = 0;
    size_t youngOnLists = 0;
    for (size_t i = 0; i < kListNameCount; ++i) {
        linkBrokenTotal += listStats[i].linkBroken;
        typeMismatchTotal += listStats[i].typeMismatch;
        freeOnListTotal += listStats[i].freeOnList;
        // Ghost list aliases from-list region pointers; exclude from young counter cross-check.
        if (std::strcmp(kListNames[i], "ghostFromRegionList") != 0) {
            youngOnLists += listStats[i].youngCount;
        }
    }

    // R3: youngRegionCount vs young flags observed on managed lists.
    // Note: free-region table units are not on these lists; young flag is cleared on free paths
    // (RegionManager.cpp SetYoungRegionFlag(0) at reclaim). Counter is authoritative for live young.
    // Mismatch is informational (not always a hard fail): transient windows may exist.
    size_t youngCounter = RegionInfo::GetYoungRegionCount();
    size_t youngCounterMismatch = (youngCounter == youngOnLists) ? 0 : 1;

    // R2: compare inside PrepareYoungGarbageCandidates' admission domain. The old predicate
    // required every young region on the three walked lists to be a candidate. That silently
    // assumed there were no legitimate admission exclusions. Route-destination hold added one:
    // a held young region stays on recentFull/unmovable and must remain outside the CSet while
    // a published route names its address. Keep those regions out of mustCoverYoung, account
    // every one, and continue treating any other absence (or a held candidate) as a defect.
    // Ghost young is an alias of from-list young — skip to avoid double-counting.
    size_t mustCoverYoungAll = 0;
    size_t mustCoverYoung = 0;
    size_t missingFromCandidates = 0;
    size_t missingRegionsByList[kListNameCount]{};
    size_t routeHeldExcluded = 0;
    size_t routeHeldExcludedByList[kListNameCount]{};
    size_t activeYoungExempt = 0;
    size_t otherYoung = 0;
    size_t unexpectedNonYoungCandidates = 0;
    size_t unexpectedRouteHeldCandidates = 0;
    constexpr size_t diffSampleLimitPerClass = 8;
    size_t routeHeldSamples = 0;
    size_t otherMissingSamples = 0;
    std::unordered_set<RegionInfo*> mustCoverSeen;

    for (size_t i = 0; i < kListNameCount; ++i) {
        if (std::strcmp(kListNames[i], "ghostFromRegionList") == 0) {
            continue;
        }
        for (RegionInfo* region : youngByList[i]) {
            if (IsMustCoverList(kListNames[i])) {
                if (!mustCoverSeen.insert(region).second) {
                    continue;
                }
                ++mustCoverYoungAll;
                const bool inCandidate = candidates.count(region) != 0;
                const bool routeDestHeld = region->IsRouteDestHeld();
                if (routeDestHeld) {
                    if (!inCandidate) {
                        ++routeHeldExcluded;
                        ++routeHeldExcludedByList[i];
                    }
                    if (routeHeldSamples < diffSampleLimitPerClass) {
                        VLOG(REPORT,
                             "[GCV2][verify][regions-domain-diff-sample] run=%zu region=%p "
                             "regionStart=%#zx list=%s regionType=%u young=1 candidate=%u "
                             "routeDestHeld=1 rawPointerObjects=%zu classification=%s",
                             youngRunIndex, region, region->GetRegionStart(), kListNames[i],
                             static_cast<unsigned>(region->GetRegionType()), static_cast<unsigned>(inCandidate),
                             region->GetRawPointerObjectCount(),
                             inCandidate ? "route-held-candidate" : "route-held-excluded");
                        ++routeHeldSamples;
                    }
                    continue;
                }
                ++mustCoverYoung;
                if (!inCandidate) {
                    ++missingFromCandidates;
                    ++missingRegionsByList[i];
                    if (otherMissingSamples < diffSampleLimitPerClass) {
                        VLOG(REPORT,
                             "[GCV2][verify][regions-domain-diff-sample] run=%zu region=%p "
                             "regionStart=%#zx list=%s regionType=%u young=1 candidate=0 "
                             "routeDestHeld=0 rawPointerObjects=%zu classification=unexplained-missing",
                             youngRunIndex, region, region->GetRegionStart(), kListNames[i],
                             static_cast<unsigned>(region->GetRegionType()), region->GetRawPointerObjectCount());
                        ++otherMissingSamples;
                    }
                }
            } else if (IsActiveYoungExemptList(kListNames[i])) {
                ++activeYoungExempt;
            } else {
                ++otherYoung;
            }
        }
    }
    for (RegionInfo* region : candidates) {
        if (region != nullptr) {
            if (!region->IsYoungRegion()) {
                ++unexpectedNonYoungCandidates;
            }
            if (region->IsRouteDestHeld()) {
                ++unexpectedRouteHeldCandidates;
            }
        }
    }

    // R6 light: RouteState vs region type sanity on from-space lists.
    size_t routeStateAnomalies = 0;
    auto checkRoute = [&routeStateAnomalies](RegionInfo* region) {
        if (region == nullptr) {
            return;
        }
        auto rs = region->GetRouteState();
        if (region->IsGarbageRegion() &&
            (rs == RegionInfo::RouteState::ROUTING || rs == RegionInfo::RouteState::ROUTED)) {
            ++routeStateAnomalies;
        }
    };
    manager.fromRegionList.VisitAllRegions(checkRoute);
    manager.unmovableFromRegionList.VisitAllRegions(checkRoute);

    uint64_t t1 = TimeUtil::NanoSeconds();
    VLOG(REPORT,
         "[GCV2][verify][regions] point=%s run=%zu phase=after-prepare-young "
         "env=MRT_GCV2_VERIFY_REGIONS=1 candidates=%zu mustCoverYoung=%zu mustCoverYoungAll=%zu "
         "routeHeldExcluded=%zu missing=%zu unexpectedCand=%zu unexpectedRouteHeldCand=%zu "
         "activeYoungExempt=%zu otherYoung=%zu youngOnLists=%zu youngCounter=%zu youngCounterMismatch=%zu "
         "multiList=%zu linkBroken=%zu typeMismatch=%zu freeOnList=%zu routeAnom=%zu costNs=%llu",
         point, youngRunIndex, candidates.size(), mustCoverYoung, mustCoverYoungAll, routeHeldExcluded,
         missingFromCandidates, unexpectedNonYoungCandidates, unexpectedRouteHeldCandidates, activeYoungExempt,
         otherYoung, youngOnLists, youngCounter, youngCounterMismatch, multiList, linkBrokenTotal,
         typeMismatchTotal, freeOnListTotal, routeStateAnomalies, static_cast<unsigned long long>(t1 - t0));

    for (size_t i = 0; i < kListNameCount; ++i) {
        if (listStats[i].youngCount == 0 && missingRegionsByList[i] == 0 && routeHeldExcludedByList[i] == 0) {
            continue;
        }
        VLOG(REPORT,
             "[GCV2][verify][regions] LIST_%s regions=%zu young=%zu routeHeldExcludedYoung=%zu "
             "missingYoungRegions=%zu linkBroken=%zu typeMismatch=%zu",
             kListNames[i], listStats[i].regionCount, listStats[i].youngCount, routeHeldExcludedByList[i],
             missingRegionsByList[i], listStats[i].linkBroken, listStats[i].typeMismatch);
    }

    bool failed = missingFromCandidates != 0 || unexpectedNonYoungCandidates != 0 ||
        unexpectedRouteHeldCandidates != 0 || multiList != 0 || linkBrokenTotal != 0;
    if (failed) {
        ReportAndMaybeAbort(true, "region-set invariants violated after prepare-young");
    }
}

void VerifyRegions::VerifyAfterYoungMark(RegionManager& manager, const CandidateSet& candidates, size_t youngRunIndex,
                                         const char* point)
{
    if (!IsEnabled() || !ShouldRunAt(youngRunIndex)) {
        return;
    }
    uint64_t t0 = TimeUtil::NanoSeconds();

    // Map every managed-list region → list name for attribution of off-candidate young objects.
    std::unordered_map<RegionInfo*, const char*> regionToList;
    regionToList.reserve(4096);
    auto bind = [&regionToList](const char* name, RegionList& list) {
        list.VisitAllRegions([&regionToList, name](RegionInfo* region) {
            if (regionToList.find(region) == regionToList.end()) {
                regionToList[region] = name;
            }
        });
    };
    bind("tlRegionList", manager.tlRegionList);
    bind("recentFullRegionList", manager.recentFullRegionList);
    bind("fromRegionList", manager.fromRegionList);
    // Ghost list uses nextRegionIdx0, not the normal next pointer.
    {
        const char* name = "ghostFromRegionList";
        manager.ghostFromRegionList.VisitAllGhostRegions([&regionToList, name](RegionInfo* region) {
            if (regionToList.find(region) == regionToList.end()) {
                regionToList[region] = name;
            }
        });
    }
    bind("unmovableFromRegionList", manager.unmovableFromRegionList);
    bind("garbageRegionList", manager.garbageRegionList);
    bind("recentPinnedRegionList", manager.recentPinnedRegionList);
    bind("oldPinnedRegionList", manager.oldPinnedRegionList);
    bind("rawPointerPinnedRegionList", manager.rawPointerPinnedRegionList);
    bind("oldLargeRegionList", manager.oldLargeRegionList);
    bind("recentLargeRegionList", manager.recentLargeRegionList);
    bind("fullTraceRegions", manager.fullTraceRegions);
    bind("largeTraceRegions", manager.largeTraceRegions);

    // Walk address-ordered heap units: any young region not in candidates contributes marked-object counts.
    // This is the structural positive control for:
    //   "minor marking differs from full marking: actual=A expected=E" with E-A ≈ constant
    // when full young scan marks objects whose regions were never enrolled as candidates.
    size_t offCandidateYoungRegions = 0;
    size_t offCandidateMarkedObjects = 0;
    size_t offCandidateLiveBytes = 0;
    size_t invalidObjects = 0;
    size_t liveByteInconsistent = 0;
    std::unordered_map<std::string, size_t> markedByList;
    std::unordered_map<std::string, size_t> regionsByList;

    for (uintptr_t regionAddr = manager.GetRegionHeapStart(); regionAddr < manager.GetInactiveZone();) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        regionAddr = region->GetRegionEnd();
        if (!region->IsValidRegion() || region->IsFreeRegion() || region->IsGarbageRegion()) {
            continue;
        }
        if (!region->IsYoungRegion()) {
            continue;
        }
        if (candidates.count(region) != 0) {
            continue;
        }
        ++offCandidateYoungRegions;
        const char* listName = "not_on_managed_list";
        auto it = regionToList.find(region);
        if (it != regionToList.end()) {
            listName = it->second;
        }
        ++regionsByList[listName];

        size_t markedInRegion = 0;
        size_t markedBytes = 0;
        region->VisitAllObjects([&](BaseObject* object) {
            if (object == nullptr) {
                return;
            }
            if (!object->IsValidObject()) {
                ++invalidObjects;
                return;
            }
            if (region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object)) {
                ++markedInRegion;
                markedBytes += object->GetSize();
            }
        });
        // R4 light: when live count is authoritative, marked-bytes should not exceed allocated.
        if (region->IsLiveCountAuthoritative()) {
            size_t live = region->GetLiveByteCount();
            size_t alloc = region->GetRegionAllocatedSize();
            if (live > alloc) {
                ++liveByteInconsistent;
            }
        }
        offCandidateMarkedObjects += markedInRegion;
        offCandidateLiveBytes += markedBytes;
        markedByList[listName] += markedInRegion;
    }

    uint64_t t1 = TimeUtil::NanoSeconds();
    VLOG(REPORT,
         "[GCV2][verify][regions] point=%s run=%zu phase=after-young-mark "
         "env=MRT_GCV2_VERIFY_REGIONS=1 candidates=%zu offCandYoungRegions=%zu offCandMarkedObjects=%zu "
         "offCandMarkedBytes=%zu invalidObjects=%zu liveByteInconsistent=%zu costNs=%llu",
         point, youngRunIndex, candidates.size(), offCandidateYoungRegions, offCandidateMarkedObjects,
         offCandidateLiveBytes, invalidObjects, liveByteInconsistent,
         static_cast<unsigned long long>(t1 - t0));

    for (const auto& entry : regionsByList) {
        size_t marked = 0;
        auto mit = markedByList.find(entry.first);
        if (mit != markedByList.end()) {
            marked = mit->second;
        }
        VLOG(REPORT,
             "[GCV2][verify][regions] OFF_CAND_LIST_%s youngRegions=%zu markedObjects=%zu",
             entry.first.c_str(), entry.second, marked);
    }

    // 4138-class: non-zero marked objects in young regions outside candidates is the defect signal.
    // TL active young may be young but should have zero marks if not in candidates and not traced —
    // under FULL_YOUNG_SCAN, marks only land on regions that MarkObject visits (young regions).
    // Report only; fatal only if FATAL env set.
    bool failed = offCandidateMarkedObjects != 0 || invalidObjects != 0;
    if (failed) {
        ReportAndMaybeAbort(true,
                            "young marked objects exist outside minorCandidateRegions (4138-class)");
    }
}
} // namespace MapleRuntime
