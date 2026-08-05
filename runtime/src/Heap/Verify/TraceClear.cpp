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

constexpr size_t kCap = 256;
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
    char kind[kKindLen] = {};
};

std::mutex gMu;
Entry gRing[kCap];
size_t gNext = 0;
size_t gTotal = 0;

void RecordEntry(const Entry& entry)
{
    std::lock_guard<std::mutex> lock(gMu);
    gRing[gNext % kCap] = entry;
    ++gNext;
    ++gTotal;
}

} // namespace

bool TraceClear::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_TRACE_CLEAR") || EnvIsOne("MRT_GCV2_F3_REGION") ||
                           EnvIsOne("MRT_GCV2_PLAINEDGE");
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
    if (kind != nullptr) {
        std::strncpy(e.kind, kind, kKindLen - 1);
        e.kind[kKindLen - 1] = '\0';
    }
    RecordEntry(e);
    VLOG(REPORT,
         "[GCV2][trace-clear] kind=%s range=[%#zx,%#zx) size=%zu region=%p liveBefore=%zu phase=%u "
         "gcStartNs=%llu total=%zu env=MRT_GCV2_TRACE_CLEAR=1|MRT_GCV2_F3_REGION=1",
         e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), size, region, liveBefore, e.phase,
         static_cast<unsigned long long>(e.gcStartNs), gTotal);
}

void TraceClear::NoteRegionEvent(MAddress start, size_t size, const char* kind, void* region, size_t liveBefore,
                                 unsigned int isGhost, unsigned int regionType, unsigned int routeState)
{
    static const bool f3Region = EnvIsOne("MRT_GCV2_F3_REGION") || EnvIsOne("MRT_GCV2_PLAINEDGE");
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
            std::snprintf(buf, bufSize,
                          "yes kind=%s range=[%#zx,%#zx) region=%p liveBefore=%zu ageNs=%llu phase=%u "
                          "gcStartNs=%llu total=%zu",
                          e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.region, e.liveBefore,
                          static_cast<unsigned long long>(TimeUtil::NanoSeconds() - e.ns), e.phase,
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
    static const bool on = EnvIsOne("MRT_GCV2_SKIP_COMPACT_MEMSET");
    return on;
}

} // namespace MapleRuntime
