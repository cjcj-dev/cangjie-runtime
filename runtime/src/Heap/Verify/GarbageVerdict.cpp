// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/GarbageVerdict.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "Allocator/RegionInfo.h"
#include "Allocator/RegionSpace.h"
#include "Base/Log.h"
#include "Common/BaseObject.h"

namespace MapleRuntime {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

constexpr size_t kRing = 256;

struct Slot {
    MAddress start = 0;
    MAddress end = 0;
    MAddress alloc = 0;
    uint32_t live = 0;
    uint32_t liveAuth = 0;
    uint32_t regionType = 0;
    uint32_t young = 0;
    uint32_t pinned = 0;
    uint32_t large = 0;
    uint32_t tl = 0;
    uint32_t neverExamined = 0;
    uint32_t knownEmpty = 0;
    size_t validObjs = 0;
    size_t markedObjs = 0;
    char site[24] = {};
    char predicate[48] = {};
    uint64_t seq = 0;
};

std::atomic<uint64_t> gSeq{ 0 };
std::atomic<size_t> gWrite{ 0 };
Slot gRing[kRing];

} // namespace

bool GarbageVerdict::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_GARBAGE_VERDICT");
    return on;
}

bool GarbageVerdict::BlockNeverExamined()
{
    static const bool on = EnvIsOne("MRT_GCV2_BLOCK_NEVEREXAMINED");
    return on;
}

size_t GarbageVerdict::CountValidObjectHeaders(RegionInfo* region)
{
    if (region == nullptr) {
        return 0;
    }
    if (region->IsLargeRegion()) {
        BaseObject* o = reinterpret_cast<BaseObject*>(region->GetRegionStart());
        return o->IsValidObject() ? 1u : 0u;
    }
    uintptr_t pos = region->GetRegionStart();
    uintptr_t alloc = region->GetRegionAllocPtr();
    size_t n = 0;
    while (pos < alloc) {
        BaseObject* o = reinterpret_cast<BaseObject*>(pos);
        if (!o->IsValidObject()) {
            break;
        }
        size_t sz = RegionSpace::GetAllocSize(*o);
        if (sz == 0) {
            break;
        }
        ++n;
        pos += sz;
    }
    return n;
}

void GarbageVerdict::Dump(const char* site, RegionInfo* region, const char* predicate)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    MAddress start = region->GetRegionStart();
    MAddress end = region->GetRegionEnd();
    MAddress alloc = region->GetRegionAllocPtr();
    uint32_t live = region->GetLiveByteCount();
    uint32_t liveAuth = region->IsLiveCountAuthoritative() ? 1u : 0u;
    uint32_t young = region->IsYoungRegion() ? 1u : 0u;
    uint32_t pinned = region->IsPinnedRegion() ? 1u : 0u;
    uint32_t large = region->IsLargeRegion() ? 1u : 0u;
    uint32_t tl = region->IsThreadLocalRegion() ? 1u : 0u;
    uint32_t neverExamined =
        (region->GetMarkBitmap() == nullptr && alloc > start) ? 1u : 0u;
    uint32_t knownEmpty = region->IsKnownEmpty() ? 1u : 0u;
    size_t validObjs = CountValidObjectHeaders(region);
    size_t markedObjs = 0;
    if (!large && alloc > start && region->GetMarkBitmap() != nullptr) {
        uintptr_t pos = start;
        while (pos < alloc) {
            BaseObject* o = reinterpret_cast<BaseObject*>(pos);
            if (!o->IsValidObject()) {
                break;
            }
            size_t sz = RegionSpace::GetAllocSize(*o);
            if (sz == 0) {
                break;
            }
            if (region->IsMarkedObject(o)) {
                ++markedObjs;
            }
            pos += sz;
        }
    }

    uint64_t seq = gSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t wi = gWrite.fetch_add(1, std::memory_order_relaxed) % kRing;
    Slot& s = gRing[wi];
    s.start = start;
    s.end = end;
    s.alloc = alloc;
    s.live = live;
    s.liveAuth = liveAuth;
    s.regionType = static_cast<uint32_t>(region->GetRegionType());
    s.young = young;
    s.pinned = pinned;
    s.large = large;
    s.tl = tl;
    s.neverExamined = neverExamined;
    s.knownEmpty = knownEmpty;
    s.validObjs = validObjs;
    s.markedObjs = markedObjs;
    s.seq = seq;
    std::snprintf(s.site, sizeof(s.site), "%s", site != nullptr ? site : "?");
    std::snprintf(s.predicate, sizeof(s.predicate), "%s", predicate != nullptr ? predicate : "?");

    VLOG(REPORT,
         "[GCV2][GARBAGE_VERDICT_DUMP] seq=%llu site=%s pred=%s region=%p start=%#zx alloc=%#zx end=%#zx "
         "size=%zu type=%u young=%u pinned=%u large=%u tl=%u neverExamined=%u knownEmpty=%u "
         "live=%u liveAuth=%u validObjs=%zu markedObjs=%zu liveSrc=%s",
         static_cast<unsigned long long>(seq), s.site, s.predicate, region,
         static_cast<size_t>(start), static_cast<size_t>(alloc), static_cast<size_t>(end),
         static_cast<size_t>(end - start), s.regionType, young, pinned, large, tl, neverExamined,
         knownEmpty, live, liveAuth, validObjs, markedObjs,
         neverExamined ? "ClearLiveInfo_AUTHORITY0_no_bitmap"
                       : (liveAuth ? "mark_AddLiveByteCount" : "bare_zero_no_authority"));
}

void GarbageVerdict::CrossCheck(MAddress crashAddr)
{
    if (!Enabled() || crashAddr == 0) {
        return;
    }
    size_t hits = 0;
    // Scan whole ring; print matches newest-first by seq.
    for (size_t pass = 0; pass < kRing; ++pass) {
        const Slot& s = gRing[pass];
        if (s.seq == 0 || s.start == 0) {
            continue;
        }
        if (crashAddr < s.start || crashAddr >= s.end) {
            continue;
        }
        ++hits;
        VLOG(REPORT,
             "[GCV2][CROSS_CHECK] crash=%#zx hitSeq=%llu site=%s pred=%s regionStart=%#zx end=%#zx "
             "live=%u liveAuth=%u neverExamined=%u knownEmpty=%u validObjs=%zu markedObjs=%zu "
             "inRange=1",
             static_cast<size_t>(crashAddr), static_cast<unsigned long long>(s.seq), s.site,
             s.predicate, static_cast<size_t>(s.start), static_cast<size_t>(s.end), s.live,
             s.liveAuth, s.neverExamined, s.knownEmpty, s.validObjs, s.markedObjs);
    }
    if (hits == 0) {
        VLOG(REPORT,
             "[GCV2][CROSS_CHECK] crash=%#zx hits=0 (no prior GARBAGE_VERDICT_DUMP covered this addr; "
             "region may have been Init'd after reclaim)",
             static_cast<size_t>(crashAddr));
    } else {
        VLOG(REPORT, "[GCV2][CROSS_CHECK] crash=%#zx totalHits=%zu", static_cast<size_t>(crashAddr),
             hits);
    }
}

} // namespace MapleRuntime
