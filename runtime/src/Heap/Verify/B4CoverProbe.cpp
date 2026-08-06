// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "B4CoverProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {

#define B4CV_LOG(fmt, ...)                                                                                             \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][b4cover] " fmt "\n", ##__VA_ARGS__);                                               \
        std::fflush(stderr);                                                                                           \
    } while (0)

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v) {
        return def;
    }
    return static_cast<size_t>(n);
}

bool PageMapped(uintptr_t addr)
{
    if (addr == 0) {
        return false;
    }
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        pageSize = 4096;
    }
    uintptr_t page = addr & ~(static_cast<uintptr_t>(pageSize) - 1);
    unsigned char vec = 0;
    if (mincore(reinterpret_cast<void*>(page), static_cast<size_t>(pageSize), &vec) == 0) {
        return true;
    }
    return false;
}

TypeInfo* PeekTypeInfoAt(uintptr_t addr)
{
    if (addr == 0 || (addr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return nullptr;
    }
    if (!Heap::IsHeapAddress(addr)) {
        return nullptr;
    }
#ifdef __arm__
    uint32_t raw = 0;
    std::memcpy(&raw, reinterpret_cast<const void*>(addr), sizeof(raw));
    return reinterpret_cast<TypeInfo*>(static_cast<uintptr_t>(raw));
#else
    uint32_t low = 0;
    uint16_t high = 0;
    std::memcpy(&low, reinterpret_cast<const void*>(addr), sizeof(low));
    std::memcpy(&high, reinterpret_cast<const void*>(addr + 4), sizeof(high));
    uintptr_t tipAddr = (static_cast<uintptr_t>(high) << 32) | static_cast<uintptr_t>(low);
    return reinterpret_cast<TypeInfo*>(tipAddr);
#endif
}

bool TipLooksValid(TypeInfo* tip)
{
    if (tip == nullptr) {
        return false;
    }
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return false;
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    bool inTim = TypeInfoManager::GetTypeInfoManager().ContainsAddress(tipAddr);
    if (!inTim && !PageMapped(tipAddr)) {
        return false;
    }
    if (!tip->IsVaildType()) {
        return false;
    }
    MSize isz = tip->GetInstanceSize();
    if (isz == 0 || isz > (1u << 20)) {
        return false;
    }
    return true;
}

size_t SaneObjectSize(TypeInfo* tip, RegionInfo* region)
{
    if (tip == nullptr || region == nullptr) {
        return 0;
    }
    MSize isz = tip->GetInstanceSize();
    size_t size = (static_cast<size_t>(isz) + 8u + 7u) & ~static_cast<size_t>(7u);
    size_t regionBytes = region->GetRegionEnd() - region->GetRegionStart();
    if (size < 16 || size > regionBytes || size > (1u << 20)) {
        return 0;
    }
    return size;
}

enum class Kind : uint8_t { Base = 0, Interior = 1, Unknown = 2, NotHeap = 3, Null = 4 };

void Classify(uintptr_t value, Kind& kind, uintptr_t& baseOut, size_t& offsetOut, TypeInfo*& tipAtBase)
{
    kind = Kind::Unknown;
    baseOut = 0;
    offsetOut = 0;
    tipAtBase = nullptr;

    if (value == 0) {
        kind = Kind::Null;
        return;
    }
    if (!Heap::IsHeapAddress(value)) {
        kind = Kind::NotHeap;
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(value);
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        kind = Kind::Unknown;
        return;
    }

    TypeInfo* tipAtValue = PeekTypeInfoAt(value);
    if (TipLooksValid(tipAtValue)) {
        kind = Kind::Base;
        baseOut = value;
        offsetOut = 0;
        tipAtBase = tipAtValue;
        return;
    }

    static const size_t kOffs[] = { 8, 16, 24, 32, 40, 48, 56, 64 };
    for (size_t off : kOffs) {
        if (value < off) {
            continue;
        }
        uintptr_t cand = value - off;
        if (!Heap::IsHeapAddress(cand)) {
            continue;
        }
        RegionInfo* candRegion = RegionInfo::TryGetRegionInfoAt(cand);
        if (candRegion != region) {
            continue;
        }
        TypeInfo* tip = PeekTypeInfoAt(cand);
        if (!TipLooksValid(tip)) {
            continue;
        }
        size_t size = SaneObjectSize(tip, region);
        bool sizeOk = false;
        if (size != 0 && value >= cand && value < cand + size && cand + size <= region->GetRegionEnd()) {
            sizeOk = true;
        }
        if (!sizeOk && size == 0 && (off == 8 || off == 16 || off == 24 || off == 32)) {
            sizeOk = true;
        }
        if (!sizeOk) {
            continue;
        }
        kind = Kind::Interior;
        baseOut = cand;
        offsetOut = off;
        tipAtBase = tip;
        return;
    }
}

const char* TipName(TypeInfo* tip)
{
    if (tip == nullptr || !TipLooksValid(tip)) {
        return "?";
    }
    const char* n = tip->GetName();
    return n != nullptr ? n : "?";
}

struct FixRec {
    MAddress oldVal = 0;
    MAddress newVal = 0;
    uint8_t visits = 0;
    uint8_t changed = 0;
    uint8_t wasGhost = 0;
    uint8_t wasOldTag = 0;
};

struct MinorLedger {
    std::unordered_set<MAddress> remset;
    std::unordered_set<MAddress> reachFields;
    std::unordered_map<MAddress, FixRec> fix;
    uint64_t gen = 0;
    bool valid = false;
};

std::mutex gMu;
std::atomic<bool> gArmed{ false };
std::atomic<uint64_t> gDumpLeft{ 0 };
uint64_t gGen = 0;

MinorLedger gCur;
MinorLedger gPrior;

std::atomic<uint64_t> gRemsetNote{ 0 };
std::atomic<uint64_t> gReachNote{ 0 };
std::atomic<uint64_t> gFixVisits{ 0 };
std::atomic<uint64_t> gFixChanged{ 0 };
std::atomic<uint64_t> gScanCalls{ 0 };
std::atomic<uint64_t> gInterior{ 0 };

// classification totals (unique slots per process via gSeen*)
std::atomic<uint64_t> gInRemsetNotFixed{ 0 };
std::atomic<uint64_t> gInRemsetFixed{ 0 };
std::atomic<uint64_t> gInReachNotFixed{ 0 };
std::atomic<uint64_t> gInReachFixed{ 0 };
std::atomic<uint64_t> gNotInFixset{ 0 };
std::atomic<uint64_t> gNotInRemset{ 0 }; // subset: not remset (may still be reach)
std::atomic<uint64_t> gNoPriorLedger{ 0 };
std::atomic<uint64_t> gInRemsetOnlyNotVisited{ 0 }; // remset membership but Fix never saw slot
std::atomic<uint64_t> gInReachOnlyNotVisited{ 0 };

std::unordered_set<MAddress> gSeenSlots;

bool TryTakeDump()
{
    uint64_t left = gDumpLeft.load(std::memory_order_relaxed);
    while (left > 0) {
        if (gDumpLeft.compare_exchange_weak(left, left - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void ArmOnce()
{
    if (gArmed.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    size_t dumpMax = EnvSizeT("MRT_GCV2_B4COVER_DUMP_MAX", 96);
    gDumpLeft.store(dumpMax, std::memory_order_relaxed);
    B4CV_LOG("ARMED env=MRT_GCV2_B4COVER=1 dumpMax=%zu", dumpMax);
}

} // namespace

bool B4CoverProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_B4COVER");
    return on;
}

void B4CoverProbe::NoteRemsetDrain(const std::unordered_set<MAddress>& slots)
{
    if (!Enabled()) {
        return;
    }
    ArmOnce();
    std::lock_guard<std::mutex> lk(gMu);
    gCur.remset = slots;
    gRemsetNote.fetch_add(1, std::memory_order_relaxed);
    B4CV_LOG("REMSET_DRAIN n=%zu gen_cur=%llu", slots.size(), static_cast<unsigned long long>(gCur.gen));
}

void B4CoverProbe::NoteReachableFieldSlots(const std::vector<BaseObject*>& reachableVec)
{
    if (!Enabled()) {
        return;
    }
    ArmOnce();
    std::unordered_set<MAddress> fields;
    fields.reserve(reachableVec.size() * 4);
    for (BaseObject* object : reachableVec) {
        if (object == nullptr || !Heap::IsHeapAddress(object) || !object->HasRefField()) {
            continue;
        }
        // Prefer ghost-from base if already prepared; else use as-is (pre-PrepareForwardTable).
        BaseObject* host = object;
        object->ForEachRefField([&fields, host](RefField<>& field) {
            (void)host;
            fields.insert(reinterpret_cast<MAddress>(&field));
        });
    }
    std::lock_guard<std::mutex> lk(gMu);
    gCur.reachFields.swap(fields);
    gReachNote.fetch_add(1, std::memory_order_relaxed);
    B4CV_LOG("REACH_FIELDS nObj=%zu nFields=%zu", reachableVec.size(), gCur.reachFields.size());
}

void B4CoverProbe::NoteFixVisit(MAddress slot, MAddress oldVal, MAddress newVal, bool casChanged, bool wasGhost,
                                bool wasOldTag)
{
    if (!Enabled()) {
        return;
    }
    ArmOnce();
    gFixVisits.fetch_add(1, std::memory_order_relaxed);
    if (casChanged) {
        gFixChanged.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lk(gMu);
    FixRec& r = gCur.fix[slot];
    if (r.visits < 255) {
        ++r.visits;
    }
    r.oldVal = oldVal;
    r.newVal = newVal;
    if (casChanged) {
        r.changed = 1;
    }
    if (wasGhost) {
        r.wasGhost = 1;
    }
    if (wasOldTag) {
        r.wasOldTag = 1;
    }
}

void B4CoverProbe::CommitMinorLedger(const char* site)
{
    if (!Enabled()) {
        return;
    }
    ArmOnce();
    std::lock_guard<std::mutex> lk(gMu);
    gCur.gen = ++gGen;
    gCur.valid = true;
    gPrior = gCur;
    // reset current for next minor (keep empty sets)
    gCur = MinorLedger{};
    B4CV_LOG("COMMIT site=%s priorGen=%llu remset=%zu reach=%zu fix=%zu", site == nullptr ? "?" : site,
             static_cast<unsigned long long>(gPrior.gen), gPrior.remset.size(), gPrior.reachFields.size(),
             gPrior.fix.size());
}

void B4CoverProbe::ScanInteriors(const char* point)
{
    if (!Enabled()) {
        return;
    }
    ArmOnce();
    gScanCalls.fetch_add(1, std::memory_order_relaxed);

    MinorLedger prior;
    {
        std::lock_guard<std::mutex> lk(gMu);
        prior = gPrior;
    }

    size_t interior = 0;
    size_t cRemNotFix = 0;
    size_t cRemFix = 0;
    size_t cReachNotFix = 0;
    size_t cReachFix = 0;
    size_t cNotFixset = 0;
    size_t cNotRemset = 0;
    size_t cNoPrior = 0;
    size_t cRemNotVisit = 0;
    size_t cReachNotVisit = 0;

    Heap::GetHeap().ForEachObj(
        [&](BaseObject* holder) {
            if (holder == nullptr || !Heap::IsHeapAddress(holder) || !holder->HasRefField()) {
                return;
            }
            TypeInfo* hTip = PeekTypeInfoAt(reinterpret_cast<uintptr_t>(holder));
            if (!TipLooksValid(hTip)) {
                return;
            }
            holder->ForEachRefField([&](RefField<>& field) {
                MAddress slot = reinterpret_cast<MAddress>(&field);
                MAddress raw = field.GetFieldValue();
                RefField<> peek(raw);
                uintptr_t value = reinterpret_cast<uintptr_t>(peek.GetTargetObject());
                Kind kind = Kind::Unknown;
                uintptr_t base = 0;
                size_t off = 0;
                TypeInfo* tip = nullptr;
                Classify(value, kind, base, off, tip);
                if (kind != Kind::Interior) {
                    return;
                }
                ++interior;
                gInterior.fetch_add(1, std::memory_order_relaxed);

                bool first = false;
                {
                    std::lock_guard<std::mutex> lk(gMu);
                    first = gSeenSlots.insert(slot).second;
                }

                const char* verdict = "B4CV_NO_PRIOR_LEDGER";
                bool inRem = false;
                bool inReach = false;
                bool visited = false;
                bool changed = false;
                bool wasGhost = false;
                bool wasOldTag = false;
                MAddress fixOld = 0;
                MAddress fixNew = 0;
                uint8_t visits = 0;

                if (!prior.valid) {
                    ++cNoPrior;
                    gNoPriorLedger.fetch_add(1, std::memory_order_relaxed);
                } else {
                    inRem = prior.remset.count(slot) != 0;
                    inReach = prior.reachFields.count(slot) != 0;
                    auto it = prior.fix.find(slot);
                    if (it != prior.fix.end()) {
                        visited = it->second.visits > 0;
                        changed = it->second.changed != 0;
                        wasGhost = it->second.wasGhost != 0;
                        wasOldTag = it->second.wasOldTag != 0;
                        fixOld = it->second.oldVal;
                        fixNew = it->second.newVal;
                        visits = it->second.visits;
                    }

                    if (inRem && visited && !changed) {
                        verdict = "B4CV_IN_REMSET_NOT_FIXED";
                        ++cRemNotFix;
                        if (first) {
                            gInRemsetNotFixed.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (inRem && visited && changed) {
                        verdict = "B4CV_IN_REMSET_FIXED";
                        ++cRemFix;
                        if (first) {
                            gInRemsetFixed.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (inRem && !visited) {
                        verdict = "B4CV_IN_REMSET_NOT_VISITED";
                        ++cRemNotVisit;
                        if (first) {
                            gInRemsetOnlyNotVisited.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (inReach && visited && !changed) {
                        verdict = "B4CV_IN_REACH_NOT_FIXED";
                        ++cReachNotFix;
                        if (first) {
                            gInReachNotFixed.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (inReach && visited && changed) {
                        verdict = "B4CV_IN_REACH_FIXED";
                        ++cReachFix;
                        if (first) {
                            gInReachFixed.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (inReach && !visited) {
                        verdict = "B4CV_IN_REACH_NOT_VISITED";
                        ++cReachNotVisit;
                        if (first) {
                            gInReachOnlyNotVisited.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        // neither remset nor reach field set
                        verdict = "B4CV_NOT_IN_FIXSET";
                        ++cNotFixset;
                        if (first) {
                            gNotInFixset.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (!inRem) {
                            ++cNotRemset;
                            if (first) {
                                gNotInRemset.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }

                if (first && TryTakeDump()) {
                    size_t foff = BaseObject::FieldOffset(holder, &field);
                    B4CV_LOG("%s point=%s slot=%#zx holder=%p hName=%s foff=%zu val=%#zx vbase=%#zx voff=%zu "
                            "tip=%s inRem=%u inReach=%u visited=%u changed=%u wasGhost=%u wasOldTag=%u "
                            "visits=%u fixOld=%#zx fixNew=%#zx priorGen=%llu",
                            verdict, point == nullptr ? "?" : point, static_cast<size_t>(slot), holder, TipName(hTip),
                            foff, static_cast<size_t>(value), static_cast<size_t>(base), off, TipName(tip),
                            static_cast<unsigned>(inRem), static_cast<unsigned>(inReach),
                            static_cast<unsigned>(visited), static_cast<unsigned>(changed),
                            static_cast<unsigned>(wasGhost), static_cast<unsigned>(wasOldTag),
                            static_cast<unsigned>(visits), static_cast<size_t>(fixOld), static_cast<size_t>(fixNew),
                            static_cast<unsigned long long>(prior.gen));
                }
            });
        },
        false);

    B4CV_LOG("SCAN point=%s interior=%zu priorValid=%u "
            "IN_REMSET_NOT_FIXED=%zu IN_REMSET_FIXED=%zu IN_REMSET_NOT_VISITED=%zu "
            "IN_REACH_NOT_FIXED=%zu IN_REACH_FIXED=%zu IN_REACH_NOT_VISITED=%zu "
            "NOT_IN_FIXSET=%zu NOT_IN_REMSET=%zu NO_PRIOR=%zu",
            point == nullptr ? "?" : point, interior, static_cast<unsigned>(prior.valid), cRemNotFix, cRemFix,
            cRemNotVisit, cReachNotFix, cReachFix, cReachNotVisit, cNotFixset, cNotRemset, cNoPrior);
}

void B4CoverProbe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    B4CV_LOG("SUMMARY site=%s remsetNotes=%llu reachNotes=%llu fixVisits=%llu fixChanged=%llu scanCalls=%llu "
            "interiorHits=%llu "
            "uniq_IN_REMSET_NOT_FIXED=%llu uniq_IN_REMSET_FIXED=%llu uniq_IN_REMSET_NOT_VISITED=%llu "
            "uniq_IN_REACH_NOT_FIXED=%llu uniq_IN_REACH_FIXED=%llu uniq_IN_REACH_NOT_VISITED=%llu "
            "uniq_NOT_IN_FIXSET=%llu uniq_NOT_IN_REMSET=%llu uniq_NO_PRIOR=%llu",
            site == nullptr ? "?" : site, static_cast<unsigned long long>(gRemsetNote.load()),
            static_cast<unsigned long long>(gReachNote.load()), static_cast<unsigned long long>(gFixVisits.load()),
            static_cast<unsigned long long>(gFixChanged.load()), static_cast<unsigned long long>(gScanCalls.load()),
            static_cast<unsigned long long>(gInterior.load()),
            static_cast<unsigned long long>(gInRemsetNotFixed.load()),
            static_cast<unsigned long long>(gInRemsetFixed.load()),
            static_cast<unsigned long long>(gInRemsetOnlyNotVisited.load()),
            static_cast<unsigned long long>(gInReachNotFixed.load()),
            static_cast<unsigned long long>(gInReachFixed.load()),
            static_cast<unsigned long long>(gInReachOnlyNotVisited.load()),
            static_cast<unsigned long long>(gNotInFixset.load()),
            static_cast<unsigned long long>(gNotInRemset.load()),
            static_cast<unsigned long long>(gNoPriorLedger.load()));
}

} // namespace MapleRuntime
