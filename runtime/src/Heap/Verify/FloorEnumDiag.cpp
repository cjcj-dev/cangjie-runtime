// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/FloorEnumDiag.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/AllocPhaseDiag.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
namespace FloorEnumDiag {

namespace {

struct MinorSnap {
    std::unordered_set<BaseObject*> reachable;
    std::unordered_set<MAddress> remsetSlots;
    std::unordered_set<BaseObject*> indepReachable;
    // floortarget: young targets reachable via holder edges at CapturePreEvacuate
    // (same face grant would walk). Used to answer contradiction ①.
    std::unordered_set<BaseObject*> grantVisibleYoung;
    // slotdelta: slot address → raw field value (zpointer bits) at T2 grant walk.
    // Same holder face as BuildGrantVisibleYoung (reachableVec ForEachRefField + remset).
    std::unordered_map<MAddress, MAddress> t2Slots;
    bool indepRan = false;
    size_t minorIndex = 0;
    size_t grantVisibleN = 0;
    size_t reachableN = 0;
    size_t remsetN = 0;
    size_t t2SlotN = 0;
    size_t t2Truncated = 0;
    size_t t2BuildUs = 0;
};

std::mutex g_mu;
MinorSnap g_snap;
std::atomic<size_t> g_sampleN{ 0 };
std::atomic<size_t> g_indepRuns{ 0 };
std::atomic<size_t> g_xgenRecord{ 0 };
std::atomic<size_t> g_xgenSkip{ 0 };
// slotdelta class counters (process lifetime while diag armed)
std::atomic<size_t> g_clsSame{ 0 };   // ① slot in T2, value same
std::atomic<size_t> g_clsDiff{ 0 };   // ② slot in T2, value different
std::atomic<size_t> g_clsMiss{ 0 };   // ③ slot not in T2
std::atomic<size_t> g_clsNoSlot{ 0 }; // slotAddr==0 (cannot classify)

void BuildIndepReachable(
    const std::function<void(const std::function<void(BaseObject*)>&)>& visitRoots,
    const std::function<BaseObject*(RefField<>&)>& resolveField,
    std::unordered_set<BaseObject*>& out)
{
    out.clear();
    std::vector<BaseObject*> stack;
    auto seed = [&](BaseObject* object) {
        if (object == nullptr || !Heap::IsHeapAddress(object)) {
            return;
        }
        if (!out.insert(object).second) {
            return;
        }
        stack.push_back(object);
    };
    visitRoots([&](BaseObject* object) { seed(object); });
    while (!stack.empty()) {
        BaseObject* object = stack.back();
        stack.pop_back();
        if (object == nullptr || !object->IsValidObject() || !object->HasRefField()) {
            continue;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
            continue;
        }
        object->ForEachRefField([&](RefField<>& field) {
            BaseObject* target = resolveField(field);
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                return;
            }
            if (!target->IsValidObject()) {
                return;
            }
            seed(target);
        });
    }
}

// Same holder face as postallocgap grant: walk reachableVec fields + remset slots,
// collect young targets (whether marked or not). Answers "would grant have seen this edge".
void BuildGrantVisibleYoung(
    const std::vector<BaseObject*>& reachableVec,
    const std::unordered_set<MAddress>& remsetSlots,
    const std::function<BaseObject*(RefField<>&)>& resolveField,
    std::unordered_set<BaseObject*>& out)
{
    out.clear();
    auto consider = [&](BaseObject* target) {
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            return;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (region == nullptr || !region->IsYoungRegion()) {
            return;
        }
        out.insert(target);
    };
    for (BaseObject* object : reachableVec) {
        if (object == nullptr || !Heap::IsHeapAddress(object)) {
            continue;
        }
        if (!object->IsValidObject() || !object->HasRefField()) {
            continue;
        }
        object->ForEachRefField([&](RefField<>& field) { consider(resolveField(field)); });
    }
    for (MAddress slot : remsetSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            continue;
        }
        consider(resolveField(HeapSlotAt<>(slot)));
    }
}

// slotdelta: record every slot address + raw value on the same face as grant walk.
// Cap-bounded; past cap we stop inserting and count truncations.
void BuildT2SlotFace(
    const std::vector<BaseObject*>& reachableVec,
    const std::unordered_set<MAddress>& remsetSlots,
    size_t cap,
    std::unordered_map<MAddress, MAddress>& out,
    size_t& truncated)
{
    out.clear();
    truncated = 0;
    if (cap == 0) {
        return;
    }
    out.reserve(cap < 1024 ? cap : 1024);
    auto record = [&](MAddress slotAddr, RefField<>& field) {
        if (slotAddr == 0) {
            return;
        }
        if (out.size() >= cap) {
            ++truncated;
            return;
        }
        MAddress rawVal = raw(field.GetFieldValue());
        // first write wins (same slot should not appear twice on this face)
        out.emplace(slotAddr, rawVal);
    };
    for (BaseObject* object : reachableVec) {
        if (object == nullptr || !Heap::IsHeapAddress(object)) {
            continue;
        }
        if (!object->IsValidObject() || !object->HasRefField()) {
            continue;
        }
        object->ForEachRefField([&](RefField<>& field) {
            record(reinterpret_cast<MAddress>(&field), field);
        });
    }
    for (MAddress slot : remsetSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            continue;
        }
        RefField<>& field = HeapSlotAt<>(slot);
        record(slot, field);
    }
}

} // namespace

bool DiagEnabled()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_FLOORENUM_DIAG");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

bool IndepEnabled()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_FLOORENUM_INDEP");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

size_t EveryN()
{
    static const size_t n = []() {
        const char* v = std::getenv("MRT_GCV2_FLOORENUM_EVERY");
        if (v == nullptr || v[0] == '\0') {
            return static_cast<size_t>(4);
        }
        long x = std::strtol(v, nullptr, 10);
        return x < 1 ? static_cast<size_t>(1) : static_cast<size_t>(x);
    }();
    return n;
}

size_t SlotCap()
{
    static const size_t n = []() {
        const char* v = std::getenv("MRT_GCV2_SLOTDELTA_CAP");
        if (v == nullptr || v[0] == '\0') {
            return static_cast<size_t>(262144); // 256k entries ~4MB of map nodes
        }
        long x = std::strtol(v, nullptr, 10);
        return x < 0 ? static_cast<size_t>(0) : static_cast<size_t>(x);
    }();
    return n;
}

void ClearSnap()
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_snap.reachable.clear();
    g_snap.remsetSlots.clear();
    g_snap.indepReachable.clear();
    g_snap.grantVisibleYoung.clear();
    g_snap.t2Slots.clear();
    g_snap.indepRan = false;
    g_snap.minorIndex = 0;
    g_snap.grantVisibleN = 0;
    g_snap.reachableN = 0;
    g_snap.remsetN = 0;
    g_snap.t2SlotN = 0;
    g_snap.t2Truncated = 0;
    g_snap.t2BuildUs = 0;
}

void CapturePreEvacuate(
    size_t minorIndex, const std::vector<BaseObject*>& reachableVec,
    const std::unordered_set<MAddress>& remsetSlots,
    const std::function<void(const std::function<void(BaseObject*)>&)>& visitRoots,
    const std::function<BaseObject*(RefField<>&)>& resolveField)
{
    if (!DiagEnabled()) {
        return;
    }
    MinorSnap local;
    local.minorIndex = minorIndex;
    local.reachable.reserve(reachableVec.size() * 2 + 8);
    for (BaseObject* o : reachableVec) {
        if (o != nullptr) {
            local.reachable.insert(o);
        }
    }
    local.remsetSlots = remsetSlots;
    local.reachableN = local.reachable.size();
    local.remsetN = local.remsetSlots.size();
    BuildGrantVisibleYoung(reachableVec, remsetSlots, resolveField, local.grantVisibleYoung);
    local.grantVisibleN = local.grantVisibleYoung.size();

    // slotdelta T2 face: same walk as grant, but keep (slot, raw-value) pairs.
    {
        auto t0 = std::chrono::steady_clock::now();
        BuildT2SlotFace(reachableVec, remsetSlots, SlotCap(), local.t2Slots, local.t2Truncated);
        auto t1 = std::chrono::steady_clock::now();
        local.t2BuildUs = static_cast<size_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        local.t2SlotN = local.t2Slots.size();
    }

    bool doIndep = IndepEnabled() && (EveryN() == 0 || (minorIndex % EveryN()) == 0 || minorIndex == 1);
    if (doIndep) {
        BuildIndepReachable(visitRoots, resolveField, local.indepReachable);
        local.indepRan = true;
        g_indepRuns.fetch_add(1, std::memory_order_relaxed);
    }
    size_t reachN = local.reachableN;
    size_t remN = local.remsetN;
    unsigned indepRan = static_cast<unsigned>(local.indepRan);
    size_t indepSz = local.indepReachable.size();
    size_t grantN = local.grantVisibleN;
    size_t t2N = local.t2SlotN;
    size_t t2Trunc = local.t2Truncated;
    size_t t2Us = local.t2BuildUs;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap = std::move(local);
    }
    LOG(RTLOG_ERROR,
        "[GCV2][floorenum] snap minor=%zu reachable=%zu remset=%zu grantVisYoung=%zu "
        "t2Slots=%zu t2Trunc=%zu t2Us=%zu indepRan=%u indepSize=%zu",
        minorIndex, reachN, remN, grantN, t2N, t2Trunc, t2Us, indepRan, indepSz);
}

void NoteCrossGen(bool recorded)
{
    if (!DiagEnabled()) {
        return;
    }
    if (recorded) {
        g_xgenRecord.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_xgenSkip.fetch_add(1, std::memory_order_relaxed);
    }
}

void NotePreForwardSnap(size_t fromRegions, size_t markedYoungSample)
{
    if (!DiagEnabled()) {
        return;
    }
    LOG(RTLOG_ERROR, "[GCV2][floortarget] prefwd fromRegions=%zu markedYoungSample=%zu",
        fromRegions, markedYoungSample);
}

void LogNullRouteSample(BaseObject* fromObj, BaseObject* hostObj, uintptr_t slotAddr,
                        const char* edgeSrc, const char* caller)
{
    if (!DiagEnabled()) {
        return;
    }
    size_t n = g_sampleN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 128) {
        return;
    }

    unsigned hostKnown = 0;
    unsigned hostInReachable = 0;
    unsigned hostBitmapMarked = 0;
    unsigned hostYoung = 0;
    unsigned hostType = 0;
    int hostIndep = -1;
    unsigned slotInRemset = 0;
    unsigned hostFree = 0;
    unsigned hostGarbage = 0;
    unsigned hostGhost = 0;
    unsigned hostAge = 0;
    uint8_t hostAllocMut = 0;
    uint8_t hostAllocHeap = 0;
    uint8_t tgtAllocMut = 0;
    uint8_t tgtAllocHeap = 0;
    unsigned hostAllocFound = 0;
    unsigned tgtAllocFound = 0;
    size_t minorIndex = 0;
    size_t indepSize = 0;
    unsigned indepRan = 0;

    // floortarget target-side lifecycle columns
    unsigned tgtInReach = 0;
    unsigned tgtGrantVis = 0;
    unsigned tgtBmMark = 0;   // current liveInfo IsMarkedObject
    unsigned tgtLive0 = 0;    // ghost liveInfo0 IsSurvivedObject (should be 0 here)
    unsigned tgtCurLive = 0;  // current liveInfo IsSurvivedObject
    unsigned tgtYoung = 0;
    unsigned tgtType = 0;
    unsigned tgtFrom = 0;
    unsigned tgtRoute = 0;
    unsigned tgtIsTrace = 0;
    unsigned tgtInCSet = 0; // IsFromRegion | FORWARDABLE|ROUTING|ROUTED
    unsigned tgtIsTraceAtAlloc = 0;
    unsigned tgtEverWasTrace = 0;
    unsigned tgtClearTraceCnt = 0;
    size_t tgtOffset = 0;
    size_t tgtLiveBytes = 0;
    int tgtIndep = -1;

    // slotdelta T2/T4 classification
    // class: 0=no_slot 1=same_value 2=value_changed 3=slot_not_in_t2
    unsigned deltaClass = 0;
    unsigned t2InSet = 0;
    MAddress t2Raw = 0;
    MAddress t4Raw = 0;
    size_t t2SlotN = 0;
    size_t t2Trunc = 0;

    {
        std::lock_guard<std::mutex> lock(g_mu);
        minorIndex = g_snap.minorIndex;
        indepRan = static_cast<unsigned>(g_snap.indepRan);
        indepSize = g_snap.indepReachable.size();
        t2SlotN = g_snap.t2SlotN;
        t2Trunc = g_snap.t2Truncated;
        if (slotAddr != 0 && g_snap.remsetSlots.count(static_cast<MAddress>(slotAddr)) != 0) {
            slotInRemset = 1;
        }
        if (hostObj != nullptr && Heap::IsHeapAddress(reinterpret_cast<MAddress>(hostObj))) {
            hostKnown = 1;
            if (g_snap.reachable.count(hostObj) != 0) {
                hostInReachable = 1;
            }
            if (g_snap.indepRan) {
                hostIndep = g_snap.indepReachable.count(hostObj) != 0 ? 1 : 0;
            }
        }
        if (fromObj != nullptr) {
            if (g_snap.reachable.count(fromObj) != 0) {
                tgtInReach = 1;
            }
            if (g_snap.grantVisibleYoung.count(fromObj) != 0) {
                tgtGrantVis = 1;
            }
            if (g_snap.indepRan) {
                tgtIndep = g_snap.indepReachable.count(fromObj) != 0 ? 1 : 0;
            }
        }
        // slotdelta classify under lock (read t2 map)
        if (slotAddr == 0) {
            deltaClass = 0;
            g_clsNoSlot.fetch_add(1, std::memory_order_relaxed);
        } else {
            auto it = g_snap.t2Slots.find(static_cast<MAddress>(slotAddr));
            if (it == g_snap.t2Slots.end()) {
                t2InSet = 0;
                deltaClass = 3; // ③ enum face mismatch
                g_clsMiss.fetch_add(1, std::memory_order_relaxed);
            } else {
                t2InSet = 1;
                t2Raw = it->second;
            }
        }
    }

    // T4 current raw value (outside lock; slot is live heap)
    if (slotAddr != 0 && Heap::IsHeapAddress(static_cast<MAddress>(slotAddr))) {
        RefField<>& field = HeapSlotAt<>(static_cast<MAddress>(slotAddr));
        t4Raw = raw(field.GetFieldValue());
        if (t2InSet == 1) {
            if (t2Raw == t4Raw) {
                deltaClass = 1; // ① same slot same value, different young verdict
                g_clsSame.fetch_add(1, std::memory_order_relaxed);
            } else {
                deltaClass = 2; // ② written after T2
                g_clsDiff.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (hostObj != nullptr && Heap::IsHeapAddress(reinterpret_cast<MAddress>(hostObj))) {
        hostKnown = 1;
        RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(hostObj));
        if (hr != nullptr) {
            hostYoung = static_cast<unsigned>(hr->IsYoungRegion());
            hostType = static_cast<unsigned>(hr->GetRegionType());
            hostFree = static_cast<unsigned>(hr->IsFreeRegion());
            hostGarbage = static_cast<unsigned>(hr->IsGarbageRegion());
            hostGhost = static_cast<unsigned>(hr->IsFromRegion());
            hostAge = static_cast<unsigned>(hr->GetYoungAge());
            hostBitmapMarked = static_cast<unsigned>(hr->IsMarkedObject(hostObj));
            AllocPhaseDiag::Lookup ap = AllocPhaseDiag::Find(hostObj, hr->GetRegionStart());
            hostAllocFound = static_cast<unsigned>(ap.found);
            hostAllocMut = ap.mutatorPhase;
            hostAllocHeap = ap.heapPhase;
        }
    }

    if (fromObj != nullptr && Heap::IsHeapAddress(reinterpret_cast<MAddress>(fromObj))) {
        RegionInfo* tr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(fromObj));
        if (tr != nullptr) {
            tgtYoung = static_cast<unsigned>(tr->IsYoungRegion());
            tgtType = static_cast<unsigned>(tr->GetRegionType());
            tgtFrom = static_cast<unsigned>(tr->IsFromRegion());
            tgtRoute = static_cast<unsigned>(tr->GetRouteState());
            tgtIsTrace = static_cast<unsigned>(tr->IsTraceRegion());
            tgtLiveBytes = tr->GetLiveByteCount();
            tgtOffset = tr->GetAddressOffset(reinterpret_cast<MAddress>(fromObj));
            tgtBmMark = static_cast<unsigned>(tr->IsMarkedObject(fromObj));
            LiveInfo* cur = tr->GetLiveInfo();
            if (cur != nullptr) {
                tgtCurLive = static_cast<unsigned>(cur->IsSurvivedObject(tgtOffset));
            }
            LiveInfo* ghost = tr->GetLiveInfo0ForProbe();
            if (ghost != nullptr) {
                tgtLive0 = static_cast<unsigned>(ghost->IsSurvivedObject(tgtOffset));
            }
            RegionInfo::RouteState rs = tr->GetRouteState();
            if (tr->IsFromRegion() || rs == RegionInfo::RouteState::FORWARDABLE ||
                rs == RegionInfo::RouteState::ROUTING || rs == RegionInfo::RouteState::ROUTED) {
                tgtInCSet = 1;
            }
            AllocPhaseDiag::Lookup ap = AllocPhaseDiag::Find(fromObj, tr->GetRegionStart());
            tgtAllocFound = static_cast<unsigned>(ap.found);
            tgtAllocMut = ap.mutatorPhase;
            tgtAllocHeap = ap.heapPhase;
            tgtIsTraceAtAlloc = static_cast<unsigned>(ap.isTraceAtAlloc);
            tgtEverWasTrace = static_cast<unsigned>(ap.everWasTrace);
            tgtClearTraceCnt = static_cast<unsigned>(ap.clearTraceCnt);
        }
    }

    const char* hint = "unknown";
    if (hostKnown == 0) {
        hint = "host_unknown";
    } else if (hostInReachable == 0 && hostIndep == 0) {
        hint = "A_fix_overwalk_floating";
    } else if (hostInReachable == 0 && hostIndep == 1) {
        hint = "B_mark_underwalk";
    } else if (hostInReachable == 1) {
        if (tgtGrantVis == 1 && tgtBmMark == 0) {
            hint = "target_grant_seen_unmarked"; // ⓐ/ⓒ: grant would see; mark bit missing
        } else if (tgtGrantVis == 0 && tgtBmMark == 0) {
            hint = "target_not_grant_visible"; // ⓑ/ⓓ: edge not on grant face at snap
        } else if (tgtBmMark == 1 && tgtLive0 == 0) {
            hint = "target_marked_live0_empty"; // ghost/snapshot desync
        } else {
            hint = "host_in_reachable_check_target";
        }
    } else if (hostIndep < 0 && hostInReachable == 0) {
        hint = "host_unmarked_indep_unknown";
    }

    const char* deltaName = "no_slot";
    if (deltaClass == 1) {
        deltaName = "same_value"; // ① predicate mismatch
    } else if (deltaClass == 2) {
        deltaName = "value_changed"; // ② written after T2
    } else if (deltaClass == 3) {
        deltaName = "slot_not_in_t2"; // ③ enum face mismatch
    }

    LOG(RTLOG_ERROR,
        "[GCV2][slotdelta] n=%zu minor=%zu class=%u(%s) t2InSet=%u "
        "slot=%#zx t2Raw=%#zx t4Raw=%#zx target=%p host=%p edgeSrc=%s "
        "t2Slots=%zu t2Trunc=%zu "
        "clsSame=%zu clsDiff=%zu clsMiss=%zu clsNoSlot=%zu "
        "tgtGrantVis=%u tgtBmMark=%u A:inReach=%u hint=%s",
        n, minorIndex, deltaClass, deltaName, t2InSet, static_cast<size_t>(slotAddr),
        static_cast<size_t>(t2Raw), static_cast<size_t>(t4Raw), fromObj, hostObj,
        edgeSrc != nullptr ? edgeSrc : "none", t2SlotN, t2Trunc,
        g_clsSame.load(std::memory_order_relaxed), g_clsDiff.load(std::memory_order_relaxed),
        g_clsMiss.load(std::memory_order_relaxed), g_clsNoSlot.load(std::memory_order_relaxed),
        tgtGrantVis, tgtBmMark, hostInReachable, hint);

    LOG(RTLOG_ERROR,
        "[GCV2][floortarget] n=%zu minor=%zu target=%p host=%p edgeSrc=%s caller=%s "
        "A:inReach=%u bmMark=%u young=%u "
        "B:slot=%#zx "
        "C:indep=%d indepRan=%u indepSz=%zu "
        "D:hostType=%u hostAge=%u hostFree=%u hostGarbage=%u hostGhost=%u "
        "slotRemset=%u hostAllocFound=%u hostMut=%u(%s) hostHeap=%u(%s) "
        "T:tgtInReach=%u tgtGrantVis=%u tgtBmMark=%u tgtCurLive=%u tgtLive0=%u "
        "tgtYoung=%u tgtType=%u tgtFrom=%u tgtRoute=%u tgtInCSet=%u tgtIsTrace=%u "
        "tgtOff=%zu tgtLiveB=%zu tgtIndep=%d "
        "tgtAllocFound=%u tgtMut=%u(%s) tgtHeap=%u(%s) "
        "tgtIsTraceAtAlloc=%u tgtEverWasTrace=%u tgtClearTrace=%u "
        "xgenRec=%zu xgenSkip=%zu hint=%s",
        n, minorIndex, fromObj, hostObj, edgeSrc != nullptr ? edgeSrc : "none",
        caller != nullptr ? caller : "none", hostInReachable, hostBitmapMarked, hostYoung,
        static_cast<size_t>(slotAddr), hostIndep, indepRan, indepSize, hostType, hostAge,
        hostFree, hostGarbage, hostGhost, slotInRemset, hostAllocFound,
        static_cast<unsigned>(hostAllocMut), AllocPhaseDiag::PhaseName(hostAllocMut),
        static_cast<unsigned>(hostAllocHeap), AllocPhaseDiag::PhaseName(hostAllocHeap),
        tgtInReach, tgtGrantVis, tgtBmMark, tgtCurLive, tgtLive0, tgtYoung, tgtType, tgtFrom,
        tgtRoute, tgtInCSet, tgtIsTrace, tgtOffset, tgtLiveBytes, tgtIndep, tgtAllocFound,
        static_cast<unsigned>(tgtAllocMut), AllocPhaseDiag::PhaseName(tgtAllocMut),
        static_cast<unsigned>(tgtAllocHeap), AllocPhaseDiag::PhaseName(tgtAllocHeap),
        tgtIsTraceAtAlloc, tgtEverWasTrace, tgtClearTraceCnt,
        g_xgenRecord.load(std::memory_order_relaxed), g_xgenSkip.load(std::memory_order_relaxed),
        hint);
    // Keep floorenum line for cross-lane histogram compatibility.
    LOG(RTLOG_ERROR,
        "[GCV2][floorenum] n=%zu minor=%zu target=%p host=%p edgeSrc=%s caller=%s "
        "A:inReach=%u bmMark=%u young=%u "
        "B:slot=%#zx "
        "C:indep=%d indepRan=%u indepSz=%zu "
        "D:hostType=%u hostAge=%u hostFree=%u hostGarbage=%u hostGhost=%u "
        "slotRemset=%u hostAllocFound=%u hostMut=%u(%s) hostHeap=%u(%s) "
        "tgtAllocFound=%u tgtMut=%u(%s) tgtHeap=%u(%s) "
        "xgenRec=%zu xgenSkip=%zu hint=%s",
        n, minorIndex, fromObj, hostObj, edgeSrc != nullptr ? edgeSrc : "none",
        caller != nullptr ? caller : "none", hostInReachable, hostBitmapMarked, hostYoung,
        static_cast<size_t>(slotAddr), hostIndep, indepRan, indepSize, hostType, hostAge,
        hostFree, hostGarbage, hostGhost, slotInRemset, hostAllocFound,
        static_cast<unsigned>(hostAllocMut), AllocPhaseDiag::PhaseName(hostAllocMut),
        static_cast<unsigned>(hostAllocHeap), AllocPhaseDiag::PhaseName(hostAllocHeap),
        tgtAllocFound, static_cast<unsigned>(tgtAllocMut), AllocPhaseDiag::PhaseName(tgtAllocMut),
        static_cast<unsigned>(tgtAllocHeap), AllocPhaseDiag::PhaseName(tgtAllocHeap),
        g_xgenRecord.load(std::memory_order_relaxed), g_xgenSkip.load(std::memory_order_relaxed),
        hint);
}

} // namespace FloorEnumDiag
} // namespace MapleRuntime
