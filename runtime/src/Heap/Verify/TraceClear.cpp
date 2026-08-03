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

namespace MapleRuntime {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

constexpr size_t kCap = 256;
constexpr size_t kKindLen = 16;

struct Entry {
    MAddress start = 0;
    MAddress end = 0;
    uint64_t ns = 0;
    size_t liveBefore = 0;
    void* region = nullptr;
    char kind[kKindLen] = {};
};

std::mutex gMu;
Entry gRing[kCap];
size_t gNext = 0;
size_t gTotal = 0;

} // namespace

bool TraceClear::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_TRACE_CLEAR");
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
    e.liveBefore = liveBefore;
    e.region = region;
    if (kind != nullptr) {
        std::strncpy(e.kind, kind, kKindLen - 1);
        e.kind[kKindLen - 1] = '\0';
    }
    {
        std::lock_guard<std::mutex> lock(gMu);
        gRing[gNext % kCap] = e;
        ++gNext;
        ++gTotal;
    }
    VLOG(REPORT,
         "[GCV2][trace-clear] kind=%s range=[%#zx,%#zx) size=%zu region=%p liveBefore=%zu total=%zu "
         "env=MRT_GCV2_TRACE_CLEAR=1",
         e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), size, region, liveBefore, gTotal);
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
                          "yes kind=%s range=[%#zx,%#zx) region=%p liveBefore=%zu ageNs=%llu total=%zu",
                          e.kind, static_cast<size_t>(e.start), static_cast<size_t>(e.end), e.region, e.liveBefore,
                          static_cast<unsigned long long>(TimeUtil::NanoSeconds() - e.ns), gTotal);
            return true;
        }
    }
    std::snprintf(buf, bufSize, "no_in_last_%zu_clears total=%zu", n, gTotal);
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
