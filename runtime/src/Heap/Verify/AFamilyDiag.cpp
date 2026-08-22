// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/AFamilyDiag.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace AFamilyDiag {
namespace {

constexpr size_t kSlots = 1u << 20;
constexpr size_t kMask = kSlots - 1;

struct Claim {
    std::atomic<uintptr_t> obj{ 0 };
    std::atomic<uint8_t> ch{ 0 };
    std::atomic<uint32_t> gc{ 0 };
};

Claim g_map[kSlots];

std::atomic<uint64_t> g_claimTrace{ 0 };
std::atomic<uint64_t> g_claimWater{ 0 };
std::atomic<uint64_t> g_claimTraceReg{ 0 };
std::atomic<uint64_t> g_claimCompact{ 0 };

std::atomic<uint64_t> g_azero{ 0 };
std::atomic<uint64_t> g_holderTrace{ 0 };
std::atomic<uint64_t> g_holderWater{ 0 };
std::atomic<uint64_t> g_holderTraceReg{ 0 };
std::atomic<uint64_t> g_holderCompact{ 0 };
std::atomic<uint64_t> g_holderNone{ 0 };

std::atomic<bool> g_atexit{ false };

const char* ChName(uint8_t ch)
{
    switch (ch) {
        case CH_TRACE:
            return "trace";
        case CH_ALLOC_WATER:
            return "alloc_water";
        case CH_TRACE_REGION:
            return "trace_region";
        case CH_COMPACT:
            return "compact";
        default:
            return "none";
    }
}

size_t Hash(uintptr_t p)
{
    return (p * 11400714819323198485ull) >> (64 - 20);
}

void InstallAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { ReportSummary("atexit"); });
    }
}

} // namespace

bool Enabled()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        return DiagGate::LegacyOrToken("MRT_GCV2_AFAMILY", "afamily");
    }();
    return on;
}

void NoteClaim(BaseObject* obj, Channel ch)
{
    if (!Enabled() || obj == nullptr) {
        return;
    }
    InstallAtexit();
    const uintptr_t p = reinterpret_cast<uintptr_t>(obj);
    const uint32_t gc = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    size_t i = Hash(p);
    for (size_t n = 0; n < 8; ++n) {
        Claim& rec = g_map[(i + n) & kMask];
        uintptr_t cur = rec.obj.load(std::memory_order_relaxed);
        if (cur == 0) {
            uintptr_t exp = 0;
            if (rec.obj.compare_exchange_strong(exp, p, std::memory_order_relaxed)) {
                rec.ch.store(static_cast<uint8_t>(ch), std::memory_order_relaxed);
                rec.gc.store(gc, std::memory_order_relaxed);
                break;
            }
            cur = rec.obj.load(std::memory_order_relaxed);
        }
        if (cur == p) {
            uint8_t old = rec.ch.load(std::memory_order_relaxed);
            // TRACE overwrites implicit-black; COMPACT overwrites water/trace-region.
            if (ch == CH_TRACE || (ch == CH_COMPACT && old != CH_TRACE)) {
                rec.ch.store(static_cast<uint8_t>(ch), std::memory_order_relaxed);
                rec.gc.store(gc, std::memory_order_relaxed);
            }
            break;
        }
    }
    switch (ch) {
        case CH_TRACE:
            g_claimTrace.fetch_add(1, std::memory_order_relaxed);
            break;
        case CH_ALLOC_WATER:
            g_claimWater.fetch_add(1, std::memory_order_relaxed);
            break;
        case CH_TRACE_REGION:
            g_claimTraceReg.fetch_add(1, std::memory_order_relaxed);
            break;
        case CH_COMPACT:
            g_claimCompact.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

static uint8_t Lookup(BaseObject* obj, uint32_t* gcOut)
{
    if (obj == nullptr) {
        return CH_NONE;
    }
    const uintptr_t p = reinterpret_cast<uintptr_t>(obj);
    size_t i = Hash(p);
    for (size_t n = 0; n < 8; ++n) {
        Claim& rec = g_map[(i + n) & kMask];
        if (rec.obj.load(std::memory_order_relaxed) == p) {
            if (gcOut != nullptr) {
                *gcOut = rec.gc.load(std::memory_order_relaxed);
            }
            return rec.ch.load(std::memory_order_relaxed);
        }
    }
    return CH_NONE;
}

void OnAZeroed(BaseObject* victim, BaseObject* holder, const void* slot, uint8_t phase, int hasTo)
{
    if (!Enabled()) {
        return;
    }
    InstallAtexit();
    const uint64_t n = g_azero.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t holderGc = 0;
    uint8_t ch = Lookup(holder, &holderGc);
    if (ch == CH_NONE && holder != nullptr) {
        RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
        if (hr != nullptr) {
            size_t off = hr->GetAddressOffset(reinterpret_cast<MAddress>(holder));
            if (hr->AllocatedAfterMarkStart(off)) {
                ch = CH_ALLOC_WATER;
            } else if (hr->IsTraceRegion()) {
                ch = CH_TRACE_REGION;
            }
        }
    }
    switch (ch) {
        case CH_TRACE:
            g_holderTrace.fetch_add(1, std::memory_order_relaxed);
            break;
        case CH_ALLOC_WATER:
            g_holderWater.fetch_add(1, std::memory_order_relaxed);
            break;
        case CH_TRACE_REGION:
            g_holderTraceReg.fetch_add(1, std::memory_order_relaxed);
            break;
        case CH_COMPACT:
            g_holderCompact.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            g_holderNone.fetch_add(1, std::memory_order_relaxed);
            break;
    }
    if ((n & (n - 1)) != 0 && n > 8) {
        return;
    }

    RegionInfo* vr = victim == nullptr ? nullptr :
        RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(victim));
    RegionInfo* hr = holder == nullptr ? nullptr :
        RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    int vYoung = -1;
    int hYoung = -1;
    unsigned vType = 0;
    unsigned hType = 0;
    int vPrev = -1;
    int water = 0;
    size_t vOff = 0;
    size_t hOff = 0;
    if (vr != nullptr) {
        vYoung = vr->IsYoungRegion() ? 1 : 0;
        vType = static_cast<unsigned>(vr->GetRegionType());
        vOff = vr->GetAddressOffset(reinterpret_cast<MAddress>(victim));
        vPrev = vr->IsRouteSurvivedObject(vOff) ? 1 : 0;
        water = vr->AllocatedAfterMarkStart(vOff) ? 1 : 0;
    }
    if (hr != nullptr) {
        hYoung = hr->IsYoungRegion() ? 1 : 0;
        hType = static_cast<unsigned>(hr->GetRegionType());
        hOff = hr->GetAddressOffset(reinterpret_cast<MAddress>(holder));
    }
    int inRemset = 0;
    if (slot != nullptr) {
        inRemset = Heap::GetHeap().GetRememberedSet().Contains(reinterpret_cast<MAddress>(slot)) ? 1 : 0;
    }
    const uint32_t gc = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    LOG(RTLOG_ERROR,
        "[AFAMILY] n=%lu victim=%p vYoung=%d vType=%u vOff=%zu vPrevMarked=%d vWater=%d "
        "holder=%p hYoung=%d hType=%u hOff=%zu hCh=%s hChGc=%u slot=%p inRemset=%d "
        "phase=%u phaseName=%s gc=%u hasTo=%d",
        static_cast<unsigned long>(n), static_cast<void*>(victim), vYoung, vType, vOff, vPrev, water,
        static_cast<void*>(holder), hYoung, hType, hOff, ChName(ch), holderGc, slot, inRemset,
        phase, Collector::GetGCPhaseName(static_cast<GCPhase>(phase)), gc, hasTo);
}

void ReportSummary(const char* point)
{
    if (!Enabled()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[AFAMILY][sum] point=%s azero=%lu holder{trace=%lu water=%lu traceReg=%lu compact=%lu none=%lu} "
        "claim{trace=%lu water=%lu traceReg=%lu compact=%lu}",
        point == nullptr ? "?" : point,
        static_cast<unsigned long>(g_azero.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_holderTrace.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_holderWater.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_holderTraceReg.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_holderCompact.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_holderNone.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_claimTrace.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_claimWater.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_claimTraceReg.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_claimCompact.load(std::memory_order_relaxed)));
}

} // namespace AFamilyDiag
} // namespace MapleRuntime
