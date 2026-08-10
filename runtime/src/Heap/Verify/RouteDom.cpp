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
};

bool PlausibleStart(BaseObject* obj, TypeInfo*& outTip, size_t& outSize)
{
    outTip = nullptr;
    outSize = 0;
    if (obj == nullptr || !Heap::IsHeapAddress(reinterpret_cast<MAddress>(obj))) {
        return false;
    }
    if (!obj->IsValidObject()) {
        return false;
    }
    TypeInfo* ti = obj->GetTypeInfo();
    if (ti == nullptr || !ti->IsVaildType()) {
        return false;
    }
    size_t sz = RegionSpace::GetAllocSize(*obj);
    if (sz == 0 || (sz & 7u) != 0) {
        return false;
    }
    outTip = ti;
    outSize = sz;
    return true;
}

// Walk backward from targetOffset to find a marked object start covering it.
// MarkBits paints multi-byte ranges, so IsMarkedObject(interior) can be true;
// START requires a plausible object header at that address.
Covering FindCoveringBackward(RegionInfo* region, size_t targetOffset)
{
    Covering c;
    if (region == nullptr) {
        return c;
    }
    MAddress regionStart = region->GetRegionStart();
    // Bound search: max small-object search window (region unit is typically 64K).
    size_t ghostSz = region->GetGhostRegionSize();
    size_t maxBack = ghostSz > 0 ? ghostSz : static_cast<size_t>(region->GetRegionEnd() - regionStart);
    if (maxBack > targetOffset) {
        maxBack = targetOffset;
    }
    // Cap steps so a single probe stays cheap (4096 * 8 = 32KB).
    size_t steps = 0;
    constexpr size_t kMaxSteps = 4096;
    for (size_t back = 0; back <= maxBack && steps < kMaxSteps; back += 8, ++steps) {
        size_t candOff = targetOffset - back;
        BaseObject* cand = reinterpret_cast<BaseObject*>(regionStart + candOff);
        // Prefer mark bit at candidate start; also accept ghost survived.
        bool marked = region->IsMarkedObject(candOff);
        if (!marked) {
            LiveInfo* g = region->GetLiveInfo0ForProbe();
            if (g != nullptr && g->IsSurvivedObject(candOff)) {
                marked = true;
            }
        }
        if (!marked) {
            continue;
        }
        TypeInfo* tip = nullptr;
        size_t sz = 0;
        if (!PlausibleStart(cand, tip, sz)) {
            continue;
        }
        if (candOff + sz <= targetOffset) {
            // Candidate ends at/before target — cannot cover.
            continue;
        }
        // candOff <= targetOffset < candOff+sz
        c.found = true;
        c.isStart = (candOff == targetOffset);
        c.host = cand;
        c.hostOffset = candOff;
        c.hostSize = sz;
        c.delta = targetOffset - candOff;
        c.hostTip = tip;
        uint64_t pre = region->GetPreLiveBytesInGhostRegion(regionStart + candOff);
        c.hostToAddr = static_cast<uintptr_t>(region->GetRoutePlanAddr(pre));
        return c;
    }
    return c;
}

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
    constexpr size_t kMaxWalk = 512;
    while (cursor < regionEnd && guard < kMaxWalk) {
        ++guard;
        BaseObject* cand = reinterpret_cast<BaseObject*>(cursor);
        if (!Heap::IsHeapAddress(cursor)) {
            return 0;
        }
        TypeInfo* tip = nullptr;
        size_t candSize = 0;
        if (!PlausibleStart(cand, tip, candSize)) {
            cursor += 8;
            continue;
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

    // MarkBits paints whole object range; IsMarkedObject(interior) can be true.
    // START ⇔ mark/survive at offset AND plausible object header at offset.
    // INTERIOR ⇔ covering marked host exists with hostStart < offset < hostStart+size.
    TypeInfo* selfTip = nullptr;
    size_t selfSize = 0;
    bool plausibleSelf = PlausibleStart(fromObj, selfTip, selfSize);
    bool markedBit = (region != nullptr && fromObj != nullptr) ? region->IsMarkedObject(fromObj) : false;
    if (!markedBit && region != nullptr) {
        LiveInfo* g = region->GetLiveInfo0ForProbe();
        if (g != nullptr && g->IsSurvivedObject(offset)) {
            markedBit = true; // survived via resurrect or ghost face
        }
    }

    Covering cov;
    bool isStart = false;
    bool isInterior = false;
    if (markedBit && plausibleSelf) {
        // Object-start in domain (common path — no walk).
        isStart = true;
        cov.found = true;
        cov.isStart = true;
        cov.host = fromObj;
        cov.hostOffset = offset;
        cov.hostSize = selfSize;
        cov.delta = 0;
        cov.hostTip = selfTip;
        cov.hostToAddr = toAddr;
        g_start.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Candidate INTERIOR / UNCOVERED: backward host search (cheap; host usually nearby).
        cov = FindCoveringBackward(region, offset);
        isStart = cov.found && cov.isStart;
        isInterior = cov.found && !cov.isStart;
        if (isStart) {
            g_start.fetch_add(1, std::memory_order_relaxed);
        } else if (isInterior) {
            g_interior.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_unmarked.fetch_add(1, std::memory_order_relaxed);
        }
    }

    uintptr_t nextTo = 0;
    bool isAlias = false;
    if (isInterior || !cov.found) {
        nextTo = NextMarkedToAddr(region, offset, selfSize > 0 ? selfSize : (cov.hostSize > 0 ? cov.hostSize : 8));
        isAlias = (nextTo != 0 && nextTo == toAddr);
        if (isAlias) {
            g_alias.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool invert = InvertOn();
    bool wantLog = false;
    if (isInterior) {
        size_t nI = g_loggedInterior.fetch_add(1, std::memory_order_relaxed) + 1;
        wantLog = (nI <= MaxSamples());
    } else if (invert && isStart) {
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
    size_t objSize = selfSize > 0 ? selfSize : cov.hostSize;
    TypeInfo* ti = selfTip != nullptr ? selfTip : cov.hostTip;
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
