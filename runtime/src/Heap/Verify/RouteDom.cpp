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
std::atomic<size_t> g_unmarked{ 0 };
std::atomic<size_t> g_alias{ 0 };
std::atomic<size_t> g_logged{ 0 };
std::atomic<bool> g_atexit{ false };

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { DumpSummary(); });
    }
}

// Next marked object after fromObj in the same region (size walk); 0 if none / walk fail.
uintptr_t NextMarkedToAddr(RegionInfo* region, BaseObject* fromObj, size_t fromOffset)
{
    if (region == nullptr || fromObj == nullptr) {
        return 0;
    }
    if (!fromObj->IsValidObject()) {
        return 0;
    }
    size_t objSize = 0;
    {
        // Size only when tip is plausible; otherwise cannot walk.
        TypeInfo* ti = fromObj->GetTypeInfo();
        if (ti == nullptr || !ti->IsVaildType()) {
            return 0;
        }
        objSize = RegionSpace::GetAllocSize(*fromObj);
        if (objSize == 0) {
            return 0;
        }
    }
    MAddress regionStart = region->GetRegionStart();
    MAddress regionEnd = region->GetGhostRegionSize() > 0
        ? regionStart + region->GetGhostRegionSize()
        : region->GetRegionEnd();
    MAddress cursor = reinterpret_cast<MAddress>(fromObj) + objSize;
    size_t guard = 0;
    constexpr size_t kMaxWalk = 4096;
    while (cursor < regionEnd && guard < kMaxWalk) {
        ++guard;
        BaseObject* cand = reinterpret_cast<BaseObject*>(cursor);
        if (!Heap::IsHeapAddress(cursor) || !cand->IsValidObject()) {
            // Cannot size-walk further safely.
            return 0;
        }
        TypeInfo* ti = cand->GetTypeInfo();
        if (ti == nullptr || !ti->IsVaildType()) {
            return 0;
        }
        size_t candOff = static_cast<size_t>(cursor - regionStart);
        bool marked = region->IsMarkedObject(cand);
        size_t candSize = RegionSpace::GetAllocSize(*cand);
        if (candSize == 0) {
            return 0;
        }
        if (marked) {
            uint64_t pre = region->GetPreLiveBytesInGhostRegion(cursor);
            return static_cast<uintptr_t>(region->GetRoutePlanAddr(pre));
        }
        (void)candOff;
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

    bool marked = (region != nullptr && fromObj != nullptr) ? region->IsMarkedObject(fromObj) : false;
    bool invert = InvertOn();
    // Formal: violation = unmarked. Positive control: invert → treat marked as violation.
    bool violation = invert ? marked : !marked;
    if (!violation) {
        return;
    }
    g_unmarked.fetch_add(1, std::memory_order_relaxed);

    uintptr_t nextTo = 0;
    bool isAlias = false;
    if (region != nullptr && fromObj != nullptr) {
        size_t off = region->GetAddressOffset(reinterpret_cast<MAddress>(fromObj));
        nextTo = NextMarkedToAddr(region, fromObj, off);
        if (nextTo != 0 && nextTo == toAddr) {
            isAlias = true;
            g_alias.fetch_add(1, std::memory_order_relaxed);
        }
    }

    size_t n = g_logged.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > MaxSamples()) {
        return;
    }

    size_t objSize = 0;
    TypeInfo* ti = nullptr;
    if (fromObj != nullptr && fromObj->IsValidObject()) {
        ti = fromObj->GetTypeInfo();
        if (ti != nullptr && ti->IsVaildType()) {
            objSize = RegionSpace::GetAllocSize(*fromObj);
        }
    }
    size_t offset = 0;
    unsigned young = 0;
    unsigned rtype = 0;
    unsigned ghostSurv = 0;
    if (region != nullptr && fromObj != nullptr) {
        offset = region->GetAddressOffset(reinterpret_cast<MAddress>(fromObj));
        young = static_cast<unsigned>(region->IsYoungRegion());
        rtype = static_cast<unsigned>(region->GetRegionType());
        LiveInfo* g = region->GetLiveInfo0ForProbe();
        if (g != nullptr) {
            ghostSurv = static_cast<unsigned>(g->IsSurvivedObject(offset));
        }
    }

    LOG(RTLOG_ERROR,
        "[GCV2][routedom] n=%zu obj=%p region=%p offset=%zu size=%zu tip=%p "
        "preLiveBytes=%llu toAddr=%#zx nextMarkedTo=%#zx alias=%u marked=%u invert=%u "
        "ghostSurv=%u young=%u type=%u",
        n, fromObj, region, offset, objSize, ti,
        static_cast<unsigned long long>(preLiveBytes),
        static_cast<size_t>(toAddr), static_cast<size_t>(nextTo),
        static_cast<unsigned>(isAlias), static_cast<unsigned>(marked),
        static_cast<unsigned>(invert), ghostSurv, young, rtype);
}

void DumpSummary()
{
    if (!GateOn()) {
        return;
    }
    size_t total = g_total.load(std::memory_order_relaxed);
    size_t unmarked = g_unmarked.load(std::memory_order_relaxed);
    size_t alias = g_alias.load(std::memory_order_relaxed);
    LOG(RTLOG_ERROR, "ROUTEDOM total=%zu alias=%zu unmarked=%zu invert=%u",
        total, alias, unmarked, static_cast<unsigned>(InvertOn()));
}

} // namespace RouteDom
} // namespace MapleRuntime
