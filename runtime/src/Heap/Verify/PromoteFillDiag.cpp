// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/PromoteFillDiag.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace PromoteFillDiag {
namespace {

constexpr size_t kRegionCap = 1u << 14; // 16384 region→liveInfo slots
constexpr size_t kSkipCap = 1u << 20;   // 1M skip-slot stamps
constexpr size_t kSampleLimit = 8;

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

// Open-address region → mark-time liveInfo pointer.
struct RegionLiveSlot {
    std::atomic<uintptr_t> region{ 0 };
    std::atomic<uintptr_t> liveInfo{ 0 };
};

RegionLiveSlot g_markLive[kRegionCap];
std::atomic<uint64_t> g_markNotes{ 0 };
std::atomic<uint64_t> g_markCollisions{ 0 };

// Skip-slot stamp: field address seen at promote dead-object skip.
struct SkipSlot {
    std::atomic<uintptr_t> field{ 0 };
};
SkipSlot g_skipSlots[kSkipCap];
std::atomic<uint64_t> g_skipSlotNotes{ 0 };
std::atomic<uint64_t> g_skipSlotCollisions{ 0 };

// Counters
std::atomic<uint64_t> g_enter{ 0 };
std::atomic<uint64_t> g_enterYoung{ 0 };
std::atomic<uint64_t> g_enterAuth{ 0 };
std::atomic<uint64_t> g_enterHasBm{ 0 };
std::atomic<uint64_t> g_enterLiveNull{ 0 };
std::atomic<uint64_t> g_enterLiveNonNull{ 0 };
std::atomic<uint64_t> g_modeLiveOnly{ 0 };
std::atomic<uint64_t> g_modeScanAll{ 0 };
std::atomic<uint64_t> g_recordedSum{ 0 };
std::atomic<uint64_t> g_safeEmptyHits{ 0 };
std::atomic<uint64_t> g_safeEmptyWithAlloc{ 0 };
std::atomic<uint64_t> g_safeEmptyLiveNull{ 0 };
std::atomic<uint64_t> g_safeEmptyLiveNonNull{ 0 };
std::atomic<uint64_t> g_safeEmptyLedgerSame{ 0 };
std::atomic<uint64_t> g_safeEmptyLedgerDiff{ 0 };
std::atomic<uint64_t> g_safeEmptyLedgerNoMark{ 0 };
std::atomic<uint64_t> g_safeEmptyLedgerMarkNull{ 0 };

std::atomic<uint64_t> g_skipDeadObjs{ 0 };
std::atomic<uint64_t> g_skipDeadYoungEdges{ 0 };
std::atomic<uint64_t> g_skipDeadLedgerSame{ 0 };
std::atomic<uint64_t> g_skipDeadLedgerDiff{ 0 };
std::atomic<uint64_t> g_skipDeadLedgerNoMark{ 0 };
std::atomic<uint64_t> g_skipDeadLedgerMarkNull{ 0 };
std::atomic<uint64_t> g_skipDeadSurvivedFalse{ 0 };
std::atomic<uint64_t> g_skipDeadLiveNull{ 0 };

std::atomic<uint64_t> g_censusNeverSeen{ 0 };
std::atomic<uint64_t> g_censusJoinSkip{ 0 };
std::atomic<uint64_t> g_censusJoinNoSkip{ 0 };

std::atomic<uint64_t> g_sampleSafe{ 0 };
std::atomic<uint64_t> g_sampleSkip{ 0 };
std::atomic<uint64_t> g_sampleJoin{ 0 };

size_t HashPtr(uintptr_t p)
{
    // splitmix64-ish
    p ^= p >> 30;
    p *= 0xbf58476d1ce4e5b9ull;
    p ^= p >> 27;
    p *= 0x94d049bb133111ebull;
    p ^= p >> 31;
    return static_cast<size_t>(p);
}

uintptr_t LookupMarkLive(RegionInfo* region)
{
    if (region == nullptr) {
        return 0;
    }
    uintptr_t key = reinterpret_cast<uintptr_t>(region);
    size_t idx = HashPtr(key) & (kRegionCap - 1);
    for (size_t p = 0; p < 32; ++p) {
        size_t i = (idx + p) & (kRegionCap - 1);
        uintptr_t cur = g_markLive[i].region.load(std::memory_order_acquire);
        if (cur == key) {
            return g_markLive[i].liveInfo.load(std::memory_order_acquire);
        }
        if (cur == 0) {
            return 0;
        }
    }
    return 0;
}

void StoreMarkLive(RegionInfo* region, void* liveInfoPtr)
{
    if (region == nullptr) {
        return;
    }
    uintptr_t key = reinterpret_cast<uintptr_t>(region);
    uintptr_t val = reinterpret_cast<uintptr_t>(liveInfoPtr);
    size_t idx = HashPtr(key) & (kRegionCap - 1);
    for (size_t p = 0; p < 32; ++p) {
        size_t i = (idx + p) & (kRegionCap - 1);
        uintptr_t cur = g_markLive[i].region.load(std::memory_order_acquire);
        if (cur == key) {
            g_markLive[i].liveInfo.store(val, std::memory_order_release);
            return;
        }
        if (cur == 0) {
            uintptr_t exp = 0;
            if (g_markLive[i].region.compare_exchange_strong(exp, key, std::memory_order_acq_rel)) {
                g_markLive[i].liveInfo.store(val, std::memory_order_release);
                g_markNotes.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (exp == key) {
                g_markLive[i].liveInfo.store(val, std::memory_order_release);
                return;
            }
        }
    }
    g_markCollisions.fetch_add(1, std::memory_order_relaxed);
}

bool StoreSkipSlot(MAddress fieldAddress)
{
    if (fieldAddress == 0) {
        return false;
    }
    uintptr_t key = static_cast<uintptr_t>(fieldAddress);
    size_t idx = HashPtr(key) & (kSkipCap - 1);
    for (size_t p = 0; p < 32; ++p) {
        size_t i = (idx + p) & (kSkipCap - 1);
        uintptr_t cur = g_skipSlots[i].field.load(std::memory_order_acquire);
        if (cur == key) {
            return true;
        }
        if (cur == 0) {
            uintptr_t exp = 0;
            if (g_skipSlots[i].field.compare_exchange_strong(exp, key, std::memory_order_acq_rel)) {
                g_skipSlotNotes.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (exp == key) {
                return true;
            }
        }
    }
    g_skipSlotCollisions.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool HasSkipSlot(MAddress fieldAddress)
{
    if (fieldAddress == 0) {
        return false;
    }
    uintptr_t key = static_cast<uintptr_t>(fieldAddress);
    size_t idx = HashPtr(key) & (kSkipCap - 1);
    for (size_t p = 0; p < 32; ++p) {
        size_t i = (idx + p) & (kSkipCap - 1);
        uintptr_t cur = g_skipSlots[i].field.load(std::memory_order_acquire);
        if (cur == key) {
            return true;
        }
        if (cur == 0) {
            return false;
        }
    }
    return false;
}

enum class LedgerClass : uint8_t {
    SAME = 0,
    DIFF = 1,
    NO_MARK = 2,
    MARK_NULL = 3,
};

// hasMarkNote=false → NO_MARK (region never painted / table miss).
// hasMarkNote=true  → SAME iff promoteLive pointer equals mark-time pointer (incl. both null).
LedgerClass ClassifyLedger2(void* promoteLive, bool hasMarkNote, uintptr_t markLive)
{
    if (!hasMarkNote) {
        return LedgerClass::NO_MARK;
    }
    uintptr_t p = reinterpret_cast<uintptr_t>(promoteLive);
    if (markLive == 0) {
        return p == 0 ? LedgerClass::SAME : LedgerClass::DIFF;
    }
    return p == markLive ? LedgerClass::SAME : LedgerClass::DIFF;
}

bool LookupMarkLiveEx(RegionInfo* region, uintptr_t* outLive, bool* found)
{
    *outLive = 0;
    *found = false;
    if (region == nullptr) {
        return false;
    }
    uintptr_t key = reinterpret_cast<uintptr_t>(region);
    size_t idx = HashPtr(key) & (kRegionCap - 1);
    for (size_t p = 0; p < 32; ++p) {
        size_t i = (idx + p) & (kRegionCap - 1);
        uintptr_t cur = g_markLive[i].region.load(std::memory_order_acquire);
        if (cur == key) {
            *outLive = g_markLive[i].liveInfo.load(std::memory_order_acquire);
            *found = true;
            return true;
        }
        if (cur == 0) {
            return false;
        }
    }
    return false;
}

void AccountLedgerSafe(LedgerClass c)
{
    switch (c) {
        case LedgerClass::SAME:
            g_safeEmptyLedgerSame.fetch_add(1, std::memory_order_relaxed);
            break;
        case LedgerClass::DIFF:
            g_safeEmptyLedgerDiff.fetch_add(1, std::memory_order_relaxed);
            break;
        case LedgerClass::NO_MARK:
            g_safeEmptyLedgerNoMark.fetch_add(1, std::memory_order_relaxed);
            break;
        case LedgerClass::MARK_NULL:
            g_safeEmptyLedgerMarkNull.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

void AccountLedgerSkip(LedgerClass c)
{
    switch (c) {
        case LedgerClass::SAME:
            g_skipDeadLedgerSame.fetch_add(1, std::memory_order_relaxed);
            break;
        case LedgerClass::DIFF:
            g_skipDeadLedgerDiff.fetch_add(1, std::memory_order_relaxed);
            break;
        case LedgerClass::NO_MARK:
            g_skipDeadLedgerNoMark.fetch_add(1, std::memory_order_relaxed);
            break;
        case LedgerClass::MARK_NULL:
            g_skipDeadLedgerMarkNull.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

} // namespace

bool Enabled()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        return DiagGate::LegacyOrToken("MRT_GCV2_PROMOTEFILL", "promotefill");
    }();
    return on;
}

void NoteMarkLiveInfo(RegionInfo* region, void* liveInfoPtr)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    StoreMarkLive(region, liveInfoPtr);
}

void NotePromoteFillEnter(RegionInfo* region, void* liveInfoAtPromote, void* /*liveInfo0AtPromote*/,
                          unsigned isYoung, unsigned auth, unsigned hasBitmap)
{
    if (!Enabled()) {
        return;
    }
    g_enter.fetch_add(1, std::memory_order_relaxed);
    if (isYoung) {
        g_enterYoung.fetch_add(1, std::memory_order_relaxed);
    }
    if (auth) {
        g_enterAuth.fetch_add(1, std::memory_order_relaxed);
    }
    if (hasBitmap) {
        g_enterHasBm.fetch_add(1, std::memory_order_relaxed);
    }
    if (liveInfoAtPromote == nullptr) {
        g_enterLiveNull.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_enterLiveNonNull.fetch_add(1, std::memory_order_relaxed);
    }
    (void)region;
}

void NotePromoteFillMode(unsigned useLiveOnly, size_t recorded)
{
    if (!Enabled()) {
        return;
    }
    if (useLiveOnly) {
        g_modeLiveOnly.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_modeScanAll.fetch_add(1, std::memory_order_relaxed);
    }
    g_recordedSum.fetch_add(recorded, std::memory_order_relaxed);
}

void NoteSafeKnownEmpty(RegionInfo* region, void* liveInfoAtPromote, void* liveInfo0AtPromote,
                        uint64_t liveByteRaw, unsigned hasBitmap, unsigned isLarge, size_t regionBytes)
{
    if (!Enabled()) {
        return;
    }
    g_safeEmptyHits.fetch_add(1, std::memory_order_relaxed);
    if (regionBytes > 0) {
        g_safeEmptyWithAlloc.fetch_add(1, std::memory_order_relaxed);
    }
    if (liveInfoAtPromote == nullptr) {
        g_safeEmptyLiveNull.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_safeEmptyLiveNonNull.fetch_add(1, std::memory_order_relaxed);
    }
    uintptr_t markLive = 0;
    bool found = false;
    (void)LookupMarkLiveEx(region, &markLive, &found);
    LedgerClass lc = ClassifyLedger2(liveInfoAtPromote, found, markLive);
    AccountLedgerSafe(lc);

    size_t sampleCap = EnvSizeT("MRT_GCV2_PROMOTEFILL_SAMPLES", kSampleLimit);
    uint64_t n = g_sampleSafe.fetch_add(1, std::memory_order_relaxed);
    if (n < sampleCap) {
        VLOG(REPORT,
             "[PROMOTEFILL][safe-empty] n=%llu region=%p liveInfo=%p liveInfo0=%p markLive=%p "
             "ledger=%s liveRaw=%llx hasBm=%u large=%u regionBytes=%zu",
             static_cast<unsigned long long>(n + 1), region, liveInfoAtPromote, liveInfo0AtPromote,
             reinterpret_cast<void*>(markLive),
             lc == LedgerClass::SAME     ? "SAME"
             : lc == LedgerClass::DIFF   ? "DIFF"
             : lc == LedgerClass::NO_MARK ? "NO_MARK"
                                           : "MARK_NULL",
             static_cast<unsigned long long>(liveByteRaw), hasBitmap, isLarge, regionBytes);
    }
}

void NoteSkipSlot(MAddress fieldAddress, RegionInfo* /*holderRegion*/, BaseObject* /*holder*/)
{
    if (!Enabled() || fieldAddress == 0) {
        return;
    }
    (void)StoreSkipSlot(fieldAddress);
}

void NoteSkipDeadObject(RegionInfo* region, BaseObject* object, void* liveInfoAtPromote,
                        void* liveInfo0AtPromote, size_t offset, unsigned survivedBit,
                        unsigned useLiveOnly, unsigned hasObjectLiveness)
{
    if (!Enabled() || object == nullptr) {
        return;
    }
    g_skipDeadObjs.fetch_add(1, std::memory_order_relaxed);
    if (survivedBit == 0) {
        g_skipDeadSurvivedFalse.fetch_add(1, std::memory_order_relaxed);
    }
    if (liveInfoAtPromote == nullptr) {
        g_skipDeadLiveNull.fetch_add(1, std::memory_order_relaxed);
    }
    uintptr_t markLive = 0;
    bool found = false;
    (void)LookupMarkLiveEx(region, &markLive, &found);
    LedgerClass lc = ClassifyLedger2(liveInfoAtPromote, found, markLive);
    AccountLedgerSkip(lc);

    size_t youngEdges = 0;
    object->ForEachRefField([&youngEdges, region, object](RefField<>& field) {
        BaseObject* target = to_object(field.GetTargetObject());
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            return;
        }
        RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
            ++youngEdges;
            NoteSkipSlot(reinterpret_cast<MAddress>(&field), region, object);
        }
    });
    if (youngEdges != 0) {
        g_skipDeadYoungEdges.fetch_add(youngEdges, std::memory_order_relaxed);
    }

    size_t sampleCap = EnvSizeT("MRT_GCV2_PROMOTEFILL_SAMPLES", kSampleLimit);
    uint64_t n = g_sampleSkip.fetch_add(1, std::memory_order_relaxed);
    if (n < sampleCap) {
        VLOG(REPORT,
             "[PROMOTEFILL][skip-dead] n=%llu region=%p obj=%p off=%zu liveInfo=%p liveInfo0=%p "
             "markLive=%p ledger=%s survived=%u useLiveOnly=%u hasLiv=%u youngEdges=%zu",
             static_cast<unsigned long long>(n + 1), region, object, offset, liveInfoAtPromote,
             liveInfo0AtPromote, reinterpret_cast<void*>(markLive),
             lc == LedgerClass::SAME     ? "SAME"
             : lc == LedgerClass::DIFF   ? "DIFF"
             : lc == LedgerClass::NO_MARK ? "NO_MARK"
                                           : "MARK_NULL",
             survivedBit, useLiveOnly, hasObjectLiveness, youngEdges);
    }
}

bool NoteCensusNeverSeen(MAddress fieldAddress)
{
    if (!Enabled() || fieldAddress == 0) {
        return false;
    }
    g_censusNeverSeen.fetch_add(1, std::memory_order_relaxed);
    if (HasSkipSlot(fieldAddress)) {
        g_censusJoinSkip.fetch_add(1, std::memory_order_relaxed);
        size_t sampleCap = EnvSizeT("MRT_GCV2_PROMOTEFILL_SAMPLES", kSampleLimit);
        uint64_t n = g_sampleJoin.fetch_add(1, std::memory_order_relaxed);
        if (n < sampleCap) {
            VLOG(REPORT, "[PROMOTEFILL][causal-join] n=%llu slot=%p neverSeen∩promoteSkip=1",
                 static_cast<unsigned long long>(n + 1), reinterpret_cast<void*>(fieldAddress));
        }
        return true;
    }
    g_censusJoinNoSkip.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void DumpProcessTotals(const char* tag)
{
    if (!Enabled()) {
        return;
    }
    VLOG(REPORT,
         "[PROMOTEFILL][TOTAL] tag=%s enter=%llu young=%llu auth=%llu hasBm=%llu liveNull=%llu "
         "liveNonNull=%llu modeLiveOnly=%llu modeScanAll=%llu recordedSum=%llu | "
         "safeEmpty=%llu (withAlloc=%llu liveNull=%llu liveNonNull=%llu) "
         "ledgerSafe same=%llu diff=%llu noMark=%llu | skipDeadObjs=%llu youngEdges=%llu "
         "survivedFalse=%llu liveNull=%llu ledgerSkip same=%llu diff=%llu noMark=%llu | "
         "censusNeverSeen=%llu joinSkip=%llu joinNoSkip=%llu | markNotes=%llu skipSlotNotes=%llu "
         "markColl=%llu skipColl=%llu",
         tag == nullptr ? "-" : tag,
         static_cast<unsigned long long>(g_enter.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_enterYoung.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_enterAuth.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_enterHasBm.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_enterLiveNull.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_enterLiveNonNull.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_modeLiveOnly.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_modeScanAll.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_recordedSum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_safeEmptyHits.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_safeEmptyWithAlloc.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_safeEmptyLiveNull.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_safeEmptyLiveNonNull.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_safeEmptyLedgerSame.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_safeEmptyLedgerDiff.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_safeEmptyLedgerNoMark.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipDeadObjs.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipDeadYoungEdges.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipDeadSurvivedFalse.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipDeadLiveNull.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipDeadLedgerSame.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipDeadLedgerDiff.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipDeadLedgerNoMark.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_censusNeverSeen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_censusJoinSkip.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_censusJoinNoSkip.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_markNotes.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipSlotNotes.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_markCollisions.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipSlotCollisions.load(std::memory_order_relaxed)));
}

} // namespace PromoteFillDiag
} // namespace MapleRuntime
