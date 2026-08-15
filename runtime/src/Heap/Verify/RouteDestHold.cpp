// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/RouteDestHold.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
namespace RouteDestHold {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

constexpr size_t kSiteCount = static_cast<size_t>(Site::SITE_COUNT);

const char* SiteName(Site site)
{
    switch (site) {
        case Site::ASSEMBLE_RECENT_FULL:
            return "assemble_recent_full";
        case Site::ASSEMBLE_UNMOVABLE:
            return "assemble_unmovable";
        case Site::YOUNG_UNMOVABLE:
            return "young_unmovable";
        case Site::YOUNG_RECENT_FULL:
            return "young_recent_full";
        case Site::TAKE_GARBAGE:
            return "take_garbage";
        case Site::TAKE_AFTER_DISPEL:
            return "take_after_dispel";
        default:
            return "unknown";
    }
}

// Would have been reclaimed while a route named it, per site. With inject on this is the
// negative arm's count; with inject off it is the count the gate actually refused.
std::atomic<size_t> g_held[kSiteCount];
// Actually refused (inject off only).
std::atomic<size_t> g_blocked[kSiteCount];

std::atomic<size_t> g_reuseTotal{ 0 };
std::atomic<size_t> g_reuseWhileRouted{ 0 };
std::atomic<size_t> g_funnelHeld{ 0 };
std::atomic<size_t> g_clearPoints{ 0 };
std::atomic<size_t> g_clearHeldRegions{ 0 };
std::atomic<size_t> g_clearHeldBytes{ 0 };
std::atomic<size_t> g_clearHeldPeak{ 0 };
std::atomic<size_t> g_to2Resolve{ 0 };
std::atomic<size_t> g_to2Divergent{ 0 };
std::atomic<size_t> g_logged{ 0 };
std::atomic<bool> g_atexit{ false };

constexpr size_t kMaxSamples = 32;
// Summary cadence. Route generations turn over several times per second under load, so this
// is frequent enough that a crash never costs more than a few cycles of counts.
constexpr size_t kSummaryEvery = 16;

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        (void)std::atexit([]() { DumpSummary(); });
    }
}

} // namespace

bool AccountOn()
{
    static const bool on = EnvIsOne("MRT_GCV2_ROUTE_DEST_ACCOUNT");
    return on;
}

bool InjectHandbackOn()
{
    static const bool on = EnvIsOne("MRT_GCV2_ROUTE_DEST_INJECT_HANDBACK");
    return on;
}

bool HoldsBack(const RegionInfo* region, Site site)
{
    if (region == nullptr || !region->IsRouteDestHeld()) {
        return false;
    }
    const size_t idx = static_cast<size_t>(site);
    if (AccountOn()) {
        EnsureAtexit();
        g_held[idx].fetch_add(1, std::memory_order_relaxed);
    }
    if (InjectHandbackOn()) {
        // Negative arm: report "not held" so the region is reclaimed exactly as it is
        // without this change, while the stamp, the clear and the counters stay live.
        return false;
    }
    if (AccountOn()) {
        g_blocked[idx].fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void NoteReuse(const RegionInfo* region, bool held)
{
    if (!AccountOn()) {
        return;
    }
    EnsureAtexit();
    g_reuseTotal.fetch_add(1, std::memory_order_relaxed);
    if (!held) {
        return;
    }
    g_reuseWhileRouted.fetch_add(1, std::memory_order_relaxed);
    size_t n = g_logged.fetch_add(1, std::memory_order_relaxed);
    if (n < kMaxSamples && region != nullptr) {
        LOG(RTLOG_ERROR,
            "[GCV2][routedest] reuse_while_routed region=%p start=%#zx inject=%u — payload was "
            "zeroed by ClearUnits while a published route still named this region",
            region, region->GetRegionStart(), static_cast<unsigned>(InjectHandbackOn()));
    }
}

void NoteReclaimFunnel(const RegionInfo* region, const char* site)
{
    if (!AccountOn() || region == nullptr || !region->IsRouteDestHeld()) {
        return;
    }
    EnsureAtexit();
    g_funnelHeld.fetch_add(1, std::memory_order_relaxed);
    VLOG(REPORT, "[GCV2][routedest] funnel site=%s region=%p start=%#zx held=1", site, region,
         region->GetRegionStart());
}

void NoteClearPoint(size_t heldRegions, size_t heldBytes)
{
    if (!AccountOn()) {
        return;
    }
    EnsureAtexit();
    g_clearPoints.fetch_add(1, std::memory_order_relaxed);
    g_clearHeldRegions.fetch_add(heldRegions, std::memory_order_relaxed);
    g_clearHeldBytes.fetch_add(heldBytes, std::memory_order_relaxed);
    size_t peak = g_clearHeldPeak.load(std::memory_order_relaxed);
    while (heldBytes > peak && !g_clearHeldPeak.compare_exchange_weak(peak, heldBytes, std::memory_order_relaxed)) {
    }
    VLOG(REPORT, "[GCV2][routedest] clear_point held_regions=%zu held_bytes=%zu", heldRegions, heldBytes);
    // The workload under measurement dies on SIGSEGV, which skips atexit. Emit the running
    // summary periodically so the numbers survive the crash they are meant to explain.
    size_t n = g_clearPoints.load(std::memory_order_relaxed);
    if (n % kSummaryEvery == 0) {
        DumpSummary();
    }
}

void NoteTo2Resolve(uintptr_t arith, uint32_t idx)
{
    if (!AccountOn()) {
        return;
    }
    EnsureAtexit();
    g_to2Resolve.fetch_add(1, std::memory_order_relaxed);
    RegionInfo* owner = RegionInfo::GetRegionInfo(idx);
    if (owner == nullptr) {
        return;
    }
    uintptr_t viaLookup = static_cast<uintptr_t>(owner->GetRegionStart());
    if (viaLookup != arith) {
        g_to2Divergent.fetch_add(1, std::memory_order_relaxed);
        VLOG(REPORT, "[GCV2][routedest] to2_divergent idx=%u arith=%#zx lookup=%#zx", idx, arith, viaLookup);
    }
}

void DumpSummary()
{
    if (!AccountOn()) {
        return;
    }
    size_t heldTotal = 0;
    size_t blockedTotal = 0;
    for (size_t i = 0; i < kSiteCount; ++i) {
        heldTotal += g_held[i].load(std::memory_order_relaxed);
        blockedTotal += g_blocked[i].load(std::memory_order_relaxed);
    }
    LOG(RTLOG_ERROR,
        "[GCV2][routedest] SUMMARY inject=%u reused_while_routed=%zu reuse_total=%zu "
        "would_reclaim_held=%zu blocked=%zu funnel_held=%zu clear_points=%zu clear_held_regions=%zu "
        "clear_held_bytes_peak=%zu to2_resolve=%zu to2_divergent=%zu",
        static_cast<unsigned>(InjectHandbackOn()), g_reuseWhileRouted.load(std::memory_order_relaxed),
        g_reuseTotal.load(std::memory_order_relaxed), heldTotal, blockedTotal,
        g_funnelHeld.load(std::memory_order_relaxed), g_clearPoints.load(std::memory_order_relaxed),
        g_clearHeldRegions.load(std::memory_order_relaxed), g_clearHeldPeak.load(std::memory_order_relaxed),
        g_to2Resolve.load(std::memory_order_relaxed), g_to2Divergent.load(std::memory_order_relaxed));
    for (size_t i = 0; i < kSiteCount; ++i) {
        size_t h = g_held[i].load(std::memory_order_relaxed);
        size_t b = g_blocked[i].load(std::memory_order_relaxed);
        if (h == 0 && b == 0) {
            continue;
        }
        LOG(RTLOG_ERROR, "[GCV2][routedest] SITE %s would_reclaim_held=%zu blocked=%zu",
            SiteName(static_cast<Site>(i)), h, b);
    }
}

} // namespace RouteDestHold
} // namespace MapleRuntime
