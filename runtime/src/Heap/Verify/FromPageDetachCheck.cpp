// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/FromPageDetachCheck.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
namespace FromPageDetach {
namespace {

struct AtomicCounters {
    std::atomic<uint64_t> checks{ 0 };
    std::atomic<uint64_t> withEvidence{ 0 };
    std::atomic<uint64_t> activeTable{ 0 };
    std::atomic<uint64_t> retiredTable{ 0 };
    std::atomic<uint64_t> routeDestHeld{ 0 };
    std::atomic<uint64_t> forwardingPositive{ 0 };
    std::atomic<uint64_t> forwardingClaimed{ 0 };
    std::atomic<uint64_t> forwardingReleased{ 0 };
    std::atomic<uint64_t> copyInflight{ 0 };
};

constexpr size_t kSiteCount = static_cast<size_t>(Site::SITE_COUNT);
std::array<AtomicCounters, kSiteCount> g_counters;

AtomicCounters& At(Site site)
{
    const size_t i = static_cast<size_t>(site);
    CHECK(i < kSiteCount);
    return g_counters[i];
}

uint64_t Load(const std::atomic<uint64_t>& value)
{
    return value.load(std::memory_order_relaxed);
}

struct DumpAtExit {
    DumpAtExit() { std::atexit(DumpSummary); }
};
const DumpAtExit g_dumpAtExit;

} // namespace

const char* SiteName(Site site)
{
    static constexpr const char* kNames[kSiteCount] = {
        "collect_from_garbage",
        "take_garbage",
        "take_after_dispel",
        "reclaim_dirty",
        "reclaim_mark_quarantine",
        "release_region",
        "release_garbage_units",
        "take_garbage_reuse",
        "take_dirty_reuse",
        "take_released_reuse",
        "clear_units",
        "release_units",
        "init_free_units",
        "init_region_info",
    };
    const size_t i = static_cast<size_t>(site);
    return i < kSiteCount ? kNames[i] : "invalid";
}

bool FromPageDetachCheck(const RegionInfo* region, Site site)
{
    AtomicCounters& out = At(site);
    out.checks.fetch_add(1, std::memory_order_relaxed);
    if (region == nullptr) {
        return true;
    }

    const MAddress start = region->GetRegionStart();
    const size_t size = region->GetRegionSizeForDetachCheck();
    const bool activeTable = ForwardingTable::GetEntries(start) != nullptr;
    const bool retiredTable = ForwardingTable::RetiredCovers(start, size);
    const bool routeDestHeld = region->IsRouteDestHeld();
    const int32_t refCount = region->ForwardingRefCount();
    const bool forwardingPositive = refCount > 0;
    const bool forwardingClaimed = refCount < 0 || region->ForwardingClaimed();
    const bool forwardingReleased = refCount == 0 && region->IsForwardingDone();
    const bool copyInflight = region->CopyInflight() != 0;
    const bool any = activeTable || retiredTable || routeDestHeld || forwardingPositive || forwardingClaimed ||
        copyInflight;

    out.withEvidence.fetch_add(any ? 1 : 0, std::memory_order_relaxed);
    out.activeTable.fetch_add(activeTable ? 1 : 0, std::memory_order_relaxed);
    out.retiredTable.fetch_add(retiredTable ? 1 : 0, std::memory_order_relaxed);
    out.routeDestHeld.fetch_add(routeDestHeld ? 1 : 0, std::memory_order_relaxed);
    out.forwardingPositive.fetch_add(forwardingPositive ? 1 : 0, std::memory_order_relaxed);
    out.forwardingClaimed.fetch_add(forwardingClaimed ? 1 : 0, std::memory_order_relaxed);
    out.forwardingReleased.fetch_add(forwardingReleased ? 1 : 0, std::memory_order_relaxed);
    out.copyInflight.fetch_add(copyInflight ? 1 : 0, std::memory_order_relaxed);

    // Measurement arm: deliberately no wait, quarantine, rejection or mutation.
    return true;
}

Counters GetCounters(Site site)
{
    AtomicCounters& c = At(site);
    return Counters{ Load(c.checks), Load(c.withEvidence), Load(c.activeTable), Load(c.retiredTable),
                     Load(c.routeDestHeld), Load(c.forwardingPositive), Load(c.forwardingClaimed),
                     Load(c.forwardingReleased), Load(c.copyInflight) };
}

void DumpSummary()
{
    for (size_t i = 0; i < kSiteCount; ++i) {
        const Site site = static_cast<Site>(i);
        const Counters c = GetCounters(site);
        std::fprintf(stderr,
                     "[GCV2][detach-check] phase=measure site=%s checks=%llu evidence=%llu "
                     "active_table=%llu retired_table=%llu route_dest_held=%llu fwd_positive=%llu "
                     "fwd_claimed=%llu fwd_released=%llu copy_inflight=%llu\n",
                     SiteName(site), static_cast<unsigned long long>(c.checks),
                     static_cast<unsigned long long>(c.withEvidence),
                     static_cast<unsigned long long>(c.activeTable),
                     static_cast<unsigned long long>(c.retiredTable),
                     static_cast<unsigned long long>(c.routeDestHeld),
                     static_cast<unsigned long long>(c.forwardingPositive),
                     static_cast<unsigned long long>(c.forwardingClaimed),
                     static_cast<unsigned long long>(c.forwardingReleased),
                     static_cast<unsigned long long>(c.copyInflight));
    }
    std::fflush(stderr);
}

} // namespace FromPageDetach
} // namespace MapleRuntime
