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

struct StampSlot {
    std::atomic<uintptr_t> field{ 0 };
    std::atomic<uint8_t> phase{ 0 };
    std::atomic<uint8_t> recorded{ 0 };
};

StampSlot* g_stamps = nullptr;
size_t g_stampCap = 0;
size_t g_stampMask = 0;
std::atomic<uint32_t> g_stampInited{ 0 };

std::atomic<uint64_t> g_stampNotes{ 0 };
std::atomic<uint64_t> g_stampWraps{ 0 };
std::atomic<uint64_t> g_stampProbeFail{ 0 };

std::atomic<uint64_t> g_minorsCensused{ 0 };
std::atomic<uint64_t> g_edgesTotal{ 0 };
std::atomic<uint64_t> g_remsetHit{ 0 };
std::atomic<uint64_t> g_remsetMiss{ 0 };
std::atomic<uint64_t> g_missBare{ 0 };
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

size_t HashField(MAddress field)
{
    uintptr_t x = static_cast<uintptr_t>(field) >> 3;
    x ^= x >> 17;
    x *= 0x9e3779b97f4a7c15ull;
    x ^= x >> 13;
    x *= 0xc2b2ae3d27d4eb4full;
    return static_cast<size_t>(x) & g_stampMask;
}

void StoreStamp(MAddress fieldAddress, uint8_t phase, bool recorded)
{
    EnsureStampTable();
    const uintptr_t key = static_cast<uintptr_t>(fieldAddress);
    size_t idx0 = HashField(fieldAddress);
    for (size_t p = 0; p < kProbeMax; ++p) {
        size_t idx = (idx0 + p) & g_stampMask;
        StampSlot& slot = g_stamps[idx];
        uintptr_t prev = slot.field.load(std::memory_order_relaxed);
        if (prev == key) {
            slot.phase.store(phase, std::memory_order_relaxed);
            slot.recorded.store(recorded ? 1 : 0, std::memory_order_relaxed);
            g_stampNotes.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (prev == 0) {
            uintptr_t expected = 0;
            if (slot.field.compare_exchange_strong(expected, key, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                slot.phase.store(phase, std::memory_order_relaxed);
                slot.recorded.store(recorded ? 1 : 0, std::memory_order_relaxed);
                g_stampNotes.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (expected == key) {
                slot.phase.store(phase, std::memory_order_relaxed);
                slot.recorded.store(recorded ? 1 : 0, std::memory_order_relaxed);
                g_stampNotes.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            g_stampWraps.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        g_stampWraps.fetch_add(1, std::memory_order_relaxed);
    }
    StampSlot& slot = g_stamps[idx0];
    g_stampProbeFail.fetch_add(1, std::memory_order_relaxed);
    g_stampWraps.fetch_add(1, std::memory_order_relaxed);
    slot.phase.store(phase, std::memory_order_relaxed);
    slot.recorded.store(recorded ? 1 : 0, std::memory_order_relaxed);
    slot.field.store(key, std::memory_order_release);
    g_stampNotes.fetch_add(1, std::memory_order_relaxed);
}

struct StampLookup {
    bool found = false;
    uint8_t phase = 0;
    bool recorded = false;
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

void ClassifyMiss(CensusStats& stats, MAddress fieldAddress, BaseObject* holder)
{
    ++stats.remsetMiss;
    StampLookup st = LoadStamp(fieldAddress);
    if (!st.found) {
        ++stats.missBare;
        ++stats.missByPhase[0];
        NoteBareHolder(holder);
        NoteBareOffset(holder, fieldAddress);
        PushSample(stats, fieldAddress);
        return;
    }
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

bool Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_IDLEEDGE");
    return on;
}

void NoteBarrierDecision(MAddress fieldAddress, GCPhase phase, bool recorded)
{
    if (!Enabled() || fieldAddress == 0) {
        return;
    }
    StoreStamp(fieldAddress, static_cast<uint8_t>(phase), recorded);
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
                    ClassifyMiss(stats, slot, holder);
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

    VLOG(REPORT,
         "[GCV2][idleedge] point=pre-pinned-stamp invoke=%zu minorRun=%zu env=MRT_GCV2_IDLEEDGE=1 "
         "remsetSize=%zu holdersScanned=%zu oldToYoungEdges=%zu remsetHit=%zu remsetMiss=%zu "
         "missPct=%.2f missBare=%zu missPhaseLe8=%zu missPhaseGt8=%zu missRecordedLost=%zu missEarly=%zu "
         "idleClassOfMiss=%zu (%.1f%%) gt8OfMiss=%zu (%.1f%%) costNs=%llu stampNotes=%llu stampWraps=%llu "
         "stampCap=%zu stampProbeFail=%llu attrRaw=%llu attrOther=%llu "
         "missSamples=[%p,%p,%p,%p]",
         invoke, minorRunIndex, stats.remsetSize, stats.holdersScanned, stats.edgesTotal, stats.remsetHit,
         stats.remsetMiss, missPct, stats.missBare, stats.missPhaseLe8, stats.missPhaseGt8, stats.missRecordedLost,
         stats.missEarly, idleClass, idlePctOfMiss, stats.missPhaseGt8, gt8PctOfMiss,
         static_cast<unsigned long long>(stats.costNs), static_cast<unsigned long long>(g_stampNotes.load()),
         static_cast<unsigned long long>(g_stampWraps.load()), g_stampCap,
         static_cast<unsigned long long>(g_stampProbeFail.load()),
         static_cast<unsigned long long>(g_attrRawArray.load()),
         static_cast<unsigned long long>(g_attrOther.load()),
         reinterpret_cast<void*>(stats.missSamples[0]),
         reinterpret_cast<void*>(stats.missSamples[1]), reinterpret_cast<void*>(stats.missSamples[2]),
         reinterpret_cast<void*>(stats.missSamples[3]));

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

    VLOG(REPORT,
         "[GCV2][idleedge][TOTAL] tag=%s minors=%llu edges=%llu hit=%llu miss=%llu missPct=%.2f "
         "perMinorEdges=%.1f perMinorMiss=%.1f missBare=%llu missPhaseLe8=%llu missPhaseGt8=%llu "
         "missRecordedLost=%llu missEarly=%llu idleClassOfMiss=%llu (%.1f%%) gt8OfMiss=%llu (%.1f%%) "
         "costNsTotal=%llu stampNotes=%llu stampWraps=%llu stampCap=%zu stampProbeFail=%llu "
         "attrRawArray=%llu attrOther=%llu attrUnknown=%llu env=MRT_GCV2_IDLEEDGE=1",
         tag == nullptr ? "?" : tag, static_cast<unsigned long long>(minors), static_cast<unsigned long long>(edges),
         static_cast<unsigned long long>(hit), static_cast<unsigned long long>(miss), missPct, perMinorEdges,
         perMinorMiss, static_cast<unsigned long long>(bare), static_cast<unsigned long long>(le8),
         static_cast<unsigned long long>(gt8), static_cast<unsigned long long>(lost),
         static_cast<unsigned long long>(early), static_cast<unsigned long long>(idleClass), idlePct,
         static_cast<unsigned long long>(gt8), gt8Pct, static_cast<unsigned long long>(cost),
         static_cast<unsigned long long>(g_stampNotes.load()), static_cast<unsigned long long>(g_stampWraps.load()),
         g_stampCap, static_cast<unsigned long long>(g_stampProbeFail.load()),
         static_cast<unsigned long long>(g_attrRawArray.load()),
         static_cast<unsigned long long>(g_attrOther.load()),
         static_cast<unsigned long long>(g_attrUnknown.load()));

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
}

} // namespace IdleEdgeDiag
} // namespace MapleRuntime
