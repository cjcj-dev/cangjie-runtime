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
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

// F3_DEATH needs a deeper ring: minor waves clear many regions before abort.
constexpr size_t kCapDefault = 256;
constexpr size_t kCapDeath = 4096;
constexpr size_t kKindLen = 24;
constexpr unsigned int kUnknownRegionField = static_cast<unsigned int>(-1);

struct Entry {
    MAddress start = 0;
    MAddress end = 0;
    uint64_t ns = 0;
    uint64_t gcStartNs = 0;
    unsigned int phase = 0;
    unsigned int gcKind = 0; // 0 unknown, 1 minor, 2 major
    size_t gcIndex = 0;
    size_t liveBefore = 0;
    void* region = nullptr;
    unsigned int isGhost = kUnknownRegionField;
    unsigned int regionType = kUnknownRegionField;
    unsigned int routeState = kUnknownRegionField;
    char kind[kKindLen] = {};
};

std::mutex gMu;
Entry* gRing = nullptr;
size_t gCap = kCapDefault;
size_t gNext = 0;
size_t gTotal = 0;
size_t gWrapCount = 0;
bool gInited = false;

void EnsureRing()
{
    if (gInited) {
        return;
    }
    gCap = EnvIsOne("MRT_GCV2_F3_DEATH") ? kCapDeath : kCapDefault;
    gRing = new Entry[gCap];
    gInited = true;
}

void RecordEntry(const Entry& entry)
{
    std::lock_guard<std::mutex> lock(gMu);
    EnsureRing();
    if (gTotal >= gCap && (gNext % gCap) == 0 && gTotal > 0) {
        ++gWrapCount;
    }
    gRing[gNext % gCap] = entry;
    ++gNext;
    ++gTotal;
}

bool IsKillKind(const char* kind)
{
    return kind != nullptr &&
        (std::strcmp(kind, "collect_region") == 0 || std::strcmp(kind, "clear_units") == 0 ||
         std::strcmp(kind, "garbage_reuse") == 0 || std::strcmp(kind, "dirty_take") == 0 ||
         std::strcmp(kind, "compact") == 0 || std::strcmp(kind, "compact_partial") == 0 ||
         std::strcmp(kind, "ghost_reclaim") == 0);
}

const char* GcKindName(unsigned int k)
{
    if (k == 1) {
        return "minor";
    }
    if (k == 2) {
        return "major";
    }
    return "unknown";
}

} // namespace

bool TraceClear::DeathEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_F3_DEATH");
    return on;
}

bool TraceClear::Enabled()
{
    static const bool on =
        EnvIsOne("MRT_GCV2_TRACE_CLEAR") || EnvIsOne("MRT_GCV2_F3_REGION") || EnvIsOne("MRT_GCV2_F3_DEATH");
    return on;
}

void TraceClear::NoteRange(MAddress start, size_t size, const char* kind, void* region, size_t liveBefore)
{
    if (!Enabled() || size == 0) {
        return;
    }
    Entry e;
    e.start = start;
    e.end = start + size;
    e.ns = TimeUtil::NanoSeconds();
    e.gcStartNs = GCStats::GetPrevGCStartTime();
    e.phase = static_cast<unsigned int>(Heap::GetHeap().GetGCPhase());
    e.liveBefore = liveBefore;
    e.region = region;
    if (DeathEnabled()) {
        GCReason reason = Heap::GetHeap().GetCollector().GetGCStats().reason;
        e.gcKind = (reason == GC_REASON_YOUNG) ? 1U : 2U;
        e.gcIndex = g_gcCount;
    }
    if (kind != nullptr) {
        std::strncpy(e.kind, kind, kKindLen - 1);
        e.kind[kKindLen - 1] = '\0';
    }
    RecordEntry(e);
    VLOG(REPORT,
         "[GCV2][trace-clear] kind=%s range=[%#zx,%#zx) size=%zu region=%p liveBefore=%zu phase=%u "
         "gcKind=%s gcIndex=%zu gcStartNs=%llu total=%zu env=MRT_GCV2_TRACE_CLEAR|F3_REGION|F3_DEATH",
         e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), size, region, liveBefore, e.phase,
         GcKindName(e.gcKind), e.gcIndex, static_cast<unsigned long long>(e.gcStartNs), gTotal);
}

void TraceClear::NoteRegionEvent(MAddress start, size_t size, const char* kind, void* region, size_t liveBefore,
                                 unsigned int isGhost, unsigned int regionType, unsigned int routeState,
                                 unsigned int gcKind, size_t gcIndex)
{
    // F3_REGION path only during PREFORWARD/FORWARD for supply events;
    // F3_DEATH records all lifecycle events regardless of phase.
    const bool f3Region = EnvIsOne("MRT_GCV2_F3_REGION");
    const bool f3Death = DeathEnabled();
    if ((!f3Region && !f3Death) || size == 0) {
        return;
    }
    const unsigned int phase = static_cast<unsigned int>(Heap::GetHeap().GetGCPhase());
    if (f3Region && !f3Death) {
        if (phase != static_cast<unsigned int>(GCPhase::GC_PHASE_PREFORWARD) &&
            phase != static_cast<unsigned int>(GCPhase::GC_PHASE_FORWARD)) {
            return;
        }
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
    if (f3Death) {
        if (gcKind == 0) {
            GCReason reason = Heap::GetHeap().GetCollector().GetGCStats().reason;
            e.gcKind = (reason == GC_REASON_YOUNG) ? 1U : 2U;
        } else {
            e.gcKind = gcKind;
        }
        e.gcIndex = (gcIndex != 0) ? gcIndex : g_gcCount;
    }
    if (kind != nullptr) {
        std::strncpy(e.kind, kind, kKindLen - 1);
        e.kind[kKindLen - 1] = '\0';
    }
    RecordEntry(e);
    VLOG(REPORT,
         "[GCV2][F3_DEATH][region-event] kind=%s region=%p range=[%#zx,%#zx) ghost=%u type=%u route=%u "
         "liveBefore=%zu phase=%u gcKind=%s gcIndex=%zu gcStartNs=%llu total=%zu",
         e.kind, region, static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.isGhost, e.regionType,
         e.routeState, e.liveBefore, e.phase, GcKindName(e.gcKind), e.gcIndex,
         static_cast<unsigned long long>(e.gcStartNs), gTotal);
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
    EnsureRing();
    size_t n = gNext < gCap ? gNext : gCap;
    // newest first
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (gNext + gCap - 1 - i) % gCap;
        const Entry& e = gRing[idx];
        if (e.end <= e.start) {
            continue;
        }
        if (addr >= e.start && addr < e.end) {
            std::snprintf(buf, bufSize,
                          "yes kind=%s range=[%#zx,%#zx) region=%p liveBefore=%zu ageNs=%llu phase=%u "
                          "gcKind=%s gcIndex=%zu gcStartNs=%llu total=%zu",
                          e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.region, e.liveBefore,
                          static_cast<unsigned long long>(TimeUtil::NanoSeconds() - e.ns), e.phase,
                          GcKindName(e.gcKind), e.gcIndex, static_cast<unsigned long long>(e.gcStartNs), gTotal);
            return true;
        }
    }
    std::snprintf(buf, bufSize, "no_in_last_%zu_clears total=%zu wrap=%zu", n, gTotal, gWrapCount);
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
    EnsureRing();
    size_t n = gNext < gCap ? gNext : gCap;
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (gNext + gCap - 1 - i) % gCap;
        const Entry& e = gRing[idx];
        if (e.end <= e.start || std::strcmp(e.kind, kind) != 0 || e.gcStartNs != gcStartNs) {
            continue;
        }
        if (addr >= e.start && addr < e.end) {
            std::snprintf(buf, bufSize,
                          "yes kind=%s range=[%#zx,%#zx) region=%p ghost=%u type=%u route=%u liveBefore=%zu "
                          "ageNs=%llu phase=%u gcKind=%s gcIndex=%zu gcStartNs=%llu total=%zu",
                          e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.region, e.isGhost,
                          e.regionType, e.routeState, e.liveBefore,
                          static_cast<unsigned long long>(TimeUtil::NanoSeconds() - e.ns), e.phase,
                          GcKindName(e.gcKind), e.gcIndex, static_cast<unsigned long long>(e.gcStartNs), gTotal);
            return true;
        }
    }
    std::snprintf(buf, bufSize, "no kind=%s gcStartNs=%llu in_last_%zu total=%zu", kind,
                  static_cast<unsigned long long>(gcStartNs), n, gTotal);
    return false;
}

size_t TraceClear::DumpHistory(MAddress addr, char* killBuf, size_t killBufSize)
{
    if (killBuf != nullptr && killBufSize > 0) {
        killBuf[0] = '\0';
    }
    if (!Enabled()) {
        if (killBuf != nullptr && killBufSize > 0) {
            std::snprintf(killBuf, killBufSize, "trace_clear_off");
        }
        return 0;
    }
    std::lock_guard<std::mutex> lock(gMu);
    EnsureRing();
    size_t n = gNext < gCap ? gNext : gCap;
    size_t hits = 0;
    bool killFilled = false;
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (gNext + gCap - 1 - i) % gCap;
        const Entry& e = gRing[idx];
        if (e.end <= e.start || addr < e.start || addr >= e.end) {
            continue;
        }
        ++hits;
        VLOG(REPORT,
             "[GCV2][F3_DEATH][history] #%zu kind=%s range=[%#zx,%#zx) region=%p ghost=%u type=%u route=%u "
             "liveBefore=%zu phase=%u gcKind=%s gcIndex=%zu ageNs=%llu gcStartNs=%llu",
             hits, e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.region, e.isGhost,
             e.regionType, e.routeState, e.liveBefore, e.phase, GcKindName(e.gcKind), e.gcIndex,
             static_cast<unsigned long long>(TimeUtil::NanoSeconds() - e.ns),
             static_cast<unsigned long long>(e.gcStartNs));
        if (!killFilled && IsKillKind(e.kind) && killBuf != nullptr && killBufSize > 0) {
            std::snprintf(killBuf, killBufSize,
                          "kind=%s gcKind=%s gcIndex=%zu phase=%u liveBefore=%zu ageNs=%llu "
                          "range=[%#zx,%#zx) region=%p type=%u route=%u gcStartNs=%llu",
                          e.kind, GcKindName(e.gcKind), e.gcIndex, e.phase, e.liveBefore,
                          static_cast<unsigned long long>(TimeUtil::NanoSeconds() - e.ns),
                          static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.region, e.regionType,
                          e.routeState, static_cast<unsigned long long>(e.gcStartNs));
            killFilled = true;
        }
    }
    if (!killFilled && killBuf != nullptr && killBufSize > 0) {
        std::snprintf(killBuf, killBufSize, "no_kill_event hits=%zu total=%zu wrap=%zu cap=%zu", hits, gTotal,
                      gWrapCount, gCap);
    }
    VLOG(REPORT, "[GCV2][F3_DEATH][history-summary] addr=%#zx hits=%zu total=%zu wrap=%zu cap=%zu killFilled=%u",
         static_cast<size_t>(addr), hits, gTotal, gWrapCount, gCap, static_cast<unsigned>(killFilled));
    return hits;
}

void TraceClear::RingStats(size_t& capacity, size_t& total, size_t& wrapCount)
{
    std::lock_guard<std::mutex> lock(gMu);
    EnsureRing();
    capacity = gCap;
    total = gTotal;
    wrapCount = gWrapCount;
}

void TraceClear::DumpRecent(size_t n)
{
    if (!Enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(gMu);
    EnsureRing();
    size_t avail = gNext < gCap ? gNext : gCap;
    size_t show = n < avail ? n : avail;
    for (size_t i = 0; i < show; ++i) {
        size_t idx = (gNext + gCap - 1 - i) % gCap;
        const Entry& e = gRing[idx];
        VLOG(REPORT, "[GCV2][trace-clear][dump] #%zu kind=%s [%#zx,%#zx) region=%p liveBefore=%zu gcKind=%s", i,
             e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.region, e.liveBefore,
             GcKindName(e.gcKind));
    }
}

bool TraceClear::SkipCompactMemset()
{
    static const bool on = EnvIsOne("MRT_GCV2_SKIP_COMPACT_MEMSET");
    return on;
}

} // namespace MapleRuntime
