// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/FysDesignDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace FysDesignDiag {
namespace {

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

bool GateOn()
{
    static const bool on = []() {
        if (EnvIsOne("MRT_GCV2_FYSDESIGN")) {
            return true;
        }
        return DiagGate::TokenOn("fysdesign");
    }();
    return on;
}

// Region-type buckets for non-young holders (aligned to RegionInfo::RegionType).
enum HolderBucket : size_t {
    HB_RECENT_FULL = 0,
    HB_PINNED = 1,   // FULL_PINNED / RECENT_PINNED / RAW_POINTER*
    HB_LARGE = 2,    // LARGE / RECENT_LARGE
    HB_FROM = 3,     // FROM / LONE_FROM / UNMOVABLE_FROM / TO
    HB_OTHER = 4,
    HB_COUNT = 5,
};

const char* HolderBucketName(size_t b)
{
    switch (b) {
        case HB_RECENT_FULL:
            return "recent_full";
        case HB_PINNED:
            return "pinned";
        case HB_LARGE:
            return "large";
        case HB_FROM:
            return "from_like";
        default:
            return "other_old";
    }
}

size_t ClassifyHolder(RegionInfo* region)
{
    if (region == nullptr) {
        return HB_OTHER;
    }
    using RT = RegionInfo::RegionType;
    switch (region->GetRegionType()) {
        case RT::RECENT_FULL_REGION:
            return HB_RECENT_FULL;
        case RT::FULL_PINNED_REGION:
        case RT::RECENT_PINNED_REGION:
        case RT::RAW_POINTER_PINNED_REGION:
        case RT::TL_RAW_POINTER_REGION:
        case RT::TL_LARGE_RAW_POINTER_REGION:
            return HB_PINNED;
        case RT::LARGE_REGION:
        case RT::RECENT_LARGE_REGION:
            return HB_LARGE;
        case RT::FROM_REGION:
        case RT::LONE_FROM_REGION:
        case RT::UNMOVABLE_FROM_REGION:
        case RT::TO_REGION:
            return HB_FROM;
        default:
            return HB_OTHER;
    }
}

struct Counters {
    size_t minorRun = 0;
    size_t holdersTotal = 0;
    size_t holdersYoung = 0;
    size_t holdersOld = 0;
    size_t edgeTotal = 0;
    size_t edgeNullOrNonheap = 0;
    size_t edgeYoungYoung = 0;
    size_t edgeYoungToOld = 0;
    size_t edgeOldToOld = 0;
    size_t edgeOldToYoung = 0;
    size_t edgeO2YInRemset = 0;
    size_t edgeO2YFysOnly = 0;
    size_t remsetSize = 0;
    size_t remsetHitsOnO2Y = 0; // same as edgeO2YInRemset
    size_t fysOnlyByBucket[HB_COUNT] = {};
    size_t o2yByBucket[HB_COUNT] = {};
    size_t samplesEmitted = 0;
};

Counters g_c;

} // namespace

bool Enabled() { return GateOn(); }

void OnMinorBegin(size_t minorRunIndex)
{
    if (!GateOn()) {
        return;
    }
    g_c = Counters{};
    g_c.minorRun = minorRunIndex;
}

void Census(const std::vector<BaseObject*>& reachableVec,
            const std::unordered_set<MAddress>& rememberedSlots, bool fullYoungScan,
            BaseObject* (*resolve)(RefField<>& field))
{
    if (!GateOn() || resolve == nullptr) {
        return;
    }
    g_c.remsetSize = rememberedSlots.size();
    const size_t sampleCap = EnvSizeT("MRT_GCV2_FYSDESIGN_SAMPLES", 8);
    for (BaseObject* object : reachableVec) {
        if (object == nullptr || !Heap::IsHeapAddress(object)) {
            continue;
        }
        ++g_c.holdersTotal;
        RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (holderRegion == nullptr) {
            continue;
        }
        const bool holderYoung = holderRegion->IsYoungRegion();
        if (holderYoung) {
            ++g_c.holdersYoung;
        } else {
            ++g_c.holdersOld;
        }
        if (!object->HasRefField()) {
            continue;
        }
        const size_t bucket = holderYoung ? HB_OTHER : ClassifyHolder(holderRegion);
        object->ForEachRefField([&](RefField<>& field) {
            ++g_c.edgeTotal;
            BaseObject* target = resolve(field);
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                ++g_c.edgeNullOrNonheap;
                return;
            }
            RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion == nullptr) {
                ++g_c.edgeNullOrNonheap;
                return;
            }
            const bool targetYoung = targetRegion->IsYoungRegion();
            if (holderYoung && targetYoung) {
                ++g_c.edgeYoungYoung;
                return;
            }
            if (holderYoung && !targetYoung) {
                ++g_c.edgeYoungToOld;
                return;
            }
            if (!holderYoung && !targetYoung) {
                ++g_c.edgeOldToOld;
                return;
            }
            // old → young: remset's contract surface
            ++g_c.edgeOldToYoung;
            ++g_c.o2yByBucket[bucket];
            MAddress slot = reinterpret_cast<MAddress>(&field);
            if (rememberedSlots.count(slot) != 0) {
                ++g_c.edgeO2YInRemset;
                ++g_c.remsetHitsOnO2Y;
            } else {
                ++g_c.edgeO2YFysOnly;
                ++g_c.fysOnlyByBucket[bucket];
                if (g_c.samplesEmitted < sampleCap) {
                    ++g_c.samplesEmitted;
                    VLOG(REPORT,
                         "[GCV2][fysdesign][SAMPLE] fys_only O→Y holder=%p hType=%u hBucket=%s "
                         "target=%p tType=%u slot=%#zx fullYoung=%u remsetSz=%zu",
                         object, static_cast<unsigned>(holderRegion->GetRegionType()), HolderBucketName(bucket),
                         target, static_cast<unsigned>(targetRegion->GetRegionType()),
                         static_cast<size_t>(slot), static_cast<unsigned>(fullYoungScan), g_c.remsetSize);
                }
            }
        });
    }
    (void)fullYoungScan;
}

void Report(const char* tag)
{
    if (!GateOn()) {
        return;
    }
    const char* t = tag != nullptr ? tag : "census";
    VLOG(REPORT,
         "[GCV2][fysdesign][%s] minor=%zu holders=%zu youngH=%zu oldH=%zu edges=%zu "
         "null=%zu YY=%zu YO=%zu OO=%zu OY=%zu OY_inRemset=%zu OY_fysOnly=%zu remsetSz=%zu",
         t, g_c.minorRun, g_c.holdersTotal, g_c.holdersYoung, g_c.holdersOld, g_c.edgeTotal,
         g_c.edgeNullOrNonheap, g_c.edgeYoungYoung, g_c.edgeYoungToOld, g_c.edgeOldToOld, g_c.edgeOldToYoung,
         g_c.edgeO2YInRemset, g_c.edgeO2YFysOnly, g_c.remsetSize);
    VLOG(REPORT,
         "[GCV2][fysdesign][%s] OY_fysOnly_by_holder: recent_full=%zu pinned=%zu large=%zu "
         "from_like=%zu other_old=%zu",
         t, g_c.fysOnlyByBucket[HB_RECENT_FULL], g_c.fysOnlyByBucket[HB_PINNED], g_c.fysOnlyByBucket[HB_LARGE],
         g_c.fysOnlyByBucket[HB_FROM], g_c.fysOnlyByBucket[HB_OTHER]);
    VLOG(REPORT,
         "[GCV2][fysdesign][%s] OY_total_by_holder: recent_full=%zu pinned=%zu large=%zu "
         "from_like=%zu other_old=%zu",
         t, g_c.o2yByBucket[HB_RECENT_FULL], g_c.o2yByBucket[HB_PINNED], g_c.o2yByBucket[HB_LARGE],
         g_c.o2yByBucket[HB_FROM], g_c.o2yByBucket[HB_OTHER]);
    // Direct answer line for the report: fraction of O→Y that remset already had.
    double fracIn = 0.0;
    if (g_c.edgeOldToYoung != 0) {
        fracIn = 100.0 * static_cast<double>(g_c.edgeO2YInRemset) / static_cast<double>(g_c.edgeOldToYoung);
    }
    VLOG(REPORT,
         "[GCV2][fysdesign][%s] VERDICT OY_inRemset_pct=%.2f fysOnly=%zu (if fysOnly=0 FYS buys zero "
         "extra O→Y vs remset on claimed holders)",
         t, fracIn, g_c.edgeO2YFysOnly);
}

} // namespace FysDesignDiag
} // namespace MapleRuntime
