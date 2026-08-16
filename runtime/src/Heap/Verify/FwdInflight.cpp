// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/FwdInflight.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace FwdInflight {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

constexpr size_t kSiteCount = static_cast<size_t>(Site::SITE_COUNT);
constexpr size_t kRetireCount = static_cast<size_t>(Retire::RETIRE_COUNT);

// One publication slot per thread. Sized past the GC worker count plus the mutator pool this
// runs under; overflow is counted rather than silently dropped, because a silent drop would
// make the headline number read low for a reason unrelated to the defect.
constexpr size_t kMaxThreads = 1024;
// The positive control's synthetic reader gets its own slot PER THREAD, at kMaxThreads + idx.
// A single shared inject slot would race between two threads retiring concurrently: one
// thread's synthetic reader would be counted as a hit by the other's scan, which would make
// the control arm report hits that the product arm cannot produce.
constexpr size_t kSlotCount = 2 * kMaxThreads;

size_t InjectSlotFor(size_t threadIdx) { return kMaxThreads + threadIdx; }

struct Slot {
    std::atomic<const void*> region;
    std::atomic<uint32_t> site;
};

Slot g_slots[kSlotCount];

std::atomic<size_t> g_nextThreadIdx{ 0 };
std::atomic<size_t> g_slotOverflow{ 0 };

std::atomic<size_t> g_published[kSiteCount];
std::atomic<size_t> g_retires[kRetireCount];
std::atomic<size_t> g_hits[kRetireCount];
std::atomic<size_t> g_hitsBySite[kSiteCount];
std::atomic<size_t> g_injectSeen{ 0 };
std::atomic<size_t> g_globalInflight{ 0 };
std::atomic<size_t> g_logged{ 0 };
std::atomic<bool> g_atexit{ false };

constexpr size_t kMaxSamples = 32;
// Retire edges turn over many times per second under load; a crash then costs at most a few
// cycles of counts. Same reasoning as RouteDestHold's kSummaryEvery.
constexpr size_t kSummaryEvery = 64;

const char* RetireName(Retire retire)
{
    switch (retire) {
        case Retire::DISPEL_GHOST:
            return "dispel_ghost";
        case Retire::TAKE_GARBAGE:
            return "take_garbage";
        case Retire::ARENA_RELEASE:
            return "arena_release";
        default:
            return "unknown";
    }
}

// Lazily assigned, never released. A thread that exits leaves its slot holding nullptr, which
// is exactly the state a scan should see for it.
size_t ThreadSlot()
{
    static thread_local size_t idx = g_nextThreadIdx.fetch_add(1, std::memory_order_relaxed);
    return idx;
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        (void)std::atexit([]() { DumpSummary(); });
    }
}

// Count published readers matching `region`; nullptr means "count every reader in flight".
size_t ScanSlots(const void* region, size_t* bySite)
{
    size_t hits = 0;
    for (size_t i = 0; i < kSlotCount; ++i) {
        const void* published = g_slots[i].region.load(std::memory_order_acquire);
        if (published == nullptr) {
            continue;
        }
        if (region != nullptr && published != region) {
            continue;
        }
        ++hits;
        uint32_t site = g_slots[i].site.load(std::memory_order_relaxed);
        if (bySite != nullptr && site < kSiteCount) {
            ++bySite[site];
        }
    }
    return hits;
}

} // namespace

bool Enabled()
{
    static const bool on = []() {
        if (EnvIsOne("MRT_GCV2_FWDINFLIGHT")) {
            return true;
        }
        return DiagGate::TokenOn("fwdinflight");
    }();
    return on;
}

bool InjectOn()
{
    static const bool on = EnvIsOne("MRT_GCV2_FWDINFLIGHT_INJECT");
    return on;
}

Scope::Scope(const RegionInfo* region, Site site) : armed(false)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    size_t idx = ThreadSlot();
    if (idx >= kMaxThreads) {
        g_slotOverflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    EnsureAtexit();
    g_published[static_cast<size_t>(site)].fetch_add(1, std::memory_order_relaxed);
    g_slots[idx].site.store(static_cast<uint32_t>(site), std::memory_order_relaxed);
    g_slots[idx].region.store(region, std::memory_order_release);
    armed = true;
}

Scope::~Scope()
{
    if (!armed) {
        return;
    }
    g_slots[ThreadSlot()].region.store(nullptr, std::memory_order_release);
}

void NoteRetireRegion(const RegionInfo* region, Retire retire)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    EnsureAtexit();
    const size_t r = static_cast<size_t>(retire);
    g_retires[r].fetch_add(1, std::memory_order_relaxed);

    // Positive control: plant a synthetic reader on this very region so the scan below must
    // find it. Without this, "hits=0" cannot be told apart from a scan that never looks.
    const size_t injectIdx = InjectSlotFor(ThreadSlot() < kMaxThreads ? ThreadSlot() : 0);
    const bool inject = InjectOn();
    if (inject) {
        g_slots[injectIdx].site.store(static_cast<uint32_t>(Site::ROUTE_LOOKUP), std::memory_order_relaxed);
        g_slots[injectIdx].region.store(region, std::memory_order_release);
    }

    size_t bySite[kSiteCount] = { 0 };
    size_t hits = ScanSlots(region, bySite);

    if (inject) {
        g_slots[injectIdx].region.store(nullptr, std::memory_order_release);
        if (hits > 0) {
            g_injectSeen.fetch_add(1, std::memory_order_relaxed);
        }
        // The synthetic reader is not evidence about the product path. Discount it so the
        // headline number keeps meaning the same thing in both arms.
        --hits;
        if (bySite[static_cast<size_t>(Site::ROUTE_LOOKUP)] > 0) {
            --bySite[static_cast<size_t>(Site::ROUTE_LOOKUP)];
        }
    }

    if (hits == 0) {
        return;
    }
    g_hits[r].fetch_add(hits, std::memory_order_relaxed);
    for (size_t i = 0; i < kSiteCount; ++i) {
        if (bySite[i] != 0) {
            g_hitsBySite[i].fetch_add(bySite[i], std::memory_order_relaxed);
        }
    }
    size_t n = g_logged.fetch_add(1, std::memory_order_relaxed);
    if (n < kMaxSamples) {
        LOG(RTLOG_ERROR,
            "[GCV2][fwdinflight] hit retire=%s region=%p start=%#zx readers=%zu "
            "with_region=%zu lookup=%zu inject=%u — from-side route state retired while a "
            "reader was inside the lookup for this region",
            RetireName(retire), static_cast<const void*>(region), region->GetRegionStart(), hits,
            bySite[static_cast<size_t>(Site::ROUTE_WITH_REGION)],
            bySite[static_cast<size_t>(Site::ROUTE_LOOKUP)], static_cast<unsigned>(inject));
    }
    if ((g_retires[r].load(std::memory_order_relaxed) % kSummaryEvery) == 0) {
        DumpSummary();
    }
}

void NoteRetireGlobal(uintptr_t rangeStart, size_t rangeSize, Retire retire)
{
    if (!Enabled()) {
        return;
    }
    EnsureAtexit();
    const size_t r = static_cast<size_t>(retire);
    g_retires[r].fetch_add(1, std::memory_order_relaxed);

    // No owning region here: the arena is shared, so any reader in flight anywhere may hold a
    // liveInfo0 local pointing into the range about to be madvise'd.
    size_t bySite[kSiteCount] = { 0 };
    size_t inflight = ScanSlots(nullptr, bySite);
    if (inflight == 0) {
        return;
    }
    g_globalInflight.fetch_add(inflight, std::memory_order_relaxed);
    size_t n = g_logged.fetch_add(1, std::memory_order_relaxed);
    if (n < kMaxSamples) {
        LOG(RTLOG_ERROR,
            "[GCV2][fwdinflight] hit retire=%s range=[%#zx+%zu) readers=%zu with_region=%zu "
            "lookup=%zu — arena released while readers held liveInfo0 locals",
            RetireName(retire), rangeStart, rangeSize, inflight,
            bySite[static_cast<size_t>(Site::ROUTE_WITH_REGION)],
            bySite[static_cast<size_t>(Site::ROUTE_LOOKUP)]);
    }
}

void DumpSummary()
{
    if (!Enabled()) {
        return;
    }
    size_t retireTotal = 0;
    size_t hitTotal = 0;
    for (size_t i = 0; i < kRetireCount; ++i) {
        retireTotal += g_retires[i].load(std::memory_order_relaxed);
        hitTotal += g_hits[i].load(std::memory_order_relaxed);
    }
    // injected_seen is the control: with inject on it must track the per-region retire count
    // (dispel + take_garbage); with inject off it must be 0. Report both so the reader can
    // check the arm rather than trust it.
    const size_t perRegionRetires = g_retires[static_cast<size_t>(Retire::DISPEL_GHOST)].load(
                                        std::memory_order_relaxed) +
        g_retires[static_cast<size_t>(Retire::TAKE_GARBAGE)].load(std::memory_order_relaxed);
    LOG(RTLOG_ERROR,
        "[GCV2][fwdinflight] SUMMARY inject=%u hits=%zu retires=%zu per_region_retires=%zu "
        "injected_seen=%zu global_inflight=%zu published_with_region=%zu published_lookup=%zu "
        "hits_with_region=%zu hits_lookup=%zu slot_overflow=%zu threads=%zu",
        static_cast<unsigned>(InjectOn()), hitTotal, retireTotal, perRegionRetires,
        g_injectSeen.load(std::memory_order_relaxed), g_globalInflight.load(std::memory_order_relaxed),
        g_published[static_cast<size_t>(Site::ROUTE_WITH_REGION)].load(std::memory_order_relaxed),
        g_published[static_cast<size_t>(Site::ROUTE_LOOKUP)].load(std::memory_order_relaxed),
        g_hitsBySite[static_cast<size_t>(Site::ROUTE_WITH_REGION)].load(std::memory_order_relaxed),
        g_hitsBySite[static_cast<size_t>(Site::ROUTE_LOOKUP)].load(std::memory_order_relaxed),
        g_slotOverflow.load(std::memory_order_relaxed), g_nextThreadIdx.load(std::memory_order_relaxed));
    for (size_t i = 0; i < kRetireCount; ++i) {
        size_t rr = g_retires[i].load(std::memory_order_relaxed);
        size_t hh = g_hits[i].load(std::memory_order_relaxed);
        if (rr == 0 && hh == 0) {
            continue;
        }
        LOG(RTLOG_ERROR, "[GCV2][fwdinflight] retire=%s events=%zu hits=%zu",
            RetireName(static_cast<Retire>(i)), rr, hh);
    }
}

} // namespace FwdInflight
} // namespace MapleRuntime
