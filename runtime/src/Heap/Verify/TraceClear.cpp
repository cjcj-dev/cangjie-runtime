// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/TraceClear.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

// 256 was sized for clears alone.  Adding the CollectRegion decision to the same ring roughly
// tripled the traffic (total went 2235 -> ~6200 in the same workload) and every lookup started
// answering no_in_last_256_clears -- the instrument stopped answering because of what the
// instrument itself added, not because the address was absent.  Sized for the whole run instead.
constexpr size_t kCap = 8192;
constexpr size_t kKindLen = 16;
constexpr unsigned int kUnknownRegionField = static_cast<unsigned int>(-1);

struct Entry {
    MAddress start = 0;
    MAddress end = 0;
    uint64_t ns = 0;
    uint64_t gcStartNs = 0;
    unsigned int phase = 0;
    size_t liveBefore = 0;
    void* region = nullptr;
    unsigned int isGhost = kUnknownRegionField;
    unsigned int regionType = kUnknownRegionField;
    unsigned int routeState = kUnknownRegionField;
    unsigned int collectGen = kUnknownRegionField;
    unsigned int freePath = kUnknownRegionField;
    unsigned int wasYoung = kUnknownRegionField;
    unsigned int routeMarkYoung = kUnknownRegionField;
    unsigned int allocating = kUnknownRegionField;
    unsigned int knownEmpty = kUnknownRegionField;
    unsigned int neverExamined = kUnknownRegionField;
    unsigned int markedThisCycle = kUnknownRegionField;
    unsigned int gcReason = kUnknownRegionField;
    unsigned int liveZero = kUnknownRegionField;
    char kind[kKindLen] = {};
};

std::mutex gMu;
Entry gRing[kCap];
size_t gNext = 0;
size_t gTotal = 0;

struct ClassRow {
    std::atomic<uint32_t> key{ 0 };
    std::atomic<uint64_t> n{ 0 };
};
constexpr size_t kClassCap = 64;
ClassRow gCollLiveClass[kClassCap];
std::atomic<uint64_t> gCollLiveLookup{ 0 };
std::atomic<uint64_t> gCollLiveSat{ 0 };
std::atomic<bool> gClassAtexit{ false };

uint32_t MakeClassKey(const Entry& e)
{
    return (e.collectGen & 1u) | ((e.freePath & 7u) << 1) | ((e.routeState & 7u) << 4) | ((e.knownEmpty & 1u) << 7) |
        ((e.neverExamined & 1u) << 8) | ((e.allocating & 1u) << 9) | ((e.routeMarkYoung & 1u) << 10) |
        ((e.isGhost & 1u) << 11) | ((e.gcReason & 15u) << 12) | ((e.wasYoung & 1u) << 16) |
        ((e.regionType & 15u) << 17) | ((e.markedThisCycle & 1u) << 21) | ((e.liveZero & 1u) << 22);
}

void BumpCollLiveClass(const Entry& e)
{
    gCollLiveLookup.fetch_add(1, std::memory_order_relaxed);
    const uint32_t key = MakeClassKey(e) | 0x80000000u;
    for (size_t i = 0; i < kClassCap; ++i) {
        uint32_t cur = gCollLiveClass[i].key.load(std::memory_order_acquire);
        if (cur == key) {
            gCollLiveClass[i].n.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (cur == 0) {
            uint32_t expected = 0;
            if (gCollLiveClass[i].key.compare_exchange_strong(expected, key, std::memory_order_acq_rel,
                                                              std::memory_order_acquire)) {
                gCollLiveClass[i].n.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (expected == key) {
                gCollLiveClass[i].n.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    gCollLiveSat.fetch_add(1, std::memory_order_relaxed);
}

void DumpCollLiveClass()
{
    std::fprintf(stderr, "[WHODEAD][class] lookup=%lu sat=%lu\n",
                 static_cast<unsigned long>(gCollLiveLookup.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(gCollLiveSat.load(std::memory_order_relaxed)));
    for (size_t i = 0; i < kClassCap; ++i) {
        const uint32_t key = gCollLiveClass[i].key.load(std::memory_order_relaxed);
        const uint64_t n = gCollLiveClass[i].n.load(std::memory_order_relaxed);
        if (key == 0 || n == 0) {
            continue;
        }
        std::fprintf(stderr,
                     "[WHODEAD][class] n=%lu cgen=%u fpath=%u route=%u ke=%u nv=%u alloc=%u rmy=%u gh=%u "
                     "reason=%u y=%u rtype=%u mk=%u lz=%u\n",
                     static_cast<unsigned long>(n), key & 1u, (key >> 1) & 7u, (key >> 4) & 7u, (key >> 7) & 1u,
                     (key >> 8) & 1u, (key >> 9) & 1u, (key >> 10) & 1u, (key >> 11) & 1u, (key >> 12) & 15u,
                     (key >> 16) & 1u, (key >> 17) & 15u, (key >> 21) & 1u, (key >> 22) & 1u);
    }
    std::fflush(stderr);
}

void EnsureClassAtexit()
{
    bool expected = false;
    if (gClassAtexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit(DumpCollLiveClass);
    }
}

void RecordEntry(const Entry& entry)
{
    std::lock_guard<std::mutex> lock(gMu);
    gRing[gNext % kCap] = entry;
    ++gNext;
    ++gTotal;
}

void FillRegionClass(Entry& e, void* region, unsigned collectGen, unsigned freePath)
{
    e.collectGen = collectGen;
    e.freePath = freePath;
    e.liveZero = e.liveBefore == 0 ? 1u : 0u;
    e.gcReason = static_cast<unsigned>(Heap::GetHeap().GetCollector().GetGCStats().reason);
    if (region == nullptr) {
        return;
    }
    RegionInfo* r = static_cast<RegionInfo*>(region);
    e.regionType = static_cast<unsigned>(r->GetRegionType());
    e.routeState = static_cast<unsigned>(r->GetRouteState());
    e.isGhost = r->IsGhostFromRegion() ? 1u : 0u;
    e.wasYoung = r->IsYoungRegion() ? 1u : 0u;
    e.routeMarkYoung = r->GetRouteMarkGeneration() == Generation::Young ? 1u : 0u;
    e.allocating = r->HasMarkStartAllocGap() ? 1u : 0u;
    e.knownEmpty = r->IsRouteKnownEmpty() ? 1u : 0u;
    LiveInfo* live = r->GetLiveInfo();
    LiveInfo* ghost = r->GetLiveInfo0ForProbe();
    RegionBitmap* mb = r->GetRouteMarkBitmap(ghost != nullptr ? ghost : live);
    e.markedThisCycle = mb != nullptr ? 1u : 0u;
    e.neverExamined =
        (e.knownEmpty && mb == nullptr && r->GetRegionAllocPtr() > r->GetRegionStart()) ? 1u : 0u;
}

} // namespace

// gcfwdfix was built for exactly the question now in hand and then pinned off, so the ring, the
// address lookup and the per-cycle lookup all already exist -- this only turns the gate back on.
//
// What it answers: the read barrier hands out targets whose header is zero and which have no
// to-version to resolve to.  Asking the region what it is at that moment does not work -- region
// type is a moving property and two samples of it gave contradictory answers (THREAD_LOCAL /
// RECENT_FULL in one run, FREE past allocPtr in the next).  The ring records the clear *when it
// happens*, so a hit is direct evidence of who zeroed that address rather than a guess from the
// state it is in afterwards.
//
// Why this matters for the ZGC comparison: ZGC has no address quarantine either.  ZPageAllocator
// ::free_page runs prepare_memory_for_free -> safe_destroy_page -> free_memory, and ZSafeDelete
// only defers deleting the ZPage *metadata object* (zSafeDelete.inline.hpp schedule_delete), not
// the heap memory.  So ZGC's protection against a stale pointer naming reused memory is not
// keeping the address away -- it is that no reachable slot still holds a stale pointer.  Ours is
// being dereferenced by a mutator, so it is reachable, so it should have been marked.
constexpr bool kTraceClearOn = true;

bool TraceClear::Enabled() { return kTraceClearOn; }

void TraceClear::NoteRange(MAddress start, size_t size, const char* kind, void* region, size_t liveBefore)
{
    NoteRange(start, size, kind, region, liveBefore, kUnknownRegionField, kUnknownRegionField);
}

void TraceClear::NoteRange(MAddress start, size_t size, const char* kind, void* region, size_t liveBefore,
                           unsigned collectGen, unsigned freePath)
{
    if (!Enabled() || size == 0) {
        return;
    }
    EnsureClassAtexit();
    Entry e;
    e.start = start;
    e.end = start + size;
    e.ns = TimeUtil::NanoSeconds();
    e.gcStartNs = GCStats::GetPrevGCStartTime();
    e.phase = static_cast<unsigned int>(Heap::GetHeap().GetGCPhase());
    e.liveBefore = liveBefore;
    e.region = region;
    FillRegionClass(e, region, collectGen, freePath);
    if (kind != nullptr) {
        std::strncpy(e.kind, kind, kKindLen - 1);
        e.kind[kKindLen - 1] = '\0';
    }
    RecordEntry(e);
    VLOG(REPORT,
         "[GCV2][trace-clear] kind=%s range=[%#zx,%#zx) size=%zu region=%p liveBefore=%zu phase=%u "
         "cgen=%u fpath=%u route=%u ke=%u nv=%u alloc=%u rmy=%u gh=%u reason=%u y=%u rtype=%u mk=%u "
         "gcStartNs=%llu total=%zu env=MRT_GCV2_TRACE_CLEAR=1|MRT_GCV2_F3_REGION=1",
         e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), size, region, liveBefore, e.phase,
         e.collectGen, e.freePath, e.routeState, e.knownEmpty, e.neverExamined, e.allocating, e.routeMarkYoung,
         e.isGhost, e.gcReason, e.wasYoung, e.regionType, e.markedThisCycle,
         static_cast<unsigned long long>(e.gcStartNs), gTotal);
}

void TraceClear::NoteRegionEvent(MAddress start, size_t size, const char* kind, void* region, size_t liveBefore,
                                 unsigned int isGhost, unsigned int regionType, unsigned int routeState)
{
    static const bool f3Region = false /* pinned:MRT_GCV2_F3_REGION */;
    if (!f3Region || size == 0) {
        return;
    }
    const unsigned int phase = static_cast<unsigned int>(Heap::GetHeap().GetGCPhase());
    if (phase != static_cast<unsigned int>(GCPhase::GC_PHASE_PREFORWARD) &&
        phase != static_cast<unsigned int>(GCPhase::GC_PHASE_FORWARD)) {
        return;
    }
    Entry e;
    e.start = start;
    e.end = start + size;
    e.ns = TimeUtil::NanoSeconds();
    e.gcStartNs = GCStats::GetPrevGCStartTime();
    e.phase = phase;
    e.liveBefore = liveBefore;
    e.region = region;
    e.isGhost = isGhost;
    e.regionType = regionType;
    e.routeState = routeState;
    if (kind != nullptr) {
        std::strncpy(e.kind, kind, kKindLen - 1);
        e.kind[kKindLen - 1] = '\0';
    }
    RecordEntry(e);
    VLOG(REPORT,
         "[GCV2][F3_REGION][supply-event] kind=%s region=%p range=[%#zx,%#zx) ghost=%u type=%u route=%u "
         "liveBefore=%zu phase=%u gcStartNs=%llu total=%zu",
         e.kind, region, static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.isGhost, e.regionType,
         e.routeState, e.liveBefore, e.phase, static_cast<unsigned long long>(e.gcStartNs), gTotal);
}

bool TraceClear::Lookup(MAddress addr, char* buf, size_t bufSize)
{
    if (buf == nullptr || bufSize == 0) {
        return false;
    }
    buf[0] = '\0';
    if (!Enabled()) {
        std::snprintf(buf, bufSize, "trace_clear_off");
        return false;
    }
    std::lock_guard<std::mutex> lock(gMu);
    size_t n = gNext < kCap ? gNext : kCap;
    size_t startIdx = gNext < kCap ? 0 : (gNext % kCap);
    // newest first
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (gNext + kCap - 1 - i) % kCap;
        (void)startIdx;
        const Entry& e = gRing[idx];
        if (e.end <= e.start) {
            continue;
        }
        if (addr >= e.start && addr < e.end) {
            if (std::strcmp(e.kind, "coll_live") == 0) {
                BumpCollLiveClass(e);
            }
            std::snprintf(buf, bufSize,
                          "yes kind=%s range=[%#zx,%#zx) region=%p liveBefore=%zu ageNs=%llu phase=%u "
                          "cgen=%u fpath=%u route=%u ke=%u nv=%u alloc=%u rmy=%u gh=%u reason=%u y=%u "
                          "rtype=%u mk=%u lz=%u gcStartNs=%llu total=%zu",
                          e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.region, e.liveBefore,
                          static_cast<unsigned long long>(TimeUtil::NanoSeconds() - e.ns), e.phase, e.collectGen,
                          e.freePath, e.routeState, e.knownEmpty, e.neverExamined, e.allocating, e.routeMarkYoung,
                          e.isGhost, e.gcReason, e.wasYoung, e.regionType, e.markedThisCycle, e.liveZero,
                          static_cast<unsigned long long>(e.gcStartNs), gTotal);
            return true;
        }
    }
    std::snprintf(buf, bufSize, "no_in_last_%zu_clears total=%zu", n, gTotal);
    return false;
}

bool TraceClear::LookupKind(MAddress addr, const char* kind, uint64_t gcStartNs, char* buf, size_t bufSize)
{
    if (buf == nullptr || bufSize == 0) {
        return false;
    }
    buf[0] = '\0';
    if (!Enabled() || kind == nullptr) {
        std::snprintf(buf, bufSize, "trace_region_event_off");
        return false;
    }
    std::lock_guard<std::mutex> lock(gMu);
    size_t n = gNext < kCap ? gNext : kCap;
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (gNext + kCap - 1 - i) % kCap;
        const Entry& e = gRing[idx];
        if (e.end <= e.start || std::strcmp(e.kind, kind) != 0 || e.gcStartNs != gcStartNs) {
            continue;
        }
        if (addr >= e.start && addr < e.end) {
            std::snprintf(buf, bufSize,
                          "yes kind=%s range=[%#zx,%#zx) region=%p ghost=%u type=%u route=%u liveBefore=%zu "
                          "ageNs=%llu phase=%u gcStartNs=%llu total=%zu",
                          e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.region, e.isGhost,
                          e.regionType, e.routeState, e.liveBefore,
                          static_cast<unsigned long long>(TimeUtil::NanoSeconds() - e.ns), e.phase,
                          static_cast<unsigned long long>(e.gcStartNs), gTotal);
            return true;
        }
    }
    std::snprintf(buf, bufSize, "no kind=%s gcStartNs=%llu in_last_%zu total=%zu", kind,
                  static_cast<unsigned long long>(gcStartNs), n, gTotal);
    return false;
}

void TraceClear::DumpRecent(size_t n)
{
    if (!Enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(gMu);
    size_t avail = gNext < kCap ? gNext : kCap;
    size_t show = n < avail ? n : avail;
    for (size_t i = 0; i < show; ++i) {
        size_t idx = (gNext + kCap - 1 - i) % kCap;
        const Entry& e = gRing[idx];
        VLOG(REPORT, "[GCV2][trace-clear][dump] #%zu kind=%s [%#zx,%#zx) region=%p liveBefore=%zu", i, e.kind,
             static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.region, e.liveBefore);
    }
}

bool TraceClear::SkipCompactMemset()
{
    // staleclr arm C (diagnostic, default off; live env read for the experiment).
    // If invalid_object_active_region collapses with the compact free-tail memset
    // skipped, the zeroed tips named by stale old-colour slots are compact-memset
    // products — proving the targets were not-survived at compact, not freed raw.
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_SKIP_COMPACT_MEMSET");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

} // namespace MapleRuntime
