// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/FloorEnumDiag.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
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
    bool indepRan = false;
    size_t minorIndex = 0;
};

std::mutex g_mu;
MinorSnap g_snap;
std::atomic<size_t> g_sampleN{ 0 };
std::atomic<size_t> g_indepRuns{ 0 };
std::atomic<size_t> g_xgenRecord{ 0 };
std::atomic<size_t> g_xgenSkip{ 0 };

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

void ClearSnap()
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_snap.reachable.clear();
    g_snap.remsetSlots.clear();
    g_snap.indepReachable.clear();
    g_snap.indepRan = false;
    g_snap.minorIndex = 0;
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
    bool doIndep = IndepEnabled() && (EveryN() == 0 || (minorIndex % EveryN()) == 0 || minorIndex == 1);
    if (doIndep) {
        BuildIndepReachable(visitRoots, resolveField, local.indepReachable);
        local.indepRan = true;
        g_indepRuns.fetch_add(1, std::memory_order_relaxed);
    }
    size_t reachN = local.reachable.size();
    size_t remN = local.remsetSlots.size();
    unsigned indepRan = static_cast<unsigned>(local.indepRan);
    size_t indepSz = local.indepReachable.size();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_snap = std::move(local);
    }
    LOG(RTLOG_ERROR,
        "[GCV2][floorenum] snap minor=%zu reachable=%zu remset=%zu indepRan=%u indepSize=%zu",
        minorIndex, reachN, remN, indepRan, indepSz);
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

    {
        std::lock_guard<std::mutex> lock(g_mu);
        minorIndex = g_snap.minorIndex;
        indepRan = static_cast<unsigned>(g_snap.indepRan);
        indepSize = g_snap.indepReachable.size();
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
            AllocPhaseDiag::Lookup ap = AllocPhaseDiag::Find(fromObj, tr->GetRegionStart());
            tgtAllocFound = static_cast<unsigned>(ap.found);
            tgtAllocMut = ap.mutatorPhase;
            tgtAllocHeap = ap.heapPhase;
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
        hint = "host_in_reachable_check_target";
    } else if (hostIndep < 0 && hostInReachable == 0) {
        hint = "host_unmarked_indep_unknown";
    }

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
