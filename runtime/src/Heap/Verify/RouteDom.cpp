// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/RouteDom.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace RouteDom {
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
    static const bool on = EnvIsOne("MRT_GCV2_VERIFY_ROUTEDOM");
    return on;
}

bool InvertOn()
{
    static const bool on = EnvIsOne("MRT_GCV2_VERIFY_ROUTEDOM_INVERT");
    return on;
}

size_t MaxSamples()
{
    static const size_t n = EnvSizeT("MRT_GCV2_VERIFY_ROUTEDOM_MAX", 64);
    return n;
}

std::atomic<size_t> g_total{ 0 };
std::atomic<size_t> g_start{ 0 };
std::atomic<size_t> g_interior{ 0 };
std::atomic<size_t> g_unmarked{ 0 };
std::atomic<size_t> g_alias{ 0 };
std::atomic<size_t> g_logged{ 0 };
std::atomic<size_t> g_loggedInterior{ 0 };
std::atomic<bool> g_atexit{ false };

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { DumpSummary(); });
    }
}

struct Covering {
    bool found = false;
    bool isStart = false;
    BaseObject* host = nullptr;
    size_t hostOffset = 0;
    size_t hostSize = 0;
    size_t delta = 0;
    TypeInfo* hostTip = nullptr;
    uintptr_t hostToAddr = 0;
    uintptr_t nextMarkedTo = 0;
};

// Size-walk ghost region for the marked object covering `targetOffset`.
// START when covering.start == target; INTERIOR when covering.start < target < start+size.
Covering FindCovering(RegionInfo* region, size_t targetOffset)
{
    Covering c;
    if (region == nullptr) {
        return c;
    }
    MAddress regionStart = region->GetRegionStart();
    size_t ghostSz = region->GetGhostRegionSize();
    MAddress regionEnd = ghostSz > 0 ? regionStart + ghostSz : region->GetRegionEnd();
    MAddress cursor = regionStart;
    size_t guard = 0;
    constexpr size_t kMaxWalk = 8192;
    BaseObject* prevMarked = nullptr;
    size_t prevMarkedOff = 0;
    size_t prevMarkedSize = 0;
    TypeInfo* prevTip = nullptr;
    uintptr_t prevTo = 0;

    while (cursor < regionEnd && guard < kMaxWalk) {
        ++guard;
        BaseObject* cand = reinterpret_cast<BaseObject*>(cursor);
        size_t candOff = static_cast<size_t>(cursor - regionStart);
        if (candOff > targetOffset && prevMarked != nullptr) {
            // Past target without containing it in prevMarked → no cover.
            break;
        }
        if (!Heap::IsHeapAddress(cursor) || !cand->IsValidObject()) {
            break;
        }
        TypeInfo* ti = cand->GetTypeInfo();
        if (ti == nullptr || !ti->IsVaildType()) {
            break;
        }
        size_t candSize = RegionSpace::GetAllocSize(*cand);
        if (candSize == 0) {
            break;
        }
        bool marked = region->IsMarkedObject(cand);
        // Also accept ghost-survived at object start (resurrect path).
        if (!marked) {
            LiveInfo* g = region->GetLiveInfo0ForProbe();
            if (g != nullptr && g->IsSurvivedObject(candOff)) {
                marked = true;
            }
        }
        if (marked) {
            uint64_t pre = region->GetPreLiveBytesInGhostRegion(cursor);
            uintptr_t to = static_cast<uintptr_t>(region->GetRoutePlanAddr(pre));
            if (candOff == targetOffset) {
                c.found = true;
                c.isStart = true;
                c.host = cand;
                c.hostOffset = candOff;
                c.hostSize = candSize;
                c.delta = 0;
                c.hostTip = ti;
                c.hostToAddr = to;
                return c;
            }
            if (candOff < targetOffset && targetOffset < candOff + candSize) {
                c.found = true;
                c.isStart = false;
                c.host = cand;
                c.hostOffset = candOff;
                c.hostSize = candSize;
                c.delta = targetOffset - candOff;
                c.hostTip = ti;
                c.hostToAddr = to;
                return c;
            }
            // Track for nextMarkedTo when target itself is a start we already returned.
            if (candOff > targetOffset && prevMarked != nullptr) {
                c.nextMarkedTo = to;
            }
            prevMarked = cand;
            prevMarkedOff = candOff;
            prevMarkedSize = candSize;
            prevTip = ti;
            prevTo = to;
            (void)prevMarkedOff;
            (void)prevMarkedSize;
            (void)prevTip;
            (void)prevTo;
        }
        cursor += candSize;
    }
    return c;
}

// Next marked object after fromOffset (for alias criterion on unmarked path).
uintptr_t NextMarkedToAddr(RegionInfo* region, size_t fromOffset, size_t fromSize)
{
    if (region == nullptr) {
        return 0;
    }
    MAddress regionStart = region->GetRegionStart();
    size_t ghostSz = region->GetGhostRegionSize();
    MAddress regionEnd = ghostSz > 0 ? regionStart + ghostSz : region->GetRegionEnd();
    MAddress cursor = regionStart + fromOffset + (fromSize > 0 ? fromSize : 8);
    size_t guard = 0;
    constexpr size_t kMaxWalk = 4096;
    while (cursor < regionEnd && guard < kMaxWalk) {
        ++guard;
        BaseObject* cand = reinterpret_cast<BaseObject*>(cursor);
        if (!Heap::IsHeapAddress(cursor) || !cand->IsValidObject()) {
            return 0;
        }
        TypeInfo* ti = cand->GetTypeInfo();
        if (ti == nullptr || !ti->IsVaildType()) {
            return 0;
        }
        size_t candSize = RegionSpace::GetAllocSize(*cand);
        if (candSize == 0) {
            return 0;
        }
        if (region->IsMarkedObject(cand)) {
            uint64_t pre = region->GetPreLiveBytesInGhostRegion(cursor);
            return static_cast<uintptr_t>(region->GetRoutePlanAddr(pre));
        }
        cursor += candSize;
    }
    return 0;
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void NoteRoute(RegionInfo* region, BaseObject* fromObj, uint64_t preLiveBytes, uintptr_t toAddr)
{
    if (!GateOn()) {
        return;
    }
    EnsureAtexit();
    g_total.fetch_add(1, std::memory_order_relaxed);

    size_t offset = 0;
    if (region != nullptr && fromObj != nullptr) {
        offset = region->GetAddressOffset(reinterpret_cast<MAddress>(fromObj));
    }

    Covering cov = FindCovering(region, offset);
    bool isStart = cov.found && cov.isStart;
    bool isInterior = cov.found && !cov.isStart;
    if (isStart) {
        g_start.fetch_add(1, std::memory_order_relaxed);
    } else if (isInterior) {
        g_interior.fetch_add(1, std::memory_order_relaxed);
    } else {
        // No covering marked object found — count as unmarked domain leak (legacy).
        g_unmarked.fetch_add(1, std::memory_order_relaxed);
    }

    // Alias: toAddr equals next marked object's to (legacy unmarked path).
    size_t objSize = 0;
    TypeInfo* ti = nullptr;
    if (fromObj != nullptr && fromObj->IsValidObject()) {
        ti = fromObj->GetTypeInfo();
        if (ti != nullptr && ti->IsVaildType()) {
            objSize = RegionSpace::GetAllocSize(*fromObj);
        }
    }
    uintptr_t nextTo = NextMarkedToAddr(region, offset, objSize);
    bool isAlias = (nextTo != 0 && nextTo == toAddr);
    if (isAlias) {
        g_alias.fetch_add(1, std::memory_order_relaxed);
    }

    bool invert = InvertOn();
    bool markedBit = (region != nullptr && fromObj != nullptr) ? region->IsMarkedObject(fromObj) : false;
    // Detail log: always log INTERIOR (capped); START only under invert or as sparse samples.
    bool wantLog = false;
    if (isInterior) {
        size_t nI = g_loggedInterior.fetch_add(1, std::memory_order_relaxed) + 1;
        wantLog = (nI <= MaxSamples());
    } else if (invert) {
        size_t n = g_logged.fetch_add(1, std::memory_order_relaxed) + 1;
        wantLog = (n <= MaxSamples());
    } else if (!cov.found) {
        size_t n = g_logged.fetch_add(1, std::memory_order_relaxed) + 1;
        wantLog = (n <= MaxSamples());
    }
    if (!wantLog) {
        return;
    }

    unsigned young = 0;
    unsigned rtype = 0;
    unsigned ghostSurv = 0;
    if (region != nullptr) {
        young = static_cast<unsigned>(region->IsYoungRegion());
        rtype = static_cast<unsigned>(region->GetRegionType());
        LiveInfo* g = region->GetLiveInfo0ForProbe();
        if (g != nullptr) {
            ghostSurv = static_cast<unsigned>(g->IsSurvivedObject(offset));
        }
    }

    const char* kind = isInterior ? "INTERIOR" : (isStart ? "START" : "UNCOVERED");
    LOG(RTLOG_ERROR,
        "[GCV2][routedom] n=%zu kind=%s obj=%p region=%p offset=%zu size=%zu tip=%p "
        "preLiveBytes=%llu toAddr=%#zx nextMarkedTo=%#zx alias=%u markedBit=%u invert=%u "
        "ghostSurv=%u young=%u type=%u host=%p hostOff=%zu hostDelta=%zu hostSize=%zu "
        "hostTip=%p hostTo=%#zx",
        g_total.load(std::memory_order_relaxed), kind, fromObj, region, offset, objSize, ti,
        static_cast<unsigned long long>(preLiveBytes),
        static_cast<size_t>(toAddr), static_cast<size_t>(nextTo),
        static_cast<unsigned>(isAlias), static_cast<unsigned>(markedBit),
        static_cast<unsigned>(invert), ghostSurv, young, rtype,
        cov.host, cov.hostOffset, cov.delta, cov.hostSize, cov.hostTip,
        static_cast<size_t>(cov.hostToAddr));
}

void DumpSummary()
{
    if (!GateOn()) {
        return;
    }
    size_t total = g_total.load(std::memory_order_relaxed);
    size_t start = g_start.load(std::memory_order_relaxed);
    size_t interior = g_interior.load(std::memory_order_relaxed);
    size_t unmarked = g_unmarked.load(std::memory_order_relaxed);
    size_t alias = g_alias.load(std::memory_order_relaxed);
    LOG(RTLOG_ERROR,
        "ROUTEDOM total=%zu start=%zu interior=%zu unmarked=%zu alias=%zu invert=%u",
        total, start, interior, unmarked, alias, static_cast<unsigned>(InvertOn()));
}

} // namespace RouteDom
} // namespace MapleRuntime
