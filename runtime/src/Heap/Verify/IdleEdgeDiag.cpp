// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/IdleEdgeDiag.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace IdleEdgeDiag {
namespace {

constexpr size_t kSampleLimit = 8;
constexpr size_t kStampCapDefault = 1u << 18;
constexpr size_t kProbeMax = 32;
constexpr size_t kPhaseBuckets = 16;
constexpr size_t kAttrBuckets = 64;
constexpr size_t kAttrNameLen = 48;
constexpr size_t kRaBuckets = 64;

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

const char* PhaseName(uint8_t p)
{
    switch (static_cast<GCPhase>(p)) {
        case GC_PHASE_UNDEF:
            return "UNDEF";
        case GC_PHASE_IDLE:
            return "IDLE";
        case GC_PHASE_FINISH:
            return "FINISH";
        case GC_PHASE_RECLAIM_SATB_NODE:
            return "RECLAIM_SATB";
        case GC_PHASE_INIT:
            return "INIT";
        case GC_PHASE_ENUM:
            return "ENUM";
        case GC_PHASE_TRACE:
            return "TRACE";
        case GC_PHASE_CLEAR_SATB_BUFFER:
            return "CLEAR_SATB";
        case GC_PHASE_POST_TRACE:
            return "POST_TRACE";
        case GC_PHASE_PREFORWARD:
            return "PREFORWARD";
        case GC_PHASE_FORWARD:
            return "FORWARD";
        default:
            return "OTHER";
    }
}

// Gen encoding for stamp / miss attribution (promoteedge).
constexpr uint8_t kGenUnknown = 0;
constexpr uint8_t kGenYoung = 1;
constexpr uint8_t kGenOld = 2;
constexpr uint8_t kGenNonHeap = 3;

// idlewrite skip-arm codes (mirror RemsetPhaseProbe::SkipReason).
constexpr uint8_t kReasonRecorded = 0;
constexpr uint8_t kReasonNoYoung = 1;
constexpr uint8_t kReasonRefNullOrNonheap = 2;
constexpr uint8_t kReasonRefNotYoung = 3;
constexpr uint8_t kReasonHolderNullOrNonheap = 4;
constexpr uint8_t kReasonHolderYoung = 5;
constexpr uint8_t kReasonUnknown = 6;
constexpr uint8_t kReasonNoStamp = 7;
constexpr size_t kReasonBuckets = 8;

struct StampSlot {
    std::atomic<uintptr_t> field{ 0 };
    std::atomic<uint8_t> phase{ 0 };
    std::atomic<uint8_t> recorded{ 0 };
    std::atomic<uint8_t> holderGen{ 0 };
    std::atomic<uint8_t> targetGen{ 0 };
    std::atomic<uint8_t> skipReason{ 0 };
};

StampSlot* g_stamps = nullptr;
size_t g_stampCap = 0;
size_t g_stampMask = 0;
std::atomic<uint32_t> g_stampInited{ 0 };

std::atomic<uint64_t> g_stampNotes{ 0 };
std::atomic<uint64_t> g_stampWraps{ 0 };
std::atomic<uint64_t> g_stampProbeFail{ 0 };
// stampfix: process totals for eviction / clear; epoch-local for HEALTH.
std::atomic<uint64_t> g_stampEvicted{ 0 };
std::atomic<uint64_t> g_stampClears{ 0 };
std::atomic<uint64_t> g_stampEpochNotes{ 0 };
std::atomic<uint64_t> g_stampEpochWraps{ 0 };
std::atomic<uint64_t> g_stampEpochProbeFail{ 0 };
std::atomic<uint64_t> g_stampEpochEvicted{ 0 };
std::atomic<uint32_t> g_stampEpoch{ 0 };

// Open-address set of field keys that lost their stamp to force-overwrite this epoch.
// Used to split bare no_stamp into never_seen vs displaced (idlewrite deliverable).
constexpr size_t kEvictCap = 1u << 16;
constexpr size_t kEvictMask = kEvictCap - 1;
constexpr size_t kEvictProbe = 16;
std::atomic<uintptr_t> g_evictKeys[kEvictCap] = {};
std::atomic<uint64_t> g_evictNotes{ 0 };
std::atomic<uint64_t> g_evictOverflow{ 0 };

std::atomic<uint64_t> g_minorsCensused{ 0 };
std::atomic<uint64_t> g_edgesTotal{ 0 };
std::atomic<uint64_t> g_remsetHit{ 0 };
std::atomic<uint64_t> g_remsetMiss{ 0 };
std::atomic<uint64_t> g_missBare{ 0 };
// stampfix split of missBare / no_stamp (sum == missBare).
std::atomic<uint64_t> g_missBareNeverSeen{ 0 };
std::atomic<uint64_t> g_missBareDisplaced{ 0 };
std::atomic<uint64_t> g_missPhaseLe8{ 0 };
std::atomic<uint64_t> g_missPhaseGt8{ 0 };
std::atomic<uint64_t> g_missRecordedLost{ 0 };
std::atomic<uint64_t> g_missEarly{ 0 };
std::array<std::atomic<uint64_t>, kPhaseBuckets> g_missByPhase{};
std::atomic<uint64_t> g_costNsTotal{ 0 };

struct AttrSlot {
    std::atomic<uint64_t> count{ 0 };
    char name[kAttrNameLen]{};
    std::atomic<uint8_t> used{ 0 };
    std::atomic<uint8_t> isRawArray{ 0 };
};
AttrSlot g_attr[kAttrBuckets] = {};
std::atomic<uint64_t> g_attrRawArray{ 0 };
std::atomic<uint64_t> g_attrOther{ 0 };
std::atomic<uint64_t> g_attrUnknown{ 0 };

// Bare miss by (holder type, slot offset relative to holder). Same gate as BARE_BY_TYPE.
constexpr size_t kOffBuckets = 128;
struct OffSlot {
    std::atomic<uint64_t> count{ 0 };
    char name[kAttrNameLen]{};
    std::atomic<uint32_t> offset{ 0 };
    std::atomic<uint8_t> used{ 0 };
    std::atomic<uint8_t> isRawArray{ 0 };
};
OffSlot g_off[kOffBuckets] = {};
std::atomic<uint64_t> g_offUnknown{ 0 };
std::atomic<uint64_t> g_offNeg{ 0 };

// Write-time generation of holder/target on missBare (promoteedge hypothesis).
// Index: 0=unknown 1=young 2=old 3=nonheap
std::atomic<uint64_t> g_bareHolderGen[4] = {};
std::atomic<uint64_t> g_bareTargetGen[4] = {};
std::atomic<uint64_t> g_missHolderGen[4] = {};
std::atomic<uint64_t> g_missTargetGen[4] = {};
// Decision-time totals (all barrier visits under idleedge, not only miss).
std::atomic<uint64_t> g_decHolderYoung{ 0 };
std::atomic<uint64_t> g_decHolderOld{ 0 };
std::atomic<uint64_t> g_decHolderOther{ 0 };
std::atomic<uint64_t> g_decTargetYoung{ 0 };
std::atomic<uint64_t> g_decTargetOld{ 0 };
std::atomic<uint64_t> g_decTargetOther{ 0 };
std::atomic<uint64_t> g_decRecorded{ 0 };
std::atomic<uint64_t> g_decSkippedHolderYoung{ 0 };
// idlewrite: decision-time skip-arm totals + field-vs-obj gen mismatch.
std::array<std::atomic<uint64_t>, kReasonBuckets> g_decByReason{};
std::atomic<uint64_t> g_decGenMismatch{ 0 }; // fieldGen != objGen (both known)
std::atomic<uint64_t> g_decGenMismatchHolderYoung{ 0 }; // field young, obj old (or vice versa)
// miss-side skip-arm attribution (stamped misses; bare → reason no_stamp)
std::array<std::atomic<uint64_t>, kReasonBuckets> g_missByReason{};
std::array<std::atomic<uint64_t>, kReasonBuckets> g_bareByReason{}; // bare always no_stamp, kept for shape
// stamped miss where write-time holderGen was young (hypothesis: barrier saw young)
std::atomic<uint64_t> g_missReasonHolderYoung{ 0 };
std::atomic<uint64_t> g_missReasonNoYoung{ 0 };
std::atomic<uint64_t> g_missReasonOther{ 0 };
// gen mismatch observed on stamped miss
std::atomic<uint64_t> g_missGenMismatch{ 0 };

// fullclear: promote-time target gen stamp vs census-time target gen on miss.
// Gate MRT_GCV2_FULLCLEAR_PROBE=1. Stamp lives across full clear (like write stamp).
struct PromoteStampSlot {
    std::atomic<uintptr_t> field{ 0 };
    std::atomic<uint8_t> targetGen{ 0 };
    std::atomic<uint8_t> recorded{ 0 };
};
PromoteStampSlot* g_promoteStamps = nullptr;
size_t g_promoteStampCap = 0;
size_t g_promoteStampMask = 0;
std::atomic<uint32_t> g_promoteStampInited{ 0 };
std::atomic<uint64_t> g_promoteStampNotes{ 0 };
std::atomic<uint64_t> g_promoteStampWraps{ 0 };
std::atomic<uint64_t> g_promoteStampProbeFail{ 0 };
// census miss matrix: promoteGen × censusGen (4×4) + promote-recorded flags
std::atomic<uint64_t> g_fcMissPromoteXCensus[4][4] = {};
std::atomic<uint64_t> g_fcMissPromoteRec[4] = {};
std::atomic<uint64_t> g_fcMissPromoteSkip[4] = {};
std::atomic<uint64_t> g_fcMissNoPromoteStamp{ 0 };
std::atomic<uint64_t> g_fcBarePromoteXCensus[4][4] = {};
std::atomic<uint64_t> g_fcBareNoPromoteStamp{ 0 };
std::atomic<uint64_t> g_fcCensusMissTotal{ 0 };
std::atomic<uint64_t> g_fcCensusBareTotal{ 0 };
// promote-time only (all walks, not only miss)
std::atomic<uint64_t> g_fcPromoteTargetGen[4] = {};
std::atomic<uint64_t> g_fcPromoteRecorded{ 0 };
std::atomic<uint64_t> g_fcPromoteSeen{ 0 };

struct RaSlot {
    std::atomic<uintptr_t> pc{ 0 };
    std::atomic<uint64_t> count{ 0 };
};
RaSlot g_ra[kRaBuckets] = {};
std::atomic<uint64_t> g_raNotes{ 0 };

void EnsureStampTable()
{
    if (g_stampInited.load(std::memory_order_acquire) == 1) {
        return;
    }
    uint32_t expected = 0;
    if (!g_stampInited.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
        while (g_stampInited.load(std::memory_order_acquire) != 1) {
        }
        return;
    }
    size_t bits = EnvSizeT("MRT_GCV2_IDLEEDGE_STAMP_BITS", 18);
    if (bits < 16) {
        bits = 16;
    }
    if (bits > 22) {
        bits = 22;
    }
    g_stampCap = size_t(1) << bits;
    g_stampMask = g_stampCap - 1;
    g_stamps = new (std::nothrow) StampSlot[g_stampCap];
    if (g_stamps == nullptr) {
        static StampSlot fallback[kStampCapDefault];
        g_stamps = fallback;
        g_stampCap = kStampCapDefault;
        g_stampMask = g_stampCap - 1;
    }
    g_stampInited.store(1, std::memory_order_release);
}

void ClearEvictSet()
{
    for (size_t i = 0; i < kEvictCap; ++i) {
        g_evictKeys[i].store(0, std::memory_order_relaxed);
    }
}

void NoteEvictedKey(uintptr_t key)
{
    if (key == 0) {
        return;
    }
    g_stampEvicted.fetch_add(1, std::memory_order_relaxed);
    g_stampEpochEvicted.fetch_add(1, std::memory_order_relaxed);
    size_t idx0 = (static_cast<size_t>(key >> 3) * 0x9e3779b97f4a7c15ull) & kEvictMask;
    for (size_t p = 0; p < kEvictProbe; ++p) {
        size_t idx = (idx0 + p) & kEvictMask;
        uintptr_t cur = g_evictKeys[idx].load(std::memory_order_relaxed);
        if (cur == key) {
            return;
        }
        if (cur == 0) {
            uintptr_t expected = 0;
            if (g_evictKeys[idx].compare_exchange_strong(expected, key, std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {
                g_evictNotes.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (expected == key) {
                return;
            }
            continue;
        }
    }
    g_evictOverflow.fetch_add(1, std::memory_order_relaxed);
}

bool WasEvicted(uintptr_t key)
{
    if (key == 0) {
        return false;
    }
    size_t idx0 = (static_cast<size_t>(key >> 3) * 0x9e3779b97f4a7c15ull) & kEvictMask;
    for (size_t p = 0; p < kEvictProbe; ++p) {
        size_t idx = (idx0 + p) & kEvictMask;
        uintptr_t cur = g_evictKeys[idx].load(std::memory_order_relaxed);
        if (cur == 0) {
            return false;
        }
        if (cur == key) {
            return true;
        }
    }
    return false;
}

// stampfix path 甲: drop write stamps after each census so the table only
// holds one minor of barrier decisions. Process totals (g_stampNotes etc.) stay.
// Called only from CensusPrePinnedStamp after ClassifyMiss (STW).
void ClearWriteStampsAfterCensus()
{
    if (g_stamps == nullptr || g_stampCap == 0) {
        return;
    }
    for (size_t i = 0; i < g_stampCap; ++i) {
        StampSlot& slot = g_stamps[i];
        slot.field.store(0, std::memory_order_relaxed);
        slot.phase.store(0, std::memory_order_relaxed);
        slot.recorded.store(0, std::memory_order_relaxed);
        slot.holderGen.store(0, std::memory_order_relaxed);
        slot.targetGen.store(0, std::memory_order_relaxed);
        slot.skipReason.store(0, std::memory_order_relaxed);
    }
    ClearEvictSet();
    g_stampEpochNotes.store(0, std::memory_order_relaxed);
    g_stampEpochWraps.store(0, std::memory_order_relaxed);
    g_stampEpochProbeFail.store(0, std::memory_order_relaxed);
    g_stampEpochEvicted.store(0, std::memory_order_relaxed);
    g_stampClears.fetch_add(1, std::memory_order_relaxed);
    g_stampEpoch.fetch_add(1, std::memory_order_relaxed);
}

bool FullClearProbeOn()
{
    return false;
}

// Scan open-address stamp table for occupancy (instrument health, not product).
size_t CountStampOccupied()
{
    if (g_stamps == nullptr || g_stampCap == 0) {
        return 0;
    }
    size_t occ = 0;
    for (size_t i = 0; i < g_stampCap; ++i) {
        if (g_stamps[i].field.load(std::memory_order_relaxed) != 0) {
            ++occ;
        }
    }
    return occ;
}

void EmitInstrumentHealth(const char* where, size_t remsetSize, size_t oldToYoungEdges)
{
    size_t occ = CountStampOccupied();
    double occPct = g_stampCap == 0 ? 0.0 : 100.0 * static_cast<double>(occ) / static_cast<double>(g_stampCap);
    // Prefer epoch (since last clear) for saturation. After a clear, epoch is empty and
    // occ is 0 — that is healthy, not saturated. Never fall back to process totals for
    // the sat decision: selftest flood would otherwise poison every post-clear HEALTH.
    uint64_t epochFail = g_stampEpochProbeFail.load(std::memory_order_relaxed);
    uint64_t epochWraps = g_stampEpochWraps.load(std::memory_order_relaxed);
    uint64_t epochNotes = g_stampEpochNotes.load(std::memory_order_relaxed);
    uint64_t epochEvict = g_stampEpochEvicted.load(std::memory_order_relaxed);
    uint64_t probeFail = g_stampProbeFail.load(std::memory_order_relaxed);
    uint64_t wraps = g_stampWraps.load(std::memory_order_relaxed);
    uint64_t notes = g_stampNotes.load(std::memory_order_relaxed);
    bool saturated = (occPct > 50.0) ||
        (epochNotes > 0 && epochFail > 0 && epochFail * 100 > epochNotes);
    // HEALTH is RTLOG_ERROR so it matches progress volume (VLOG(REPORT) is file-gated
    // and silent under DEFAULT_MRT_REPORT=0 — that hid table saturation for a whole night).
    LOG(RTLOG_ERROR,
        "[GCV2][diag][HEALTH] where=%s stampCap=%zu stampOcc=%zu stampOccPct=%.1f "
        "stampNotes=%llu stampWraps=%llu stampProbeFail=%llu stampEpochNotes=%llu "
        "stampEpochProbeFail=%llu stampEpochEvicted=%llu stampClears=%llu stampEpoch=%u "
        "remsetSize=%zu oldToYoungEdges=%zu trustworthy=%s",
        where == nullptr ? "?" : where, g_stampCap, occ, occPct,
        static_cast<unsigned long long>(notes), static_cast<unsigned long long>(wraps),
        static_cast<unsigned long long>(probeFail), static_cast<unsigned long long>(epochNotes),
        static_cast<unsigned long long>(epochFail), static_cast<unsigned long long>(epochEvict),
        static_cast<unsigned long long>(g_stampClears.load(std::memory_order_relaxed)),
        g_stampEpoch.load(std::memory_order_relaxed), remsetSize, oldToYoungEdges,
        saturated ? "NO" : "YES");
    if (saturated) {
        LOG(RTLOG_ERROR,
            "[GCV2][diag][INSTRUMENT_SATURATED] where=%s stampOccPct=%.1f (threshold 50) "
            "stampProbeFail=%llu stampWraps=%llu stampCap=%zu stampEpochEvicted=%llu "
            "ACTION=raise MRT_GCV2_IDLEEDGE_STAMP_BITS (max 22) or distrust missBare reclass "
            "(per-minor clear is on; if still saturated within one minor, table too small)",
            where == nullptr ? "?" : where, occPct, static_cast<unsigned long long>(probeFail),
            static_cast<unsigned long long>(wraps), g_stampCap,
            static_cast<unsigned long long>(epochEvict));
        LOG(RTLOG_ERROR,
            "[GCV2][diag][REFUSE_NUMBERS] where=%s reason=stamp_saturated "
            "missBare_reclass_untrustworthy=1 bareNeverSeen_vs_displaced_partial",
            where == nullptr ? "?" : where);
    }
}

size_t HashField(MAddress field);

void EnsurePromoteStampTable()
{
    if (g_promoteStampInited.load(std::memory_order_acquire) == 1) {
        return;
    }
    uint32_t expected = 0;
    if (!g_promoteStampInited.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
        while (g_promoteStampInited.load(std::memory_order_acquire) != 1) {
        }
        return;
    }
    size_t bits = EnvSizeT("MRT_GCV2_IDLEEDGE_STAMP_BITS", 18);
    if (bits < 16) {
        bits = 16;
    }
    if (bits > 22) {
        bits = 22;
    }
    g_promoteStampCap = size_t(1) << bits;
    g_promoteStampMask = g_promoteStampCap - 1;
    g_promoteStamps = new (std::nothrow) PromoteStampSlot[g_promoteStampCap];
    if (g_promoteStamps == nullptr) {
        static PromoteStampSlot fallback[kStampCapDefault];
        g_promoteStamps = fallback;
        g_promoteStampCap = kStampCapDefault;
        g_promoteStampMask = g_promoteStampCap - 1;
    }
    g_promoteStampInited.store(1, std::memory_order_release);
}

void StorePromoteStamp(MAddress fieldAddress, uint8_t targetGen, bool recorded)
{
    EnsurePromoteStampTable();
    if (g_promoteStamps == nullptr || g_promoteStampCap == 0) {
        return;
    }
    size_t idx = HashField(fieldAddress) & g_promoteStampMask;
    for (size_t probe = 0; probe < kProbeMax; ++probe) {
        size_t i = (idx + probe) & g_promoteStampMask;
        PromoteStampSlot& slot = g_promoteStamps[i];
        uintptr_t cur = slot.field.load(std::memory_order_acquire);
        if (cur == 0) {
            uintptr_t expected = 0;
            if (slot.field.compare_exchange_strong(expected, fieldAddress, std::memory_order_acq_rel)) {
                slot.targetGen.store(targetGen, std::memory_order_relaxed);
                slot.recorded.store(recorded ? 1 : 0, std::memory_order_relaxed);
                g_promoteStampNotes.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            cur = expected;
        }
        if (cur == fieldAddress) {
            slot.targetGen.store(targetGen, std::memory_order_relaxed);
            slot.recorded.store(recorded ? 1 : 0, std::memory_order_relaxed);
            g_promoteStampWraps.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_promoteStampProbeFail.fetch_add(1, std::memory_order_relaxed);
}

struct PromoteStampLookup {
    bool found = false;
    uint8_t targetGen = 0;
    bool recorded = false;
};

PromoteStampLookup LoadPromoteStamp(MAddress fieldAddress)
{
    PromoteStampLookup r;
    if (g_promoteStamps == nullptr || g_promoteStampCap == 0) {
        return r;
    }
    size_t idx = HashField(fieldAddress) & g_promoteStampMask;
    for (size_t probe = 0; probe < kProbeMax; ++probe) {
        size_t i = (idx + probe) & g_promoteStampMask;
        PromoteStampSlot& slot = g_promoteStamps[i];
        uintptr_t cur = slot.field.load(std::memory_order_acquire);
        if (cur == 0) {
            return r;
        }
        if (cur == fieldAddress) {
            r.found = true;
            r.targetGen = slot.targetGen.load(std::memory_order_relaxed);
            r.recorded = slot.recorded.load(std::memory_order_relaxed) != 0;
            return r;
        }
    }
    return r;
}

uint8_t CensusTargetGenOf(BaseObject* target)
{
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return kGenNonHeap;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    if (region == nullptr) {
        return kGenUnknown;
    }
    return region->IsYoungRegion() ? kGenYoung : kGenOld;
}

void NoteFullClearMiss(MAddress fieldAddress, BaseObject* target, bool bare)
{
    if (!FullClearProbeOn()) {
        return;
    }
    EnsurePromoteStampTable();
    uint8_t censusGen = CensusTargetGenOf(target);
    if (censusGen > 3) {
        censusGen = kGenUnknown;
    }
    g_fcCensusMissTotal.fetch_add(1, std::memory_order_relaxed);
    if (bare) {
        g_fcCensusBareTotal.fetch_add(1, std::memory_order_relaxed);
    }
    PromoteStampLookup st = LoadPromoteStamp(fieldAddress);
    if (!st.found) {
        g_fcMissNoPromoteStamp.fetch_add(1, std::memory_order_relaxed);
        if (bare) {
            g_fcBareNoPromoteStamp.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    uint8_t pg = st.targetGen;
    if (pg > 3) {
        pg = kGenUnknown;
    }
    g_fcMissPromoteXCensus[pg][censusGen].fetch_add(1, std::memory_order_relaxed);
    if (st.recorded) {
        g_fcMissPromoteRec[pg].fetch_add(1, std::memory_order_relaxed);
    } else {
        g_fcMissPromoteSkip[pg].fetch_add(1, std::memory_order_relaxed);
    }
    if (bare) {
        g_fcBarePromoteXCensus[pg][censusGen].fetch_add(1, std::memory_order_relaxed);
    }
}

size_t HashField(MAddress field)
{
    uintptr_t x = static_cast<uintptr_t>(field) >> 3;
    x ^= x >> 17;
    x *= 0x9e3779b97f4a7c15ull;
    x ^= x >> 13;
    x *= 0xc2b2ae3d27d4eb4full;
    return static_cast<size_t>(x) & g_stampMask;
}

void StoreStamp(MAddress fieldAddress, uint8_t phase, bool recorded, uint8_t holderGen, uint8_t targetGen,
                uint8_t skipReason = 0)
{
    EnsureStampTable();
    const uintptr_t key = static_cast<uintptr_t>(fieldAddress);
    size_t idx0 = HashField(fieldAddress);
    auto writeFields = [&](StampSlot& slot) {
        slot.phase.store(phase, std::memory_order_relaxed);
        slot.recorded.store(recorded ? 1 : 0, std::memory_order_relaxed);
        slot.holderGen.store(holderGen, std::memory_order_relaxed);
        slot.targetGen.store(targetGen, std::memory_order_relaxed);
        slot.skipReason.store(skipReason, std::memory_order_relaxed);
        g_stampNotes.fetch_add(1, std::memory_order_relaxed);
        g_stampEpochNotes.fetch_add(1, std::memory_order_relaxed);
    };
    for (size_t p = 0; p < kProbeMax; ++p) {
        size_t idx = (idx0 + p) & g_stampMask;
        StampSlot& slot = g_stamps[idx];
        uintptr_t prev = slot.field.load(std::memory_order_relaxed);
        if (prev == key) {
            writeFields(slot);
            return;
        }
        if (prev == 0) {
            uintptr_t expected = 0;
            if (slot.field.compare_exchange_strong(expected, key, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                writeFields(slot);
                return;
            }
            if (expected == key) {
                writeFields(slot);
                return;
            }
            g_stampWraps.fetch_add(1, std::memory_order_relaxed);
            g_stampEpochWraps.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        g_stampWraps.fetch_add(1, std::memory_order_relaxed);
        g_stampEpochWraps.fetch_add(1, std::memory_order_relaxed);
    }
    // Force-overwrite home slot: previous key is displaced (no longer findable).
    StampSlot& slot = g_stamps[idx0];
    uintptr_t displaced = slot.field.load(std::memory_order_relaxed);
    if (displaced != 0 && displaced != key) {
        NoteEvictedKey(displaced);
    }
    g_stampProbeFail.fetch_add(1, std::memory_order_relaxed);
    g_stampEpochProbeFail.fetch_add(1, std::memory_order_relaxed);
    g_stampWraps.fetch_add(1, std::memory_order_relaxed);
    g_stampEpochWraps.fetch_add(1, std::memory_order_relaxed);
    writeFields(slot);
    slot.field.store(key, std::memory_order_release);
}

struct StampLookup {
    bool found = false;
    uint8_t phase = 0;
    bool recorded = false;
    uint8_t holderGen = 0;
    uint8_t targetGen = 0;
    uint8_t skipReason = 0;
};

StampLookup LoadStamp(MAddress fieldAddress)
{
    StampLookup r;
    if (g_stamps == nullptr) {
        return r;
    }
    size_t idx0 = HashField(fieldAddress);
    for (size_t p = 0; p < kProbeMax; ++p) {
        size_t idx = (idx0 + p) & g_stampMask;
        StampSlot& slot = g_stamps[idx];
        uintptr_t f = slot.field.load(std::memory_order_acquire);
        if (f == 0) {
            return r;
        }
        if (f == static_cast<uintptr_t>(fieldAddress)) {
            r.found = true;
            r.phase = slot.phase.load(std::memory_order_relaxed);
            r.recorded = slot.recorded.load(std::memory_order_relaxed) != 0;
            r.holderGen = slot.holderGen.load(std::memory_order_relaxed);
            r.targetGen = slot.targetGen.load(std::memory_order_relaxed);
            r.skipReason = slot.skipReason.load(std::memory_order_relaxed);
            return r;
        }
    }
    return r;
}

void NoteRa(void* ra)
{
    if (ra == nullptr) {
        return;
    }
    uintptr_t pc = reinterpret_cast<uintptr_t>(ra);
    size_t idx = (pc >> 4) % kRaBuckets;
    for (size_t p = 0; p < 8; ++p) {
        size_t i = (idx + p) % kRaBuckets;
        uintptr_t cur = g_ra[i].pc.load(std::memory_order_relaxed);
        if (cur == 0) {
            uintptr_t exp = 0;
            if (g_ra[i].pc.compare_exchange_strong(exp, pc, std::memory_order_relaxed)) {
                g_ra[i].count.fetch_add(1, std::memory_order_relaxed);
                g_raNotes.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            cur = exp;
        }
        if (cur == pc) {
            g_ra[i].count.fetch_add(1, std::memory_order_relaxed);
            g_raNotes.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void NoteBareOffset(BaseObject* holder, MAddress fieldAddress)
{
    if (holder == nullptr || fieldAddress == 0) {
        g_offUnknown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(holder);
    uintptr_t slot = static_cast<uintptr_t>(fieldAddress);
    if (slot < base) {
        g_offNeg.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t off = static_cast<uint32_t>(slot - base);
    TypeInfo* ti = holder->GetTypeInfo();
    bool raw = holder->IsRawArray();
    const char* name = (ti != nullptr) ? ti->GetName() : nullptr;
    if (name == nullptr) {
        name = raw ? "<RawArray>" : "<unknown>";
    }
    size_t h = 1469598103934665603ull;
    for (const char* p = name; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 1099511628211ull;
    }
    h ^= static_cast<size_t>(off) + 0x9e3779b9u + (h << 6) + (h >> 2);
    size_t idx = h % kOffBuckets;
    for (size_t p = 0; p < 24; ++p) {
        size_t i = (idx + p) % kOffBuckets;
        if (g_off[i].used.load(std::memory_order_acquire) == 0) {
            uint8_t exp = 0;
            if (g_off[i].used.compare_exchange_strong(exp, 1, std::memory_order_acq_rel)) {
                std::strncpy(g_off[i].name, name, kAttrNameLen - 1);
                g_off[i].name[kAttrNameLen - 1] = '\0';
                g_off[i].offset.store(off, std::memory_order_relaxed);
                g_off[i].isRawArray.store(raw ? 1 : 0, std::memory_order_relaxed);
                g_off[i].count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        if (g_off[i].offset.load(std::memory_order_relaxed) == off &&
            std::strncmp(g_off[i].name, name, kAttrNameLen) == 0) {
            g_off[i].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void NoteBareHolder(BaseObject* holder)
{
    if (holder == nullptr) {
        g_attrUnknown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    TypeInfo* ti = holder->GetTypeInfo();
    if (ti == nullptr) {
        g_attrUnknown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    bool raw = holder->IsRawArray();
    if (raw) {
        g_attrRawArray.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_attrOther.fetch_add(1, std::memory_order_relaxed);
    }
    const char* name = ti->GetName();
    if (name == nullptr) {
        name = raw ? "<RawArray>" : "<unknown>";
    }
    size_t h = 1469598103934665603ull;
    for (const char* p = name; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 1099511628211ull;
    }
    size_t idx = h % kAttrBuckets;
    for (size_t p = 0; p < 16; ++p) {
        size_t i = (idx + p) % kAttrBuckets;
        if (g_attr[i].used.load(std::memory_order_acquire) == 0) {
            uint8_t exp = 0;
            if (g_attr[i].used.compare_exchange_strong(exp, 1, std::memory_order_acq_rel)) {
                std::strncpy(g_attr[i].name, name, kAttrNameLen - 1);
                g_attr[i].name[kAttrNameLen - 1] = '\0';
                g_attr[i].isRawArray.store(raw ? 1 : 0, std::memory_order_relaxed);
                g_attr[i].count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        if (std::strncmp(g_attr[i].name, name, kAttrNameLen) == 0) {
            g_attr[i].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

struct CensusStats {
    size_t holdersScanned = 0;
    size_t edgesTotal = 0;
    size_t remsetHit = 0;
    size_t remsetMiss = 0;
    size_t missBare = 0;
    size_t missBareNeverSeen = 0;
    size_t missBareDisplaced = 0;
    size_t missPhaseLe8 = 0;
    size_t missPhaseGt8 = 0;
    size_t missRecordedLost = 0;
    size_t missEarly = 0;
    size_t remsetSize = 0;
    uint64_t costNs = 0;
    std::array<size_t, kPhaseBuckets> missByPhase{};
    std::array<MAddress, kSampleLimit> missSamples{};
    size_t missSampleCount = 0;
};

void PushSample(CensusStats& stats, MAddress slot)
{
    if (stats.missSampleCount < kSampleLimit) {
        stats.missSamples[stats.missSampleCount++] = slot;
    }
}

void NoteGenOnMiss(const StampLookup& st, bool bare)
{
    uint8_t hg = st.found ? st.holderGen : kGenUnknown;
    uint8_t tg = st.found ? st.targetGen : kGenUnknown;
    if (hg > 3) {
        hg = kGenUnknown;
    }
    if (tg > 3) {
        tg = kGenUnknown;
    }
    g_missHolderGen[hg].fetch_add(1, std::memory_order_relaxed);
    g_missTargetGen[tg].fetch_add(1, std::memory_order_relaxed);
    if (bare) {
        g_bareHolderGen[hg].fetch_add(1, std::memory_order_relaxed);
        g_bareTargetGen[tg].fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteReasonOnMiss(const StampLookup& st, bool bare)
{
    uint8_t reason = bare ? kReasonNoStamp : (st.found ? st.skipReason : kReasonNoStamp);
    if (reason >= kReasonBuckets) {
        reason = kReasonUnknown;
    }
    g_missByReason[reason].fetch_add(1, std::memory_order_relaxed);
    if (bare) {
        g_bareByReason[kReasonNoStamp].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (reason == kReasonHolderYoung) {
        g_missReasonHolderYoung.fetch_add(1, std::memory_order_relaxed);
    } else if (reason == kReasonNoYoung) {
        g_missReasonNoYoung.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_missReasonOther.fetch_add(1, std::memory_order_relaxed);
    }
    // census-time holder is always non-young here; if write-time stamp said young ⇒ mismatch class
    if (st.found && st.holderGen == kGenYoung) {
        g_missGenMismatch.fetch_add(1, std::memory_order_relaxed);
    }
}

void ClassifyMiss(CensusStats& stats, MAddress fieldAddress, BaseObject* holder, BaseObject* target)
{
    ++stats.remsetMiss;
    StampLookup st = LoadStamp(fieldAddress);
    if (!st.found) {
        ++stats.missBare;
        // stampfix: split no_stamp into never entered vs squeezed out by table.
        if (WasEvicted(static_cast<uintptr_t>(fieldAddress))) {
            ++stats.missBareDisplaced;
        } else {
            ++stats.missBareNeverSeen;
        }
        ++stats.missByPhase[0];
        NoteBareHolder(holder);
        NoteBareOffset(holder, fieldAddress);
        NoteGenOnMiss(st, true);
        NoteReasonOnMiss(st, true);
        NoteFullClearMiss(fieldAddress, target, true);
        PushSample(stats, fieldAddress);
        return;
    }
    NoteGenOnMiss(st, false);
    NoteReasonOnMiss(st, false);
    NoteFullClearMiss(fieldAddress, target, false);
    if (st.phase < kPhaseBuckets) {
        ++stats.missByPhase[st.phase];
    } else {
        ++stats.missByPhase[0];
    }
    if (st.recorded) {
        ++stats.missRecordedLost;
    } else {
        ++stats.missEarly;
    }
    if (st.phase <= static_cast<uint8_t>(GC_PHASE_INIT)) {
        ++stats.missPhaseLe8;
    } else {
        ++stats.missPhaseGt8;
    }
    PushSample(stats, fieldAddress);
}

} // namespace

// Compile-time gate: getenv MRT_GCV2_* is pinned-off. Census arm of A-V3
// (ZGC_CONVERGENCE_PLAN.md §A.6 / Z-2) must actually run.
constexpr bool kIdleEdge = true;

bool Enabled()
{
    return kIdleEdge;
}

void NotePromoteTimeTarget(MAddress fieldAddress, uint8_t targetGen, bool recorded)
{
    if (!FullClearProbeOn() || fieldAddress == 0) {
        return;
    }
    if (targetGen > 3) {
        targetGen = kGenUnknown;
    }
    g_fcPromoteSeen.fetch_add(1, std::memory_order_relaxed);
    g_fcPromoteTargetGen[targetGen].fetch_add(1, std::memory_order_relaxed);
    if (recorded) {
        g_fcPromoteRecorded.fetch_add(1, std::memory_order_relaxed);
    }
    StorePromoteStamp(fieldAddress, targetGen, recorded);
}

void NoteBarrierDecision(MAddress fieldAddress, GCPhase phase, bool recorded, uint8_t holderGen,
                         uint8_t targetGen, uint8_t skipReason, uint8_t holderObjGen)
{
    if (!Enabled() || fieldAddress == 0) {
        return;
    }
    if (holderGen == kGenYoung) {
        g_decHolderYoung.fetch_add(1, std::memory_order_relaxed);
        if (!recorded) {
            g_decSkippedHolderYoung.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (holderGen == kGenOld) {
        g_decHolderOld.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_decHolderOther.fetch_add(1, std::memory_order_relaxed);
    }
    if (targetGen == kGenYoung) {
        g_decTargetYoung.fetch_add(1, std::memory_order_relaxed);
    } else if (targetGen == kGenOld) {
        g_decTargetOld.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_decTargetOther.fetch_add(1, std::memory_order_relaxed);
    }
    if (recorded) {
        g_decRecorded.fetch_add(1, std::memory_order_relaxed);
        skipReason = kReasonRecorded;
    }
    if (skipReason >= kReasonBuckets) {
        skipReason = kReasonUnknown;
    }
    g_decByReason[skipReason].fetch_add(1, std::memory_order_relaxed);
    // field-addr gen vs object-header gen (both known and differ)
    if (holderObjGen != 0 && holderGen != 0 && holderObjGen != holderGen) {
        g_decGenMismatch.fetch_add(1, std::memory_order_relaxed);
        if ((holderGen == kGenYoung && holderObjGen == kGenOld) ||
            (holderGen == kGenOld && holderObjGen == kGenYoung)) {
            g_decGenMismatchHolderYoung.fetch_add(1, std::memory_order_relaxed);
        }
    }
    StoreStamp(fieldAddress, static_cast<uint8_t>(phase), recorded, holderGen, targetGen, skipReason);
    NoteRa(__builtin_return_address(1));
}

void CensusPrePinnedStamp(size_t minorRunIndex)
{
    if (!Enabled()) {
        return;
    }
    EnsureStampTable();
    static std::atomic<size_t> invokeCount{ 0 };
    size_t invoke = invokeCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (invoke == 1) {
        LOG(RTLOG_ERROR, "[GCV2][idleedge] armed first invoke=1 (kIdleEdge=1)");
    }
    size_t every = EnvSizeT("MRT_GCV2_IDLEEDGE_EVERY", 1);
    if (every == 0) {
        every = 1;
    }
    if ((invoke - 1) % every != 0) {
        return;
    }

    uint64_t startNs = TimeUtil::NanoSeconds();
    CensusStats stats;
    std::unordered_set<MAddress> remsetSnap = Heap::GetHeap().GetRememberedSet().Snapshot();
    stats.remsetSize = remsetSnap.size();

    Heap::GetHeap().ForEachObj(
        [&stats, &remsetSnap](BaseObject* holder) {
            if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                return;
            }
            RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (holderRegion == nullptr || holderRegion->IsYoungRegion() || holderRegion->IsGarbageRegion() ||
                holderRegion->IsFreeRegion()) {
                return;
            }
            ++stats.holdersScanned;
            holder->ForEachRefField([&stats, &remsetSnap, holder](RefField<>& field) {
                BaseObject* target = to_object(field.GetTargetObject());
                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                    return;
                }
                RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                if (targetRegion == nullptr || !targetRegion->IsYoungRegion()) {
                    return;
                }
                MAddress slot = reinterpret_cast<MAddress>(&field);
                ++stats.edgesTotal;
                if (remsetSnap.count(slot) != 0) {
                    ++stats.remsetHit;
                } else {
                    ClassifyMiss(stats, slot, holder, target);
                }
            });
        },
        false);

    stats.costNs = TimeUtil::NanoSeconds() - startNs;

    g_minorsCensused.fetch_add(1, std::memory_order_relaxed);
    g_edgesTotal.fetch_add(stats.edgesTotal, std::memory_order_relaxed);
    g_remsetHit.fetch_add(stats.remsetHit, std::memory_order_relaxed);
    g_remsetMiss.fetch_add(stats.remsetMiss, std::memory_order_relaxed);
    g_missBare.fetch_add(stats.missBare, std::memory_order_relaxed);
    g_missBareNeverSeen.fetch_add(stats.missBareNeverSeen, std::memory_order_relaxed);
    g_missBareDisplaced.fetch_add(stats.missBareDisplaced, std::memory_order_relaxed);
    g_missPhaseLe8.fetch_add(stats.missPhaseLe8, std::memory_order_relaxed);
    g_missPhaseGt8.fetch_add(stats.missPhaseGt8, std::memory_order_relaxed);
    g_missRecordedLost.fetch_add(stats.missRecordedLost, std::memory_order_relaxed);
    g_missEarly.fetch_add(stats.missEarly, std::memory_order_relaxed);
    g_costNsTotal.fetch_add(stats.costNs, std::memory_order_relaxed);
    for (size_t i = 0; i < kPhaseBuckets; ++i) {
        if (stats.missByPhase[i] != 0) {
            g_missByPhase[i].fetch_add(stats.missByPhase[i], std::memory_order_relaxed);
        }
    }

    double missPct =
        stats.edgesTotal == 0 ? 0.0 : (100.0 * static_cast<double>(stats.remsetMiss) / static_cast<double>(stats.edgesTotal));
    size_t idleClass = stats.missBare + stats.missPhaseLe8;
    double idlePctOfMiss =
        stats.remsetMiss == 0 ? 0.0 : (100.0 * static_cast<double>(idleClass) / static_cast<double>(stats.remsetMiss));
    double gt8PctOfMiss =
        stats.remsetMiss == 0 ? 0.0 :
                                (100.0 * static_cast<double>(stats.missPhaseGt8) / static_cast<double>(stats.remsetMiss));

    LOG(RTLOG_ERROR,
         "[GCV2][idleedge] point=pre-pinned-stamp invoke=%zu minorRun=%zu env=kIdleEdge=1 "
         "remsetSize=%zu holdersScanned=%zu oldToYoungEdges=%zu remsetHit=%zu remsetMiss=%zu "
         "missPct=%.2f missBare=%zu missBareNeverSeen=%zu missBareDisplaced=%zu "
         "missPhaseLe8=%zu missPhaseGt8=%zu missRecordedLost=%zu missEarly=%zu "
         "idleClassOfMiss=%zu (%.1f%%) gt8OfMiss=%zu (%.1f%%) costNs=%llu stampNotes=%llu stampWraps=%llu "
         "stampCap=%zu stampProbeFail=%llu stampEpochEvicted=%llu attrRaw=%llu attrOther=%llu "
         "missSamples=[%p,%p,%p,%p]",
         invoke, minorRunIndex, stats.remsetSize, stats.holdersScanned, stats.edgesTotal, stats.remsetHit,
         stats.remsetMiss, missPct, stats.missBare, stats.missBareNeverSeen, stats.missBareDisplaced,
         stats.missPhaseLe8, stats.missPhaseGt8, stats.missRecordedLost, stats.missEarly, idleClass,
         idlePctOfMiss, stats.missPhaseGt8, gt8PctOfMiss, static_cast<unsigned long long>(stats.costNs),
         static_cast<unsigned long long>(g_stampNotes.load()),
         static_cast<unsigned long long>(g_stampWraps.load()), g_stampCap,
         static_cast<unsigned long long>(g_stampProbeFail.load()),
         static_cast<unsigned long long>(g_stampEpochEvicted.load()),
         static_cast<unsigned long long>(g_attrRawArray.load()),
         static_cast<unsigned long long>(g_attrOther.load()),
         reinterpret_cast<void*>(stats.missSamples[0]),
         reinterpret_cast<void*>(stats.missSamples[1]), reinterpret_cast<void*>(stats.missSamples[2]),
         reinterpret_cast<void*>(stats.missSamples[3]));

    EmitInstrumentHealth("census", stats.remsetSize, stats.edgesTotal);

    for (size_t i = 0; i < kPhaseBuckets; ++i) {
        if (stats.missByPhase[i] == 0) {
            continue;
        }
        double pct = stats.remsetMiss == 0 ?
            0.0 :
            (100.0 * static_cast<double>(stats.missByPhase[i]) / static_cast<double>(stats.remsetMiss));
        VLOG(REPORT, "[GCV2][idleedge][MISS_BY_PHASE] invoke=%zu phase=%s(%zu) miss=%zu (%.1f%%)", invoke,
             PhaseName(static_cast<uint8_t>(i)), i, stats.missByPhase[i], pct);
    }

    // stampfix path 甲: clear write stamps after census so next minor starts empty.
    // Promote stamps (fullclear) are intentionally NOT cleared — they span full GC.
    ClearWriteStampsAfterCensus();
}

void DumpProcessTotals(const char* tag)
{
    if (!Enabled()) {
        return;
    }
    uint64_t minors = g_minorsCensused.load(std::memory_order_relaxed);
    uint64_t edges = g_edgesTotal.load(std::memory_order_relaxed);
    uint64_t hit = g_remsetHit.load(std::memory_order_relaxed);
    uint64_t miss = g_remsetMiss.load(std::memory_order_relaxed);
    uint64_t bare = g_missBare.load(std::memory_order_relaxed);
    uint64_t bareNever = g_missBareNeverSeen.load(std::memory_order_relaxed);
    uint64_t bareDisp = g_missBareDisplaced.load(std::memory_order_relaxed);
    uint64_t le8 = g_missPhaseLe8.load(std::memory_order_relaxed);
    uint64_t gt8 = g_missPhaseGt8.load(std::memory_order_relaxed);
    uint64_t lost = g_missRecordedLost.load(std::memory_order_relaxed);
    uint64_t early = g_missEarly.load(std::memory_order_relaxed);
    uint64_t cost = g_costNsTotal.load(std::memory_order_relaxed);
    double missPct = edges == 0 ? 0.0 : (100.0 * static_cast<double>(miss) / static_cast<double>(edges));
    double perMinorMiss = minors == 0 ? 0.0 : static_cast<double>(miss) / static_cast<double>(minors);
    double perMinorEdges = minors == 0 ? 0.0 : static_cast<double>(edges) / static_cast<double>(minors);
    uint64_t idleClass = bare + le8;
    double idlePct = miss == 0 ? 0.0 : (100.0 * static_cast<double>(idleClass) / static_cast<double>(miss));
    double gt8Pct = miss == 0 ? 0.0 : (100.0 * static_cast<double>(gt8) / static_cast<double>(miss));

    LOG(RTLOG_ERROR,
         "[GCV2][idleedge][TOTAL] tag=%s minors=%llu edges=%llu hit=%llu miss=%llu missPct=%.2f "
         "perMinorEdges=%.1f perMinorMiss=%.1f missBare=%llu missBareNeverSeen=%llu missBareDisplaced=%llu "
         "missPhaseLe8=%llu missPhaseGt8=%llu missRecordedLost=%llu missEarly=%llu "
         "idleClassOfMiss=%llu (%.1f%%) gt8OfMiss=%llu (%.1f%%) costNsTotal=%llu "
         "stampNotes=%llu stampWraps=%llu stampCap=%zu stampProbeFail=%llu stampEvicted=%llu "
         "stampClears=%llu stampEpoch=%u attrRawArray=%llu attrOther=%llu attrUnknown=%llu "
         "env=kIdleEdge=1",
         tag == nullptr ? "?" : tag, static_cast<unsigned long long>(minors), static_cast<unsigned long long>(edges),
         static_cast<unsigned long long>(hit), static_cast<unsigned long long>(miss), missPct, perMinorEdges,
         perMinorMiss, static_cast<unsigned long long>(bare), static_cast<unsigned long long>(bareNever),
         static_cast<unsigned long long>(bareDisp), static_cast<unsigned long long>(le8),
         static_cast<unsigned long long>(gt8), static_cast<unsigned long long>(lost),
         static_cast<unsigned long long>(early), static_cast<unsigned long long>(idleClass), idlePct,
         static_cast<unsigned long long>(gt8), gt8Pct, static_cast<unsigned long long>(cost),
         static_cast<unsigned long long>(g_stampNotes.load()), static_cast<unsigned long long>(g_stampWraps.load()),
         g_stampCap, static_cast<unsigned long long>(g_stampProbeFail.load()),
         static_cast<unsigned long long>(g_stampEvicted.load()),
         static_cast<unsigned long long>(g_stampClears.load()),
         g_stampEpoch.load(std::memory_order_relaxed),
         static_cast<unsigned long long>(g_attrRawArray.load()),
         static_cast<unsigned long long>(g_attrOther.load()),
         static_cast<unsigned long long>(g_attrUnknown.load()));

    VLOG(REPORT,
         "[GCV2][idleedge][NO_STAMP_SPLIT] tag=%s missBare=%llu neverSeen=%llu(%.1f%%) "
         "displaced=%llu(%.1f%%) stampEvicted=%llu evictOverflow=%llu "
         "compat=missBare_equals_neverSeen_plus_displaced",
         tag == nullptr ? "?" : tag, static_cast<unsigned long long>(bare),
         static_cast<unsigned long long>(bareNever),
         bare == 0 ? 0.0 : 100.0 * static_cast<double>(bareNever) / static_cast<double>(bare),
         static_cast<unsigned long long>(bareDisp),
         bare == 0 ? 0.0 : 100.0 * static_cast<double>(bareDisp) / static_cast<double>(bare),
         static_cast<unsigned long long>(g_stampEvicted.load()),
         static_cast<unsigned long long>(g_evictOverflow.load()));

    for (size_t i = 0; i < kPhaseBuckets; ++i) {
        uint64_t m = g_missByPhase[i].load(std::memory_order_relaxed);
        if (m == 0) {
            continue;
        }
        double pct = miss == 0 ? 0.0 : (100.0 * static_cast<double>(m) / static_cast<double>(miss));
        VLOG(REPORT, "[GCV2][idleedge][TOTAL_BY_PHASE] tag=%s phase=%s(%zu) miss=%llu (%.1f%%)",
             tag == nullptr ? "?" : tag, PhaseName(static_cast<uint8_t>(i)), i, static_cast<unsigned long long>(m),
             pct);
    }

    struct TypeP {
        uint64_t c;
        const char* n;
        int raw;
    };
    TypeP top[kAttrBuckets];
    size_t ntop = 0;
    for (size_t i = 0; i < kAttrBuckets; ++i) {
        uint64_t c = g_attr[i].count.load(std::memory_order_relaxed);
        if (c == 0 || g_attr[i].used.load(std::memory_order_relaxed) == 0) {
            continue;
        }
        top[ntop++] = TypeP{ c, g_attr[i].name, static_cast<int>(g_attr[i].isRawArray.load()) };
    }
    for (size_t i = 0; i < ntop; ++i) {
        for (size_t j = i + 1; j < ntop; ++j) {
            if (top[j].c > top[i].c) {
                TypeP t = top[i];
                top[i] = top[j];
                top[j] = t;
            }
        }
    }
    size_t show = ntop < 16 ? ntop : 16;
    for (size_t i = 0; i < show; ++i) {
        double pct = bare == 0 ? 0.0 : (100.0 * static_cast<double>(top[i].c) / static_cast<double>(bare));
        VLOG(REPORT, "[GCV2][idleedge][BARE_BY_TYPE] tag=%s rank=%zu count=%llu (%.1f%% of bare) rawArray=%d type=%s",
             tag == nullptr ? "?" : tag, i + 1, static_cast<unsigned long long>(top[i].c), pct, top[i].raw,
             top[i].n);
    }

    struct OffP {
        uint64_t c;
        const char* n;
        uint32_t off;
        int raw;
    };
    OffP otop[kOffBuckets];
    size_t notop = 0;
    for (size_t i = 0; i < kOffBuckets; ++i) {
        uint64_t c = g_off[i].count.load(std::memory_order_relaxed);
        if (c == 0 || g_off[i].used.load(std::memory_order_relaxed) == 0) {
            continue;
        }
        otop[notop++] = OffP{ c, g_off[i].name, g_off[i].offset.load(std::memory_order_relaxed),
                              static_cast<int>(g_off[i].isRawArray.load()) };
    }
    for (size_t i = 0; i < notop; ++i) {
        for (size_t j = i + 1; j < notop; ++j) {
            if (otop[j].c > otop[i].c) {
                OffP t = otop[i];
                otop[i] = otop[j];
                otop[j] = t;
            }
        }
    }
    size_t showOff = notop < 24 ? notop : 24;
    VLOG(REPORT,
         "[GCV2][idleedge][BARE_BY_OFF] tag=%s nKeys=%zu offUnknown=%llu offNeg=%llu bare=%llu",
         tag == nullptr ? "?" : tag, notop, static_cast<unsigned long long>(g_offUnknown.load()),
         static_cast<unsigned long long>(g_offNeg.load()), static_cast<unsigned long long>(bare));
    for (size_t i = 0; i < showOff; ++i) {
        double pct = bare == 0 ? 0.0 : (100.0 * static_cast<double>(otop[i].c) / static_cast<double>(bare));
        VLOG(REPORT,
             "[GCV2][idleedge][BARE_BY_OFF] tag=%s rank=%zu count=%llu (%.1f%% of bare) rawArray=%d "
             "offset=0x%x (%u) type=%s",
             tag == nullptr ? "?" : tag, i + 1, static_cast<unsigned long long>(otop[i].c), pct, otop[i].raw,
             otop[i].off, otop[i].off, otop[i].n);
    }

    // promoteedge: write-time generation of holder/target on bare/miss edges.
    auto genName = [](size_t g) -> const char* {
        switch (g) {
            case 1:
                return "young";
            case 2:
                return "old";
            case 3:
                return "nonheap";
            default:
                return "unknown";
        }
    };
    uint64_t bareHg[4];
    uint64_t bareTg[4];
    uint64_t missHg[4];
    uint64_t missTg[4];
    uint64_t bareHgSum = 0;
    uint64_t bareTgSum = 0;
    uint64_t missHgSum = 0;
    uint64_t missTgSum = 0;
    for (size_t g = 0; g < 4; ++g) {
        bareHg[g] = g_bareHolderGen[g].load(std::memory_order_relaxed);
        bareTg[g] = g_bareTargetGen[g].load(std::memory_order_relaxed);
        missHg[g] = g_missHolderGen[g].load(std::memory_order_relaxed);
        missTg[g] = g_missTargetGen[g].load(std::memory_order_relaxed);
        bareHgSum += bareHg[g];
        bareTgSum += bareTg[g];
        missHgSum += missHg[g];
        missTgSum += missTg[g];
    }
    VLOG(REPORT,
         "[GCV2][idleedge][WRITE_GEN] tag=%s bareHolder young=%llu(%.1f%%) old=%llu(%.1f%%) "
         "nonheap=%llu unknown=%llu total=%llu | bareTarget young=%llu old=%llu nonheap=%llu unknown=%llu | "
         "missHolder young=%llu(%.1f%%) old=%llu(%.1f%%) nonheap=%llu unknown=%llu total=%llu | "
         "missTarget young=%llu old=%llu nonheap=%llu unknown=%llu",
         tag == nullptr ? "?" : tag,
         static_cast<unsigned long long>(bareHg[kGenYoung]),
         bareHgSum == 0 ? 0.0 : 100.0 * static_cast<double>(bareHg[kGenYoung]) / static_cast<double>(bareHgSum),
         static_cast<unsigned long long>(bareHg[kGenOld]),
         bareHgSum == 0 ? 0.0 : 100.0 * static_cast<double>(bareHg[kGenOld]) / static_cast<double>(bareHgSum),
         static_cast<unsigned long long>(bareHg[kGenNonHeap]),
         static_cast<unsigned long long>(bareHg[kGenUnknown]),
         static_cast<unsigned long long>(bareHgSum),
         static_cast<unsigned long long>(bareTg[kGenYoung]), static_cast<unsigned long long>(bareTg[kGenOld]),
         static_cast<unsigned long long>(bareTg[kGenNonHeap]),
         static_cast<unsigned long long>(bareTg[kGenUnknown]),
         static_cast<unsigned long long>(missHg[kGenYoung]),
         missHgSum == 0 ? 0.0 : 100.0 * static_cast<double>(missHg[kGenYoung]) / static_cast<double>(missHgSum),
         static_cast<unsigned long long>(missHg[kGenOld]),
         missHgSum == 0 ? 0.0 : 100.0 * static_cast<double>(missHg[kGenOld]) / static_cast<double>(missHgSum),
         static_cast<unsigned long long>(missHg[kGenNonHeap]),
         static_cast<unsigned long long>(missHg[kGenUnknown]),
         static_cast<unsigned long long>(missHgSum),
         static_cast<unsigned long long>(missTg[kGenYoung]), static_cast<unsigned long long>(missTg[kGenOld]),
         static_cast<unsigned long long>(missTg[kGenNonHeap]),
         static_cast<unsigned long long>(missTg[kGenUnknown]));
    for (size_t g = 0; g < 4; ++g) {
        if (bareHg[g] == 0 && bareTg[g] == 0 && missHg[g] == 0 && missTg[g] == 0) {
            continue;
        }
        VLOG(REPORT,
             "[GCV2][idleedge][WRITE_GEN_DETAIL] tag=%s gen=%s bareHolder=%llu bareTarget=%llu "
             "missHolder=%llu missTarget=%llu",
             tag == nullptr ? "?" : tag, genName(g), static_cast<unsigned long long>(bareHg[g]),
             static_cast<unsigned long long>(bareTg[g]), static_cast<unsigned long long>(missHg[g]),
             static_cast<unsigned long long>(missTg[g]));
    }
    uint64_t decHy = g_decHolderYoung.load(std::memory_order_relaxed);
    uint64_t decHo = g_decHolderOld.load(std::memory_order_relaxed);
    uint64_t decHx = g_decHolderOther.load(std::memory_order_relaxed);
    uint64_t decTy = g_decTargetYoung.load(std::memory_order_relaxed);
    uint64_t decTo = g_decTargetOld.load(std::memory_order_relaxed);
    uint64_t decTx = g_decTargetOther.load(std::memory_order_relaxed);
    uint64_t decRec = g_decRecorded.load(std::memory_order_relaxed);
    uint64_t decSkipY = g_decSkippedHolderYoung.load(std::memory_order_relaxed);
    uint64_t decHsum = decHy + decHo + decHx;
    VLOG(REPORT,
         "[GCV2][idleedge][WRITE_GEN_DEC] tag=%s holderYoung=%llu(%.1f%%) holderOld=%llu(%.1f%%) "
         "holderOther=%llu targetYoung=%llu targetOld=%llu targetOther=%llu recorded=%llu "
         "skippedHolderYoung=%llu totalDec=%llu",
         tag == nullptr ? "?" : tag, static_cast<unsigned long long>(decHy),
         decHsum == 0 ? 0.0 : 100.0 * static_cast<double>(decHy) / static_cast<double>(decHsum),
         static_cast<unsigned long long>(decHo),
         decHsum == 0 ? 0.0 : 100.0 * static_cast<double>(decHo) / static_cast<double>(decHsum),
         static_cast<unsigned long long>(decHx), static_cast<unsigned long long>(decTy),
         static_cast<unsigned long long>(decTo), static_cast<unsigned long long>(decTx),
         static_cast<unsigned long long>(decRec), static_cast<unsigned long long>(decSkipY),
         static_cast<unsigned long long>(decHsum));

    // idlewrite: skip-arm attribution (decision totals + miss-side)
    auto reasonName = [](size_t r) -> const char* {
        switch (r) {
            case kReasonRecorded:
                return "recorded";
            case kReasonNoYoung:
                return "no_young";
            case kReasonRefNullOrNonheap:
                return "ref_null_or_nonheap";
            case kReasonRefNotYoung:
                return "ref_not_young";
            case kReasonHolderNullOrNonheap:
                return "holder_null_or_nonheap";
            case kReasonHolderYoung:
                return "holder_young";
            case kReasonNoStamp:
                return "no_stamp";
            default:
                return "unknown";
        }
    };
    uint64_t decReasonSum = 0;
    uint64_t missReasonSum = 0;
    uint64_t decReasons[kReasonBuckets];
    uint64_t missReasons[kReasonBuckets];
    for (size_t r = 0; r < kReasonBuckets; ++r) {
        decReasons[r] = g_decByReason[r].load(std::memory_order_relaxed);
        missReasons[r] = g_missByReason[r].load(std::memory_order_relaxed);
        decReasonSum += decReasons[r];
        missReasonSum += missReasons[r];
    }
    uint64_t genMismatch = g_decGenMismatch.load(std::memory_order_relaxed);
    uint64_t genMismatchHY = g_decGenMismatchHolderYoung.load(std::memory_order_relaxed);
    uint64_t missHY = g_missReasonHolderYoung.load(std::memory_order_relaxed);
    uint64_t missNY = g_missReasonNoYoung.load(std::memory_order_relaxed);
    uint64_t missOth = g_missReasonOther.load(std::memory_order_relaxed);
    uint64_t missGenMM = g_missGenMismatch.load(std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][idleedge][SKIP_ARM] tag=%s decTotal=%llu missTotal=%llu "
         "missHolderYoung=%llu missNoYoung=%llu missOther=%llu missGenMismatch=%llu "
         "decGenMismatch=%llu decGenMismatchFieldVsObj=%llu",
         tag == nullptr ? "?" : tag, static_cast<unsigned long long>(decReasonSum),
         static_cast<unsigned long long>(missReasonSum), static_cast<unsigned long long>(missHY),
         static_cast<unsigned long long>(missNY), static_cast<unsigned long long>(missOth),
         static_cast<unsigned long long>(missGenMM), static_cast<unsigned long long>(genMismatch),
         static_cast<unsigned long long>(genMismatchHY));
    for (size_t r = 0; r < kReasonBuckets; ++r) {
        if (decReasons[r] == 0 && missReasons[r] == 0) {
            continue;
        }
        double dPct =
            decReasonSum == 0 ? 0.0 : 100.0 * static_cast<double>(decReasons[r]) / static_cast<double>(decReasonSum);
        double mPct =
            missReasonSum == 0 ? 0.0 : 100.0 * static_cast<double>(missReasons[r]) / static_cast<double>(missReasonSum);
        VLOG(REPORT,
             "[GCV2][idleedge][SKIP_ARM_DETAIL] tag=%s reason=%s(%zu) dec=%llu(%.1f%%) miss=%llu(%.1f%%)",
             tag == nullptr ? "?" : tag, reasonName(r), r, static_cast<unsigned long long>(decReasons[r]), dPct,
             static_cast<unsigned long long>(missReasons[r]), mPct);
    }
    // stampfix: no_stamp split (legacy no_stamp miss count = neverSeen + displaced)
    VLOG(REPORT,
         "[GCV2][idleedge][SKIP_ARM_DETAIL] tag=%s reason=no_stamp_never_seen dec=0 miss=%llu "
         "of_no_stamp=%.1f%% positive=ClassifyMiss without prior StoreStamp",
         tag == nullptr ? "?" : tag, static_cast<unsigned long long>(bareNever),
         bare == 0 ? 0.0 : 100.0 * static_cast<double>(bareNever) / static_cast<double>(bare));
    VLOG(REPORT,
         "[GCV2][idleedge][SKIP_ARM_DETAIL] tag=%s reason=no_stamp_displaced dec=0 miss=%llu "
         "of_no_stamp=%.1f%% positive=force-overwrite StoreStamp then ClassifyMiss same key",
         tag == nullptr ? "?" : tag, static_cast<unsigned long long>(bareDisp),
         bare == 0 ? 0.0 : 100.0 * static_cast<double>(bareDisp) / static_cast<double>(bare));

    if (FullClearProbeOn()) {
        uint64_t pSeen = g_fcPromoteSeen.load(std::memory_order_relaxed);
        uint64_t pRec = g_fcPromoteRecorded.load(std::memory_order_relaxed);
        uint64_t pY = g_fcPromoteTargetGen[kGenYoung].load(std::memory_order_relaxed);
        uint64_t pO = g_fcPromoteTargetGen[kGenOld].load(std::memory_order_relaxed);
        uint64_t pN = g_fcPromoteTargetGen[kGenNonHeap].load(std::memory_order_relaxed);
        uint64_t pU = g_fcPromoteTargetGen[kGenUnknown].load(std::memory_order_relaxed);
        uint64_t missTot = g_fcCensusMissTotal.load(std::memory_order_relaxed);
        uint64_t bareTot = g_fcCensusBareTotal.load(std::memory_order_relaxed);
        uint64_t noStamp = g_fcMissNoPromoteStamp.load(std::memory_order_relaxed);
        uint64_t bareNoStamp = g_fcBareNoPromoteStamp.load(std::memory_order_relaxed);
        // true miss = census young while promote was already non-young (old/null)
        uint64_t barePromOld = g_fcBarePromoteXCensus[kGenOld][kGenYoung].load(std::memory_order_relaxed);
        uint64_t barePromNull =
            g_fcBarePromoteXCensus[kGenNonHeap][kGenYoung].load(std::memory_order_relaxed);
        uint64_t barePromYoung =
            g_fcBarePromoteXCensus[kGenYoung][kGenYoung].load(std::memory_order_relaxed);
        uint64_t barePromUnk =
            g_fcBarePromoteXCensus[kGenUnknown][kGenYoung].load(std::memory_order_relaxed);
        uint64_t missPromOld = g_fcMissPromoteXCensus[kGenOld][kGenYoung].load(std::memory_order_relaxed);
        uint64_t missPromNull =
            g_fcMissPromoteXCensus[kGenNonHeap][kGenYoung].load(std::memory_order_relaxed);
        uint64_t missPromYoung =
            g_fcMissPromoteXCensus[kGenYoung][kGenYoung].load(std::memory_order_relaxed);
        uint64_t missPromUnk =
            g_fcMissPromoteXCensus[kGenUnknown][kGenYoung].load(std::memory_order_relaxed);
        // spurious: promote-time already not young ⇒ remset should not have it then
        uint64_t bareSpurious = barePromOld + barePromNull;
        uint64_t missSpurious = missPromOld + missPromNull;
        // real gap: promote-time young (should have been recorded) but census miss
        uint64_t bareReal = barePromYoung;
        uint64_t missReal = missPromYoung;
        VLOG(REPORT,
             "[GCV2][fullclear][PROMOTE_GEN] tag=%s seen=%llu rec=%llu targetYoung=%llu "
             "targetOld=%llu targetNull=%llu targetUnk=%llu stampNotes=%llu stampWraps=%llu "
             "stampFail=%llu",
             tag == nullptr ? "?" : tag, static_cast<unsigned long long>(pSeen),
             static_cast<unsigned long long>(pRec), static_cast<unsigned long long>(pY),
             static_cast<unsigned long long>(pO), static_cast<unsigned long long>(pN),
             static_cast<unsigned long long>(pU),
             static_cast<unsigned long long>(g_promoteStampNotes.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_promoteStampWraps.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_promoteStampProbeFail.load(std::memory_order_relaxed)));
        VLOG(REPORT,
             "[GCV2][fullclear][MISS_MATRIX] tag=%s missTot=%llu bareTot=%llu noPromoteStamp=%llu "
             "bareNoStamp=%llu | bare: promYoung=%llu promOld=%llu promNull=%llu promUnk=%llu "
             "spurious=%llu real=%llu | miss: promYoung=%llu promOld=%llu promNull=%llu "
             "promUnk=%llu spurious=%llu real=%llu",
             tag == nullptr ? "?" : tag, static_cast<unsigned long long>(missTot),
             static_cast<unsigned long long>(bareTot), static_cast<unsigned long long>(noStamp),
             static_cast<unsigned long long>(bareNoStamp),
             static_cast<unsigned long long>(barePromYoung),
             static_cast<unsigned long long>(barePromOld),
             static_cast<unsigned long long>(barePromNull),
             static_cast<unsigned long long>(barePromUnk),
             static_cast<unsigned long long>(bareSpurious), static_cast<unsigned long long>(bareReal),
             static_cast<unsigned long long>(missPromYoung),
             static_cast<unsigned long long>(missPromOld),
             static_cast<unsigned long long>(missPromNull),
             static_cast<unsigned long long>(missPromUnk),
             static_cast<unsigned long long>(missSpurious), static_cast<unsigned long long>(missReal));
        for (size_t pg = 0; pg < 4; ++pg) {
            for (size_t cg = 0; cg < 4; ++cg) {
                uint64_t c = g_fcMissPromoteXCensus[pg][cg].load(std::memory_order_relaxed);
                uint64_t b = g_fcBarePromoteXCensus[pg][cg].load(std::memory_order_relaxed);
                if (c == 0 && b == 0) {
                    continue;
                }
                VLOG(REPORT,
                     "[GCV2][fullclear][MISS_CELL] tag=%s promote=%s census=%s miss=%llu bare=%llu "
                     "recAtPromote=%llu skipAtPromote=%llu",
                     tag == nullptr ? "?" : tag, genName(pg), genName(cg),
                     static_cast<unsigned long long>(c), static_cast<unsigned long long>(b),
                     static_cast<unsigned long long>(
                         pg < 4 ? g_fcMissPromoteRec[pg].load(std::memory_order_relaxed) : 0),
                     static_cast<unsigned long long>(
                         pg < 4 ? g_fcMissPromoteSkip[pg].load(std::memory_order_relaxed) : 0));
            }
        }
    }

    struct RaP {
        uint64_t c;
        uintptr_t pc;
    };
    RaP rt[kRaBuckets];
    size_t nra = 0;
    for (size_t i = 0; i < kRaBuckets; ++i) {
        uint64_t c = g_ra[i].count.load(std::memory_order_relaxed);
        uintptr_t pc = g_ra[i].pc.load(std::memory_order_relaxed);
        if (c == 0 || pc == 0) {
            continue;
        }
        rt[nra++] = RaP{ c, pc };
    }
    for (size_t i = 0; i < nra; ++i) {
        for (size_t j = i + 1; j < nra; ++j) {
            if (rt[j].c > rt[i].c) {
                RaP t = rt[i];
                rt[i] = rt[j];
                rt[j] = t;
            }
        }
    }
    size_t showRa = nra < 12 ? nra : 12;
    for (size_t i = 0; i < showRa; ++i) {
        VLOG(REPORT, "[GCV2][idleedge][BARRIER_RA] tag=%s rank=%zu count=%llu pc=%p",
             tag == nullptr ? "?" : tag, i + 1, static_cast<unsigned long long>(rt[i].c),
             reinterpret_cast<void*>(rt[i].pc));
    }

    EmitInstrumentHealth(tag == nullptr ? "totals" : tag, 0, static_cast<size_t>(edges));
}

void RunSelfTest()
{
    // Positive controls: prove counters move when conditions are forced.
    // Does NOT mutate product remset/heap — only instrument tables + local stats.
    EnsureStampTable();

    const size_t nForce = (g_stampCap > 0 ? g_stampCap : kStampCapDefault) + 64;
    uint64_t notes0 = g_stampNotes.load(std::memory_order_relaxed);
    uint64_t wraps0 = g_stampWraps.load(std::memory_order_relaxed);
    uint64_t fail0 = g_stampProbeFail.load(std::memory_order_relaxed);

    // 1) stampWraps / stampProbeFail / occupancy: flood open-address table.
    for (size_t i = 0; i < nForce; ++i) {
        MAddress fake = static_cast<MAddress>(0x1000ull + i * 0x18ull);
        StoreStamp(fake, static_cast<uint8_t>(GC_PHASE_IDLE), /*recorded=*/false, kGenOld, kGenYoung);
    }
    uint64_t notes1 = g_stampNotes.load(std::memory_order_relaxed);
    uint64_t wraps1 = g_stampWraps.load(std::memory_order_relaxed);
    uint64_t fail1 = g_stampProbeFail.load(std::memory_order_relaxed);
    size_t occ = CountStampOccupied();
    double occPct = g_stampCap == 0 ? 0.0 : 100.0 * static_cast<double>(occ) / static_cast<double>(g_stampCap);

    bool okNotes = notes1 > notes0;
    bool okWrapOrFail = (wraps1 > wraps0) || (fail1 > fail0) || (occPct > 50.0);
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=stampNotes forcedDelta=%llu ok=%d "
        "healthyExpect=>0 when barrier notes",
        static_cast<unsigned long long>(notes1 - notes0), okNotes ? 1 : 0);
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=stampWraps|stampProbeFail|occ "
        "wrapsDelta=%llu failDelta=%llu occPct=%.1f ok=%d "
        "healthyExpect=wraps/fail rise or occ>50 under flood",
        static_cast<unsigned long long>(wraps1 - wraps0),
        static_cast<unsigned long long>(fail1 - fail0), occPct, okWrapOrFail ? 1 : 0);

    // 2) missBare / missRecordedLost / phase buckets via ClassifyMiss on synthetic stamps.
    CensusStats local;
    MAddress bareSlot = static_cast<MAddress>(0xBEEF0000ull);
    // Ensure bareSlot has no stamp (use address outside flood if possible).
    ClassifyMiss(local, bareSlot, nullptr, nullptr);
    bool okBare = local.missBare > 0;
    bool okNever = local.missBareNeverSeen > 0 && local.missBareDisplaced == 0;
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=missBare forced=%zu ok=%d "
        "healthyExpect=>0 when heap edge lacks stamp",
        local.missBare, okBare ? 1 : 0);
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=missBareNeverSeen forced=%zu ok=%d "
        "healthyExpect=>0 when edge never StoreStamp'd this epoch",
        local.missBareNeverSeen, okNever ? 1 : 0);

    // 2b) missBareDisplaced: force-overwrite a known key then classify as bare.
    MAddress victim = static_cast<MAddress>(0x1000ull);
    StoreStamp(victim, static_cast<uint8_t>(GC_PHASE_IDLE), /*recorded=*/false, kGenOld, kGenYoung);
    // Fill home probe chain with distinct keys that hash near victim so StoreStamp force-evicts.
    // StoreStamp force-overwrites home slot when probe fails; NoteEvictedKey records prior key.
    uint64_t ev0 = g_stampEvicted.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kProbeMax + 8; ++i) {
        // Distinct keys that share victim's hash home after wrap — flood already filled table;
        // another store that collides will displace.
        MAddress coll = static_cast<MAddress>(0x2000ull + i * 0x18ull);
        StoreStamp(coll, static_cast<uint8_t>(GC_PHASE_IDLE), /*recorded=*/false, kGenOld, kGenYoung);
    }
    // Directly mark victim as evicted if flood didn't (table may have re-homed it).
    if (!WasEvicted(static_cast<uintptr_t>(victim))) {
        NoteEvictedKey(static_cast<uintptr_t>(victim));
        // Wipe stamp so ClassifyMiss sees bare.
        if (g_stamps != nullptr) {
            size_t idx0 = HashField(victim);
            for (size_t p = 0; p < kProbeMax; ++p) {
                size_t idx = (idx0 + p) & g_stampMask;
                if (g_stamps[idx].field.load(std::memory_order_relaxed) == static_cast<uintptr_t>(victim)) {
                    g_stamps[idx].field.store(0, std::memory_order_relaxed);
                    break;
                }
            }
        }
    } else {
        // Ensure not found as stamp even if still present under different path.
        if (g_stamps != nullptr) {
            size_t idx0 = HashField(victim);
            for (size_t p = 0; p < kProbeMax; ++p) {
                size_t idx = (idx0 + p) & g_stampMask;
                if (g_stamps[idx].field.load(std::memory_order_relaxed) == static_cast<uintptr_t>(victim)) {
                    g_stamps[idx].field.store(0, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }
    CensusStats localDisp;
    ClassifyMiss(localDisp, victim, nullptr, nullptr);
    bool okDisp = localDisp.missBareDisplaced > 0;
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=missBareDisplaced forced=%zu stampEvictedDelta=%llu ok=%d "
        "healthyExpect=>0 when key was force-overwritten then bare at census",
        localDisp.missBareDisplaced,
        static_cast<unsigned long long>(g_stampEvicted.load(std::memory_order_relaxed) - ev0),
        okDisp ? 1 : 0);

    MAddress lostSlot = static_cast<MAddress>(0x1000ull + 0x30ull);
    StoreStamp(lostSlot, static_cast<uint8_t>(GC_PHASE_FORWARD), /*recorded=*/true, kGenOld, kGenYoung);
    CensusStats local2;
    ClassifyMiss(local2, lostSlot, nullptr, nullptr);
    bool okLost = local2.missRecordedLost > 0;
    bool okGt8 = local2.missPhaseGt8 > 0;
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=missRecordedLost forced=%zu ok=%d "
        "healthyExpect=>0 when stamp.recorded=1 but slot not in remset snap",
        local2.missRecordedLost, okLost ? 1 : 0);
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=missPhaseGt8 forced=%zu ok=%d "
        "healthyExpect=>0 when write phase=FORWARD",
        local2.missPhaseGt8, okGt8 ? 1 : 0);

    MAddress le8Slot = static_cast<MAddress>(0x1000ull + 0x48ull);
    StoreStamp(le8Slot, static_cast<uint8_t>(GC_PHASE_IDLE), /*recorded=*/true, kGenOld, kGenYoung);
    CensusStats local3;
    ClassifyMiss(local3, le8Slot, nullptr, nullptr);
    bool okLe8 = local3.missPhaseLe8 > 0;
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=missPhaseLe8 forced=%zu ok=%d "
        "healthyExpect=>0 when write phase=IDLE",
        local3.missPhaseLe8, okLe8 ? 1 : 0);

    // 3) remsetSize / oldToYoungEdges are walk-derived — cannot force without heap.
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=remsetSize status=NO_FORCE "
        "reason=needs live RememberedSet::Snapshot; prove via load census remsetSize>0");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=oldToYoungEdges status=NO_FORCE "
        "reason=needs heap ForEachObj walk; prove via load census edges>0");

    // 4) fullclear matrix cells — stamp promote table if gate on.
    if (FullClearProbeOn()) {
        uint64_t m0 = g_fcCensusBareTotal.load(std::memory_order_relaxed);
        NotePromoteTimeTarget(static_cast<MAddress>(0xC0FFEE00ull), kGenOld, false);
        NoteFullClearMiss(static_cast<MAddress>(0xC0FFEE00ull), nullptr, true);
        // target null → censusGen nonheap; force young by calling with a fake path:
        // NoteFullClearMiss uses CensusTargetGenOf(target); nullptr → nonheap.
        // Still proves promote stamp path + noStamp path not stuck.
        uint64_t m1 = g_fcCensusBareTotal.load(std::memory_order_relaxed);
        bool okFc = m1 > m0;
        LOG(RTLOG_ERROR,
            "[GCV2][diag][SELFTEST] counter=fullclear.censusBare forcedDelta=%llu ok=%d "
            "healthyExpect=>0 under FULLCLEAR_PROBE",
            static_cast<unsigned long long>(m1 - m0), okFc ? 1 : 0);
    } else {
        LOG(RTLOG_ERROR,
            "[GCV2][diag][SELFTEST] counter=fullclear.* status=SKIPPED gate_off "
            "enable=MRT_GCV2_FULLCLEAR_PROBE=1 or MRT_GCV2_DIAG=fullclear");
    }

    // 5) grant/already/tooLate — product Ensure path; cannot force here without region.
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=grant status=NO_FORCE "
        "reason=EnsureRouteDomainMembership needs live region; "
        "read legend: grant=0 means already-in-domain NOT failure "
        "(prove via pregrant already>>0 tooLate=0)");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=promotegap.* status=NO_FORCE "
        "reason=lives in RegionManager promote walk; enable PROMOTEGAP_PROBE under load");

    EmitInstrumentHealth("selftest", /*remsetSize=*/0, /*oldToYoungEdges=*/0);

    // Leave a clean epoch for the real census that follows (selftest runs inside first census).
    uint64_t clears0 = g_stampClears.load(std::memory_order_relaxed);
    ClearWriteStampsAfterCensus();
    bool okClear = g_stampClears.load(std::memory_order_relaxed) > clears0 && CountStampOccupied() == 0;
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] counter=stampClears forcedDelta=%llu occAfter=%zu ok=%d "
        "healthyExpect=clear zeros table; one clear per census",
        static_cast<unsigned long long>(g_stampClears.load(std::memory_order_relaxed) - clears0),
        CountStampOccupied(), okClear ? 1 : 0);

    int pass = (okNotes ? 1 : 0) + (okWrapOrFail ? 1 : 0) + (okBare ? 1 : 0) + (okLost ? 1 : 0) +
        (okGt8 ? 1 : 0) + (okLe8 ? 1 : 0) + (okNever ? 1 : 0) + (okDisp ? 1 : 0) + (okClear ? 1 : 0);
    LOG(RTLOG_ERROR,
        "[GCV2][diag][SELFTEST] summary forcedPass=%d/9 "
        "noForce=remsetSize,oldToYoungEdges,grant,promotegap "
        "rule=do_not_conclude_from_counter_without_positive_arm",
        pass);
}

} // namespace IdleEdgeDiag
} // namespace MapleRuntime
