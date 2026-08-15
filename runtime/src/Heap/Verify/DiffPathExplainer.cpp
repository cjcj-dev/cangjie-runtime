// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/DiffPathExplainer.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/Macros.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {

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

bool ShouldRunAt(size_t runIndex)
{
    size_t startAt = EnvSizeT("MRT_GCV2_DIFF_PATH_START_AT", 0);
    if (startAt != 0 && runIndex < startAt) {
        return false;
    }
    size_t every = EnvSizeT("MRT_GCV2_DIFF_PATH_EVERY", 1);
    if (every <= 1) {
        return true;
    }
    if (startAt == 0) {
        return (runIndex % every) == 0;
    }
    return ((runIndex - startAt) % every) == 0;
}

const char* TypeNameOf(BaseObject* object)
{
    if (object == nullptr || !object->IsValidObject()) {
        return "?";
    }
    TypeInfo* ti = object->GetTypeInfo();
    if (ti == nullptr) {
        return "?";
    }
    const char* name = ti->GetName();
    return name == nullptr ? "?" : name;
}

size_t SizeOf(BaseObject* object)
{
    if (object == nullptr || !object->IsValidObject()) {
        return 0;
    }
    return object->GetSize();
}

struct RegionAttrs {
    bool young = false;
    bool pinned = false;
    bool large = false;
    bool threadLocal = false;
    bool from = false;
    bool to = false;
    bool free = false;
    bool garbage = false;
    bool neverExamined = false;
    bool isCandidate = false;
    MAddress start = 0;
};

RegionAttrs DescribeRegion(BaseObject* object, const std::unordered_set<RegionInfo*>* candidates)
{
    RegionAttrs a;
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
    if (region == nullptr) {
        return a;
    }
    a.young = region->IsYoungRegion();
    a.pinned = region->IsPinnedRegion();
    a.large = region->IsLargeRegion();
    a.threadLocal = region->IsThreadLocalRegion();
    a.from = region->IsFromRegion() || region->IsLoneFromRegion();
    a.to = region->IsToRegion();
    a.free = region->IsFreeRegion();
    a.garbage = region->IsGarbageRegion();
    a.neverExamined = region->GetMarkBitmap() == nullptr && region->GetRegionAllocPtr() > region->GetRegionStart();
    a.start = region->GetRegionStart();
    if (candidates != nullptr) {
        a.isCandidate = candidates->count(region) != 0;
    }
    return a;
}

// Parent edge recorded during independent full closure for path reconstruction.
struct ParentEdge {
    BaseObject* holder = nullptr; // nullptr ⇒ root / remset synthetic
    MAddress slot = 0;            // 0 ⇒ root (not a field)
    bool viaRemset = false;
    const char* kind = "root"; // root | remset | field
};

using ObjectSet = std::unordered_set<BaseObject*>;
using ParentMap = std::unordered_map<BaseObject*, ParentEdge>;

// 乙 Full closure — independent of TraceYoungClosure.
// Follows every heap ref from roots. Builds parent map for path backtrace.
void RunFullClosure(const std::function<void(const std::function<void(BaseObject*)>&)>& visitRoots,
                    const std::function<BaseObject*(RefField<>&)>& resolveField, ObjectSet& reachable,
                    ObjectSet& youngReachable, ParentMap& parent)
{
    std::vector<BaseObject*> stack;
    auto seed = [&](BaseObject* object, const ParentEdge& edge) {
        // A free/garbage target proves its already-reachable holder is live,
        // but must not itself enter the closure: following reclaimed payload
        // invents reachability and lets the diagnostic walk that payload.
        if (!Heap::IsHeapAddress(object)) {
            return;
        }
        RegionInfo* seedRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (seedRegion == nullptr || seedRegion->IsFreeRegion() || seedRegion->IsGarbageRegion()) {
            return;
        }
        if (!reachable.insert(object).second) {
            return;
        }
        parent.emplace(object, edge);
        stack.push_back(object);
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region != nullptr && region->IsYoungRegion()) {
            youngReachable.insert(object);
        }
    };

    visitRoots([&](BaseObject* object) {
        ParentEdge e;
        e.kind = "root";
        seed(object, e);
    });

    while (!stack.empty()) {
        BaseObject* object = stack.back();
        stack.pop_back();
        if (object == nullptr || !object->IsValidObject() || !object->HasRefField()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            HeapSlot<>& referentField =
                HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            BaseObject* referent = resolveField(referentField);
            RegionInfo* referentRegion =
                Heap::IsHeapAddress(referent) ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(referent)) :
                                                nullptr;
            if (referentRegion != nullptr && !referentRegion->IsFreeRegion() && !referentRegion->IsGarbageRegion() &&
                referent->IsValidObject() && referent->HasRefField()) {
                referent->ForEachRefField([&](RefField<>& field) {
                    BaseObject* target = resolveField(field);
                    ParentEdge e;
                    e.holder = object;
                    e.slot = reinterpret_cast<MAddress>(&field);
                    e.kind = "field";
                    seed(target, e);
                });
            }
            continue;
        }
        object->ForEachRefField([&](RefField<>& field) {
            BaseObject* target = resolveField(field);
            ParentEdge e;
            e.holder = object;
            e.slot = reinterpret_cast<MAddress>(&field);
            e.kind = "field";
            seed(target, e);
        });
    }
}

// 甲 Young-only closure — independent reimplementation (NOT TraceYoungClosure).
// Seeds: roots that are young + remset slot targets that are young.
// Follows only fields of young objects into young targets.
void RunYoungOnlyClosure(const std::function<void(const std::function<void(BaseObject*)>&)>& visitRoots,
                         const std::function<BaseObject*(RefField<>&)>& resolveField,
                         const std::unordered_set<MAddress>& remsetSlots, ObjectSet& youngReachable,
                         ParentMap& parent)
{
    std::vector<BaseObject*> stack;
    auto seedYoung = [&](BaseObject* object, const ParentEdge& edge) {
        if (!Heap::IsHeapAddress(object)) {
            return;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region == nullptr || !region->IsYoungRegion()) {
            return;
        }
        if (!youngReachable.insert(object).second) {
            return;
        }
        parent.emplace(object, edge);
        stack.push_back(object);
    };

    visitRoots([&](BaseObject* object) {
        ParentEdge e;
        e.kind = "root";
        seedYoung(object, e);
    });

    for (MAddress slot : remsetSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            continue;
        }
        HeapSlot<>& field = HeapSlotAt<>(slot);
        BaseObject* target = resolveField(field);
        ParentEdge e;
        e.holder = nullptr;
        e.slot = slot;
        e.viaRemset = true;
        e.kind = "remset";
        seedYoung(target, e);
    }

    while (!stack.empty()) {
        BaseObject* object = stack.back();
        stack.pop_back();
        if (object == nullptr || !object->IsValidObject() || !object->HasRefField()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            HeapSlot<>& referentField =
                HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            BaseObject* referent = resolveField(referentField);
            RegionInfo* referentRegion =
                Heap::IsHeapAddress(referent) ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(referent)) :
                                                nullptr;
            if (referentRegion != nullptr && !referentRegion->IsFreeRegion() && !referentRegion->IsGarbageRegion() &&
                referent->IsValidObject() && referent->HasRefField()) {
                referent->ForEachRefField([&](RefField<>& field) {
                    BaseObject* target = resolveField(field);
                    ParentEdge e;
                    e.holder = object;
                    e.slot = reinterpret_cast<MAddress>(&field);
                    e.kind = "field";
                    seedYoung(target, e);
                });
            }
            continue;
        }
        object->ForEachRefField([&](RefField<>& field) {
            BaseObject* target = resolveField(field);
            ParentEdge e;
            e.holder = object;
            e.slot = reinterpret_cast<MAddress>(&field);
            e.kind = "field";
            seedYoung(target, e);
        });
    }
}

void PrintRegionAttrs(const char* prefix, BaseObject* object, const RegionAttrs& a)
{
    VLOG(REPORT,
         "%s obj=%p type=%s size=%zu regionStart=%#zx young=%u pinned=%u large=%u tl=%u "
         "from=%u to=%u free=%u garbage=%u neverExamined=%u isCandidate=%u",
         prefix, object, TypeNameOf(object), SizeOf(object), static_cast<size_t>(a.start),
         static_cast<unsigned>(a.young), static_cast<unsigned>(a.pinned), static_cast<unsigned>(a.large),
         static_cast<unsigned>(a.threadLocal), static_cast<unsigned>(a.from), static_cast<unsigned>(a.to),
         static_cast<unsigned>(a.free), static_cast<unsigned>(a.garbage), static_cast<unsigned>(a.neverExamined),
         static_cast<unsigned>(a.isCandidate));
}

// Classify one edge on a reconstructed path.
void ClassifyEdge(BaseObject* target, const ParentEdge& edge, const std::unordered_set<MAddress>& remsetSlots,
                  const std::unordered_set<MAddress>& consumedSlots, size_t edgeIndex)
{
    bool holderYoung = false;
    bool holderOld = false;
    bool holderPinned = false;
    const char* holderType = "-";
    if (edge.holder != nullptr) {
        RegionAttrs ha = DescribeRegion(edge.holder, nullptr);
        holderYoung = ha.young;
        holderOld = !ha.young && Heap::IsHeapAddress(edge.holder);
        holderPinned = ha.pinned;
        holderType = TypeNameOf(edge.holder);
    } else if (std::strcmp(edge.kind, "remset") == 0) {
        holderOld = true; // remset entries are by definition non-young holders
        holderType = "(remset-slot)";
    } else {
        holderType = "(root)";
    }

    bool inRemset = edge.slot != 0 && remsetSlots.count(edge.slot) != 0;
    bool consumed = edge.slot != 0 && consumedSlots.count(edge.slot) != 0;

    const char* why = "ok";
    if (std::strcmp(edge.kind, "root") == 0) {
        why = "root-edge";
    } else if (holderYoung) {
        why = "young-to-young";
    } else if (holderOld && !inRemset) {
        why = "BROKEN_old_to_young_NOT_IN_REMSET";
    } else if (holderOld && inRemset && !consumed) {
        why = "BROKEN_in_remset_NOT_CONSUMED";
    } else if (holderOld && inRemset && consumed) {
        // Slot was a remset root but target still not in young-only — means young-only
        // seed from remset failed (target not young? already filtered?) or later drop.
        why = "BROKEN_remset_consumed_but_target_not_in_young_only";
    }

    VLOG(REPORT,
         "[GCV2][diffpath][edge] i=%zu kind=%s holder=%p holderType=%s holderYoung=%u holderOld=%u "
         "holderPinned=%u slot=%p target=%p targetType=%s inRemset=%u consumed=%u why=%s",
         edgeIndex, edge.kind, edge.holder, holderType, static_cast<unsigned>(holderYoung),
         static_cast<unsigned>(holderOld), static_cast<unsigned>(holderPinned),
         reinterpret_cast<void*>(edge.slot), target, TypeNameOf(target), static_cast<unsigned>(inRemset),
         static_cast<unsigned>(consumed), why);

    if (std::strncmp(why, "BROKEN_", 7) == 0) {
        VLOG(REPORT,
             "[GCV2][diffpath][BROKEN_LINK] holder=%p holderType=%s holderYoung=%u holderOld=%u "
             "holderPinned=%u slot=%p target=%p targetType=%s inRemset=%u consumed=%u reason=%s",
             edge.holder, holderType, static_cast<unsigned>(holderYoung), static_cast<unsigned>(holderOld),
             static_cast<unsigned>(holderPinned), reinterpret_cast<void*>(edge.slot), target, TypeNameOf(target),
             static_cast<unsigned>(inRemset), static_cast<unsigned>(consumed), why);
    }
}

void ExplainOne(BaseObject* object, const ParentMap& fullParent, const std::unordered_set<MAddress>& remsetSlots,
                const std::unordered_set<MAddress>& consumedSlots,
                const std::unordered_set<RegionInfo*>* candidates, size_t sampleIndex)
{
    RegionAttrs a = DescribeRegion(object, candidates);
    VLOG(REPORT, "[GCV2][diffpath][D] sample=%zu ----- begin -----", sampleIndex);
    PrintRegionAttrs("[GCV2][diffpath][D]", object, a);

    // Walk parent chain in full closure toward a root (cap depth).
    constexpr size_t kMaxDepth = 32;
    std::vector<std::pair<BaseObject*, ParentEdge>> path;
    BaseObject* cur = object;
    for (size_t depth = 0; depth < kMaxDepth; ++depth) {
        auto it = fullParent.find(cur);
        if (it == fullParent.end()) {
            break;
        }
        path.push_back(*it);
        if (it->second.holder == nullptr) {
            break;
        }
        cur = it->second.holder;
    }

    VLOG(REPORT, "[GCV2][diffpath][path] sample=%zu depth=%zu (root→…→obj printed as edges leaf-first)",
         sampleIndex, path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        ClassifyEdge(path[i].first, path[i].second, remsetSlots, consumedSlots, i);
    }
    VLOG(REPORT, "[GCV2][diffpath][D] sample=%zu ----- end -----", sampleIndex);
}

} // namespace

void ReportRemsetConsumeStats(size_t minorRunIndex, const DiffPathRemsetStats& stats)
{
    if (!EnvEnabled("MRT_GCV2_REMSET_STATS") && !EnvEnabled("MRT_GCV2_DIFF_PATH")) {
        return;
    }
    size_t gap = stats.recorded >= stats.consumed ? stats.recorded - stats.consumed : 0;
    VLOG(REPORT,
         "[GCV2][remset-stats] run=%zu recorded=%zu live=%zu consumed=%zu gap(recorded-consumed)=%zu "
         "skippedNotHeap=%zu skippedWeak=%zu skippedFysFilter=%zu "
         "env=MRT_GCV2_REMSET_STATS|MRT_GCV2_DIFF_PATH "
         "CONSUMED_VS_RECORDED_rec=%zu_cons=%zu_gap=%zu",
         minorRunIndex, stats.recorded, stats.live, stats.consumed, gap, stats.skippedNotHeap, stats.skippedWeak,
         stats.skippedFysFilter, stats.recorded, stats.consumed, gap);
}

void RunDiffPathExplainer(size_t minorRunIndex,
                          const std::function<void(const std::function<void(BaseObject*)>&)>& visitRoots,
                          const std::function<BaseObject*(RefField<>&)>& resolveField,
                          const std::unordered_set<MAddress>& remsetSlots,
                          const std::unordered_set<MAddress>& consumedSlots,
                          const std::unordered_set<RegionInfo*>* candidateRegions,
                          const DiffPathRemsetStats& remsetStats,
                          std::unordered_set<BaseObject*>* rootReachableOut)
{
    ReportRemsetConsumeStats(minorRunIndex, remsetStats);

    bool runDiffPath = EnvEnabled("MRT_GCV2_DIFF_PATH") && ShouldRunAt(minorRunIndex);
    if (!runDiffPath && rootReachableOut == nullptr) {
        return;
    }

    uint64_t t0 = TimeUtil::NanoSeconds();

    ObjectSet localFullReachable;
    ObjectSet& fullReachable = rootReachableOut == nullptr ? localFullReachable : *rootReachableOut;
    fullReachable.clear();
    ObjectSet fullYoung;
    ParentMap fullParent;
    RunFullClosure(visitRoots, resolveField, fullReachable, fullYoung, fullParent);

    if (!runDiffPath) {
        return;
    }

    ObjectSet youngOnly;
    ParentMap youngParent;
    RunYoungOnlyClosure(visitRoots, resolveField, remsetSlots, youngOnly, youngParent);

    // D = fullYoung \ youngOnly
    std::vector<BaseObject*> diff;
    for (BaseObject* object : fullYoung) {
        if (youngOnly.count(object) == 0) {
            diff.push_back(object);
        }
    }

    size_t maxSamples = EnvSizeT("MRT_GCV2_DIFF_PATH_MAX_FAILURES", 8);
    size_t printed = 0;
    for (BaseObject* object : diff) {
        if (printed >= maxSamples) {
            break;
        }
        ExplainOne(object, fullParent, remsetSlots, consumedSlots, candidateRegions, printed);
        ++printed;
    }

    uint64_t costNs = TimeUtil::NanoSeconds() - t0;
    VLOG(REPORT,
         "[GCV2][diffpath][summary] run=%zu fullReachable=%zu fullYoung=%zu youngOnly=%zu "
         "DIFF_SET_size=%zu printed=%zu remsetRecorded=%zu remsetConsumed=%zu costNs=%llu "
         "env=MRT_GCV2_DIFF_PATH=1 independentClosures=1 "
         "(甲=RunYoungOnlyClosure 乙=RunFullClosure; neither is TraceYoungClosure)",
         minorRunIndex, fullReachable.size(), fullYoung.size(), youngOnly.size(), diff.size(), printed,
         remsetStats.recorded, remsetStats.consumed, static_cast<unsigned long long>(costNs));

    if (EnvEnabled("MRT_GCV2_DIFF_PATH_FATAL") && !diff.empty()) {
        CHECK_DETAIL(false,
                     "diff-path: young-only misses full-reachable young objects: |D|=%zu fullYoung=%zu youngOnly=%zu",
                     diff.size(), fullYoung.size(), youngOnly.size());
    }
}

} // namespace MapleRuntime
