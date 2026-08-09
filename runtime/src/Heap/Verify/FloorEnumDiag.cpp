// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/FloorEnumDiag.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/AllocPhaseDiag.h"
#include "Heap/Verify/NullRouteCaller.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
namespace FloorEnumDiag {

namespace {

// slotdelta key: (fromHost, fieldOffset). Absolute slot addresses diverge after
// ForwardObject (T4 walks to-version; T2 snaps from-version). Offset is stable.
struct SlotKey {
    BaseObject* host = nullptr;
    size_t offset = 0;
    bool operator==(const SlotKey& o) const { return host == o.host && offset == o.offset; }
};

struct SlotKeyHash {
    size_t operator()(const SlotKey& k) const
    {
        return std::hash<uintptr_t>()(reinterpret_cast<uintptr_t>(k.host)) ^
               (std::hash<size_t>()(k.offset) * 0x9e3779b97f4a7c15ULL);
    }
};

struct WriteRec {
    BaseObject* host = nullptr;
    size_t offset = 0;
    uintptr_t slot = 0;
    MAddress newRaw = 0;
    uint8_t phase = 0;
    uint8_t isGc = 0;
    char path[24]{};
};

struct PhaseRec {
    char where[24]{};
    uint8_t phase = 0;
};

struct MinorSnap {
    std::unordered_set<BaseObject*> reachable;
    std::unordered_set<MAddress> remsetSlots;
    std::unordered_set<BaseObject*> indepReachable;
    std::unordered_set<BaseObject*> grantVisibleYoung;
    // slotdelta: (fromHost, fieldOffset) → raw field value at T2
    std::unordered_map<SlotKey, MAddress, SlotKeyHash> t2Slots;
    // remset face: absolute slot → raw (remset has no host offset identity)
    std::unordered_map<MAddress, MAddress> t2RemsetSlots;
    // evacwrite: last-write maps while journal armed (T2→ClearSnap)
    std::unordered_map<SlotKey, WriteRec, SlotKeyHash> writeByKey;
    std::unordered_map<MAddress, WriteRec> writeBySlot;
    std::vector<PhaseRec> phases;
    size_t writeN = 0;
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
std::atomic<size_t> g_clsSame{ 0 };   // ①
std::atomic<size_t> g_clsDiff{ 0 };   // ②
std::atomic<size_t> g_clsMiss{ 0 };   // ③
std::atomic<size_t> g_clsNoSlot{ 0 };
std::atomic<bool> g_armed{ false };
// writer path counters (window-wide, not per-slot)
std::atomic<size_t> g_wMccRef{ 0 };
std::atomic<size_t> g_wMccStruct{ 0 };
std::atomic<size_t> g_wMccAtomic{ 0 };
std::atomic<size_t> g_wFixCas{ 0 };
std::atomic<size_t> g_wResolveCas{ 0 };
std::atomic<size_t> g_wCopyObject{ 0 };
std::atomic<size_t> g_wOther{ 0 };
constexpr size_t kWriteCap = 4096;
constexpr size_t kPhaseCap = 64;

const char* PhaseNameLocal(uint8_t p)
{
    switch (p) {
        case 0: return "UNDEF";
        case 1: return "IDLE";
        case 2: return "FINISH";
        case 3: return "RECLAIM_SATB";
        case 8: return "INIT";
        case 9: return "ENUM";
        case 10: return "TRACE";
        case 11: return "CLEAR_SATB";
        case 12: return "POST_TRACE";
        case 13: return "PREFORWARD";
        case 14: return "FORWARD";
        default: return "?";
    }
}

void BumpPathCounter(const char* path)
{
    if (path == nullptr) {
        g_wOther.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (std::strcmp(path, "mcc_write_ref") == 0) {
        g_wMccRef.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(path, "mcc_write_struct") == 0) {
        g_wMccStruct.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(path, "mcc_atomic") == 0) {
        g_wMccAtomic.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(path, "fix_minor_cas") == 0) {
        g_wFixCas.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(path, "fix_resolve_cas") == 0) {
        g_wResolveCas.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(path, "copy_object") == 0) {
        g_wCopyObject.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_wOther.fetch_add(1, std::memory_order_relaxed);
    }
}

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

// T2 face: key liveobj edges by (fromHost, fieldOffset); remset by absolute slot.
void BuildT2SlotFace(
    const std::vector<BaseObject*>& reachableVec,
    const std::unordered_set<MAddress>& remsetSlots,
    size_t cap,
    std::unordered_map<SlotKey, MAddress, SlotKeyHash>& outLive,
    std::unordered_map<MAddress, MAddress>& outRem,
    size_t& truncated)
{
    outLive.clear();
    outRem.clear();
    truncated = 0;
    if (cap == 0) {
        return;
    }
    outLive.reserve(cap < 1024 ? cap : 1024);
    auto liveFull = [&]() { return outLive.size() + outRem.size() >= cap; };
    for (BaseObject* object : reachableVec) {
        if (object == nullptr || !Heap::IsHeapAddress(object)) {
            continue;
        }
        if (!object->IsValidObject() || !object->HasRefField()) {
            continue;
        }
        object->ForEachRefField([&](RefField<>& field) {
            if (liveFull()) {
                ++truncated;
                return;
            }
            size_t off = reinterpret_cast<uintptr_t>(&field) - reinterpret_cast<uintptr_t>(object);
            MAddress rawVal = raw(field.GetFieldValue());
            outLive.emplace(SlotKey{ object, off }, rawVal);
        });
    }
    for (MAddress slot : remsetSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            continue;
        }
        if (liveFull()) {
            ++truncated;
            continue;
        }
        RefField<>& field = HeapSlotAt<>(slot);
        outRem.emplace(slot, raw(field.GetFieldValue()));
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
            // 2M entries: enough for ~1M reachable hosts with a few refs each before trunc.
            return static_cast<size_t>(2000000);
        }
        long x = std::strtol(v, nullptr, 10);
        return x < 0 ? static_cast<size_t>(0) : static_cast<size_t>(x);
    }();
    return n;
}

void ClearSnap()
{
    size_t writeN = 0;
    size_t writeDrop = 0;
    size_t phaseN = 0;
    size_t minorIndex = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        writeN = g_snap.writeN;
        writeDrop = 0;
        phaseN = g_snap.phases.size();
        minorIndex = g_snap.minorIndex;
        g_snap.reachable.clear();
        g_snap.remsetSlots.clear();
        g_snap.indepReachable.clear();
        g_snap.grantVisibleYoung.clear();
        g_snap.t2Slots.clear();
        g_snap.t2RemsetSlots.clear();
        g_snap.writeByKey.clear();
        g_snap.writeBySlot.clear();
        g_snap.phases.clear();
        g_snap.writeN = 0;
        g_snap.indepRan = false;
        g_snap.minorIndex = 0;
        g_snap.grantVisibleN = 0;
        g_snap.reachableN = 0;
        g_snap.remsetN = 0;
        g_snap.t2SlotN = 0;
        g_snap.t2Truncated = 0;
        g_snap.t2BuildUs = 0;
    }
    g_armed.store(false, std::memory_order_release);
    if (DiagEnabled() && (writeN != 0 || phaseN != 0)) {
        LOG(RTLOG_ERROR,
            "[GCV2][evacwrite] clear minor=%zu writeN=%zu writeDrop=%zu phaseN=%zu "
            "cntMccRef=%zu cntMccStruct=%zu cntMccAtomic=%zu cntFixCas=%zu "
            "cntResolveCas=%zu cntCopy=%zu cntOther=%zu",
            minorIndex, writeN, writeDrop, phaseN,
            g_wMccRef.load(std::memory_order_relaxed), g_wMccStruct.load(std::memory_order_relaxed),
            g_wMccAtomic.load(std::memory_order_relaxed), g_wFixCas.load(std::memory_order_relaxed),
            g_wResolveCas.load(std::memory_order_relaxed), g_wCopyObject.load(std::memory_order_relaxed),
            g_wOther.load(std::memory_order_relaxed));
    }
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

    {
        auto t0 = std::chrono::steady_clock::now();
        BuildT2SlotFace(reachableVec, remsetSlots, SlotCap(), local.t2Slots, local.t2RemsetSlots,
                        local.t2Truncated);
        auto t1 = std::chrono::steady_clock::now();
        local.t2BuildUs = static_cast<size_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        local.t2SlotN = local.t2Slots.size() + local.t2RemsetSlots.size();
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
        g_snap.writeByKey.clear();
        g_snap.writeBySlot.clear();
        g_snap.phases.clear();
        g_snap.writeN = 0;
        g_snap.phases.reserve(16);
    }
    g_wMccRef.store(0, std::memory_order_relaxed);
    g_wMccStruct.store(0, std::memory_order_relaxed);
    g_wMccAtomic.store(0, std::memory_order_relaxed);
    g_wFixCas.store(0, std::memory_order_relaxed);
    g_wResolveCas.store(0, std::memory_order_relaxed);
    g_wCopyObject.store(0, std::memory_order_relaxed);
    g_wOther.store(0, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_release);
    {
        uint8_t ph = static_cast<uint8_t>(Heap::GetHeap().GetGCPhase());
        NotePhase("t2_capture", ph);
    }
    LOG(RTLOG_ERROR,
        "[GCV2][floorenum] snap minor=%zu reachable=%zu remset=%zu grantVisYoung=%zu "
        "t2Slots=%zu t2Trunc=%zu t2Us=%zu indepRan=%u indepSize=%zu",
        minorIndex, reachN, remN, grantN, t2N, t2Trunc, t2Us, indepRan, indepSz);
}

bool WriteJournalArmed()
{
    return DiagEnabled() && g_armed.load(std::memory_order_acquire);
}

void NotePhase(const char* where, uint8_t phase)
{
    if (!DiagEnabled()) {
        return;
    }
    PhaseRec rec{};
    rec.phase = phase;
    if (where != nullptr) {
        std::strncpy(rec.where, where, sizeof(rec.where) - 1);
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_snap.phases.size() < kPhaseCap) {
            g_snap.phases.push_back(rec);
        }
    }
    LOG(RTLOG_ERROR, "[GCV2][evacwrite] phase where=%s phase=%u(%s) armed=%u",
        where != nullptr ? where : "?", static_cast<unsigned>(phase), PhaseNameLocal(phase),
        static_cast<unsigned>(g_armed.load(std::memory_order_relaxed)));
}

void NoteWrite(BaseObject* markHost, size_t fieldOffset, uintptr_t slotAddr, MAddress newRaw,
               const char* path, uint8_t phase, bool isGcThread)
{
    if (!WriteJournalArmed()) {
        return;
    }
    BumpPathCounter(path);
    WriteRec rec{};
    rec.host = markHost;
    rec.offset = fieldOffset;
    rec.slot = slotAddr;
    rec.newRaw = newRaw;
    rec.phase = phase;
    rec.isGc = isGcThread ? 1 : 0;
    if (path != nullptr) {
        std::strncpy(rec.path, path, sizeof(rec.path) - 1);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    ++g_snap.writeN;
    if (markHost != nullptr && fieldOffset != 0) {
        g_snap.writeByKey[SlotKey{ markHost, fieldOffset }] = rec;
    }
    if (slotAddr != 0) {
        g_snap.writeBySlot[static_cast<MAddress>(slotAddr)] = rec;
    }
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

    unsigned tgtInReach = 0;
    unsigned tgtGrantVis = 0;
    unsigned tgtBmMark = 0;
    unsigned tgtLive0 = 0;
    unsigned tgtCurLive = 0;
    unsigned tgtYoung = 0;
    unsigned tgtType = 0;
    unsigned tgtFrom = 0;
    unsigned tgtRoute = 0;
    unsigned tgtIsTrace = 0;
    unsigned tgtInCSet = 0;
    unsigned tgtIsTraceAtAlloc = 0;
    unsigned tgtEverWasTrace = 0;
    unsigned tgtClearTraceCnt = 0;
    size_t tgtOffset = 0;
    size_t tgtLiveBytes = 0;
    int tgtIndep = -1;

    // slotdelta
    unsigned deltaClass = 0;
    unsigned t2InSet = 0;
    MAddress t2Raw = 0;
    MAddress t4Raw = 0;
    size_t t2SlotN = 0;
    size_t t2Trunc = 0;
    size_t fieldOff = NullRouteCaller::FieldOffset();
    void* walkBase = NullRouteCaller::WalkBase();
    const char* face = "none"; // liveobj | remset | root | unknown

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

        // Classify against T2 face. Prefer liveobj key (fromHost, offset); remset by addr.
        const char* es = edgeSrc != nullptr ? edgeSrc : "none";
        if (slotAddr == 0 && hostObj == nullptr) {
            deltaClass = 0;
            face = "no_slot";
            g_clsNoSlot.fetch_add(1, std::memory_order_relaxed);
        } else if (std::strcmp(es, "remset") == 0 ||
                   (hostObj == nullptr && slotAddr != 0 &&
                    g_snap.t2RemsetSlots.count(static_cast<MAddress>(slotAddr)) != 0)) {
            face = "remset";
            auto it = g_snap.t2RemsetSlots.find(static_cast<MAddress>(slotAddr));
            if (it == g_snap.t2RemsetSlots.end()) {
                t2InSet = 0;
                deltaClass = 3;
                g_clsMiss.fetch_add(1, std::memory_order_relaxed);
            } else {
                t2InSet = 1;
                t2Raw = it->second;
            }
        } else if (hostObj != nullptr && fieldOff != 0) {
            face = "liveobj";
            SlotKey key{ hostObj, fieldOff };
            auto it = g_snap.t2Slots.find(key);
            if (it == g_snap.t2Slots.end()) {
                t2InSet = 0;
                deltaClass = 3;
                g_clsMiss.fetch_add(1, std::memory_order_relaxed);
            } else {
                t2InSet = 1;
                t2Raw = it->second;
            }
        } else if (hostObj != nullptr) {
            // fieldOff==0 (header?) or walkBase unset — try offset from host if slot known
            face = "liveobj_fallback";
            if (slotAddr != 0 && walkBase != nullptr) {
                fieldOff = static_cast<size_t>(slotAddr - reinterpret_cast<uintptr_t>(walkBase));
            } else if (slotAddr != 0) {
                fieldOff = static_cast<size_t>(slotAddr - reinterpret_cast<uintptr_t>(hostObj));
            }
            SlotKey key{ hostObj, fieldOff };
            auto it = g_snap.t2Slots.find(key);
            if (it == g_snap.t2Slots.end()) {
                t2InSet = 0;
                deltaClass = 3;
                g_clsMiss.fetch_add(1, std::memory_order_relaxed);
            } else {
                t2InSet = 1;
                t2Raw = it->second;
            }
        } else {
            face = "unknown";
            deltaClass = 0;
            g_clsNoSlot.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // T4 current raw value
    if (slotAddr != 0 && Heap::IsHeapAddress(static_cast<MAddress>(slotAddr))) {
        RefField<>& field = HeapSlotAt<>(static_cast<MAddress>(slotAddr));
        t4Raw = raw(field.GetFieldValue());
        if (t2InSet == 1) {
            if (t2Raw == t4Raw) {
                deltaClass = 1;
                g_clsSame.fetch_add(1, std::memory_order_relaxed);
            } else {
                deltaClass = 2;
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
            hint = "target_grant_seen_unmarked";
        } else if (tgtGrantVis == 0 && tgtBmMark == 0) {
            hint = "target_not_grant_visible";
        } else if (tgtBmMark == 1 && tgtLive0 == 0) {
            hint = "target_marked_live0_empty";
        } else {
            hint = "host_in_reachable_check_target";
        }
    } else if (hostIndep < 0 && hostInReachable == 0) {
        hint = "host_unmarked_indep_unknown";
    }

    const char* deltaName = "no_slot";
    if (deltaClass == 1) {
        deltaName = "same_value";
    } else if (deltaClass == 2) {
        deltaName = "value_changed";
    } else if (deltaClass == 3) {
        deltaName = "slot_not_in_t2";
    }

    // evacwrite: match journal entries for this (host,offset) or absolute slot.
    char wPathBuf[24] = "none";
    uint8_t wPhase = 0;
    uint8_t wIsGc = 0;
    unsigned wHits = 0;
    unsigned wGcHits = 0;
    unsigned wMutHits = 0;
    MAddress wLastRaw = 0;
    size_t writeN = 0;
    size_t writeDrop = 0;
    size_t phaseN = 0;
    char phaseTimeline[96]{};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        writeN = g_snap.writeN;
        writeDrop = 0;
        phaseN = g_snap.phases.size();
        size_t pt = 0;
        for (size_t i = 0; i < g_snap.phases.size() && pt + 12 < sizeof(phaseTimeline); ++i) {
            if (i != 0 && pt + 1 < sizeof(phaseTimeline)) {
                phaseTimeline[pt++] = ',';
            }
            int nw = std::snprintf(phaseTimeline + pt, sizeof(phaseTimeline) - pt, "%s=%u",
                                   g_snap.phases[i].where[0] ? g_snap.phases[i].where : "?",
                                   static_cast<unsigned>(g_snap.phases[i].phase));
            if (nw > 0) {
                pt += static_cast<size_t>(nw);
            }
        }
        const WriteRec* hit = nullptr;
        if (hostObj != nullptr && fieldOff != 0) {
            auto it = g_snap.writeByKey.find(SlotKey{ hostObj, fieldOff });
            if (it != g_snap.writeByKey.end()) {
                hit = &it->second;
            }
        }
        if (hit == nullptr && slotAddr != 0) {
            auto it = g_snap.writeBySlot.find(static_cast<MAddress>(slotAddr));
            if (it != g_snap.writeBySlot.end()) {
                hit = &it->second;
            }
        }
        if (hit != nullptr) {
            wHits = 1;
            if (hit->isGc) {
                wGcHits = 1;
            } else {
                wMutHits = 1;
            }
            if (hit->path[0]) {
                std::memcpy(wPathBuf, hit->path, sizeof(wPathBuf));
                wPathBuf[sizeof(wPathBuf) - 1] = '\0';
            }
            wPhase = hit->phase;
            wIsGc = hit->isGc;
            wLastRaw = hit->newRaw;
        }
    }
    const char* wPath = wPathBuf;
    // Heuristic when journal missed: same address bits ⇒ colour-only rewrite by fix.
    unsigned sameAddrBits = 0;
    if (t2InSet == 1 && t2Raw != 0 && t4Raw != 0) {
        constexpr MAddress kAddrMask = (static_cast<MAddress>(1) << 48) - 1u;
        if ((t2Raw & kAddrMask) == (t4Raw & kAddrMask)) {
            sameAddrBits = 1;
        }
    }
    const char* writerHint = "no_journal_hit";
    if (wHits != 0) {
        if (wGcHits != 0 && wMutHits == 0) {
            writerHint = "gc_only";
        } else if (wMutHits != 0 && wGcHits == 0) {
            writerHint = "mutator_only";
        } else {
            writerHint = "mixed";
        }
    } else if (deltaClass == 2 && sameAddrBits == 1) {
        writerHint = "colour_only_likely_fix";
    } else if (deltaClass == 2) {
        writerHint = "value_changed_no_hit";
    }

    LOG(RTLOG_ERROR,
        "[GCV2][slotdelta] n=%zu minor=%zu class=%u(%s) t2InSet=%u face=%s "
        "slot=%#zx fieldOff=%zu t2Raw=%#zx t4Raw=%#zx target=%p host=%p walkBase=%p "
        "edgeSrc=%s t2Slots=%zu t2Trunc=%zu "
        "clsSame=%zu clsDiff=%zu clsMiss=%zu clsNoSlot=%zu "
        "tgtGrantVis=%u tgtBmMark=%u A:inReach=%u hint=%s",
        n, minorIndex, deltaClass, deltaName, t2InSet, face, static_cast<size_t>(slotAddr),
        fieldOff, static_cast<size_t>(t2Raw), static_cast<size_t>(t4Raw), fromObj, hostObj,
        walkBase, edgeSrc != nullptr ? edgeSrc : "none", t2SlotN, t2Trunc,
        g_clsSame.load(std::memory_order_relaxed), g_clsDiff.load(std::memory_order_relaxed),
        g_clsMiss.load(std::memory_order_relaxed), g_clsNoSlot.load(std::memory_order_relaxed),
        tgtGrantVis, tgtBmMark, hostInReachable, hint);

    LOG(RTLOG_ERROR,
        "[GCV2][evacwrite] n=%zu minor=%zu class=%u(%s) host=%p fieldOff=%zu slot=%#zx "
        "t2Raw=%#zx t4Raw=%#zx sameAddr=%u "
        "wHits=%u wGc=%u wMut=%u wPath=%s wPhase=%u(%s) wIsGc=%u wLastRaw=%#zx "
        "writerHint=%s writeN=%zu writeDrop=%zu phaseN=%zu timeline=%s "
        "cntMccRef=%zu cntFixCas=%zu cntResolveCas=%zu cntCopy=%zu",
        n, minorIndex, deltaClass, deltaName, hostObj, fieldOff, static_cast<size_t>(slotAddr),
        static_cast<size_t>(t2Raw), static_cast<size_t>(t4Raw), sameAddrBits,
        wHits, wGcHits, wMutHits, wPath, static_cast<unsigned>(wPhase), PhaseNameLocal(wPhase),
        static_cast<unsigned>(wIsGc), static_cast<size_t>(wLastRaw),
        writerHint, writeN, writeDrop, phaseN, phaseTimeline[0] ? phaseTimeline : "-",
        g_wMccRef.load(std::memory_order_relaxed), g_wFixCas.load(std::memory_order_relaxed),
        g_wResolveCas.load(std::memory_order_relaxed),
        g_wCopyObject.load(std::memory_order_relaxed));

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
