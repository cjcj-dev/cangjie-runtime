// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/FwdInflight.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

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

size_t EnvSize(const char* name, size_t fallback)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v || *end != '\0') {
        return fallback;
    }
    return static_cast<size_t>(parsed);
}

// Negative arm for the arena drain. A retire edge can only be seen to wait for a reader if a
// reader is still there when it runs, and on a well-behaved run there may be none -- in which
// case "hits=0" says nothing about the drain. This widens the publication window by holding it
// open on a sampled fraction of lookups, so the arm that is supposed to report non-zero can be
// made to report non-zero. Sampled, not blanket: RouteObject runs millions of times per cycle
// and a blanket delay would change the schedule being measured rather than lengthen one window
// inside it.
//   MRT_GCV2_FWDINFLIGHT_HOLD_US=N     microseconds to hold (0 = off, the default)
//   MRT_GCV2_FWDINFLIGHT_HOLD_EVERY=M  hold on every Mth armed scope (default 1)
size_t HoldUs()
{
    static const size_t v = EnvSize("MRT_GCV2_FWDINFLIGHT_HOLD_US", 0);
    return v;
}

size_t HoldEvery()
{
    static const size_t v = EnvSize("MRT_GCV2_FWDINFLIGHT_HOLD_EVERY", 1);
    return v;
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
std::atomic<size_t> g_holdSeq{ 0 };
std::atomic<size_t> g_holds{ 0 };

// A scan can only ever find a reader on a DIFFERENT thread: a thread inside RouteObject is
// not simultaneously executing a retire edge. So "hits=0" has two readings that the headline
// number cannot tell apart -- the readers and the retires genuinely never overlapped in time,
// or they were always the same thread and could not have overlapped by construction. These
// two histograms separate them: if the thread slots that publish and the thread slots that
// retire are the same single slot, hits=0 is arithmetic, not evidence about the schedule.
constexpr size_t kRosterSlots = 8;
std::atomic<size_t> g_pubByThread[kRosterSlots];
std::atomic<size_t> g_retireByThread[kRosterSlots];
// Which thread actually got held. The hold sampler counts lookups globally, so a run can
// spend all its holds on the retiring thread -- where a hold can never produce a hit -- and
// look like a negative arm that fired when it did not.
std::atomic<size_t> g_holdByThread[kRosterSlots];

void NoteThread(std::atomic<size_t>* histogram, size_t idx)
{
    if (idx < kRosterSlots) {
        histogram[idx].fetch_add(1, std::memory_order_relaxed);
    }
}
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
// Count published readers in [first, last). `region == nullptr` means "count every reader".
size_t ScanRange(size_t first, size_t last, const void* region, size_t* bySite)
{
    size_t hits = 0;
    for (size_t i = first; i < last; ++i) {
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

size_t ScanSlots(const void* region, size_t* bySite)
{
    // Only slots below the thread high-water mark can ever have been published, and the
    // control's slots mirror them at kMaxThreads + idx. Scanning the full 2 * kMaxThreads on
    // every retire edge would put millions of loads per GC on an instrument whose whole point
    // is to observe the schedule rather than perturb it.
    const size_t threads = g_nextThreadIdx.load(std::memory_order_relaxed);
    const size_t highWater = (threads < kMaxThreads) ? threads : kMaxThreads;
    return ScanRange(0, highWater, region, bySite) +
        ScanRange(kMaxThreads, kMaxThreads + highWater, region, bySite);
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
    // A nested lookup on the same thread must not take the slot from its caller: the inner
    // scope's destructor would clear it and the caller's remaining window would go
    // unpublished, which under-reports in exactly the direction that would make the defect
    // look smaller. The outermost scope owns the slot, and its window covers the inner one.
    if (g_slots[idx].region.load(std::memory_order_relaxed) != nullptr) {
        return;
    }
    EnsureAtexit();
    NoteThread(g_pubByThread, idx);
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
    // Hold before un-publishing: the thread is still inside RouteObject with its liveInfo0
    // local live, so this lengthens the real exposure, not just the bookkeeping.
    const size_t holdUs = HoldUs();
    if (holdUs != 0) {
        const size_t every = HoldEvery();
        if (every <= 1 || (g_holdSeq.fetch_add(1, std::memory_order_relaxed) % every) == 0) {
            g_holds.fetch_add(1, std::memory_order_relaxed);
            NoteThread(g_holdByThread, ThreadSlot());
            std::this_thread::sleep_for(std::chrono::microseconds(holdUs));
        }
    }
    g_slots[ThreadSlot()].region.store(nullptr, std::memory_order_release);
}

void NoteRetireRegion(const RegionInfo* region, Retire retire)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    EnsureAtexit();
    NoteThread(g_retireByThread, ThreadSlot());
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
    NoteThread(g_retireByThread, ThreadSlot());
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
        "injected_seen=%zu global_inflight=%zu holds=%zu hold_us=%zu hold_every=%zu "
        "published_with_region=%zu published_lookup=%zu "
        "hits_with_region=%zu hits_lookup=%zu slot_overflow=%zu threads=%zu",
        static_cast<unsigned>(InjectOn()), hitTotal, retireTotal, perRegionRetires,
        g_injectSeen.load(std::memory_order_relaxed), g_globalInflight.load(std::memory_order_relaxed),
        g_holds.load(std::memory_order_relaxed), HoldUs(), HoldEvery(),
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
    for (size_t i = 0; i < kRosterSlots; ++i) {
        size_t pub = g_pubByThread[i].load(std::memory_order_relaxed);
        size_t ret = g_retireByThread[i].load(std::memory_order_relaxed);
        size_t held = g_holdByThread[i].load(std::memory_order_relaxed);
        if (pub == 0 && ret == 0 && held == 0) {
            continue;
        }
        LOG(RTLOG_ERROR, "[GCV2][fwdinflight] thread_slot=%zu published=%zu retired=%zu held=%zu", i,
            pub, ret, held);
    }
}

} // namespace FwdInflight
} // namespace MapleRuntime
