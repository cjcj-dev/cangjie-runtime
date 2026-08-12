// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/OffpastDiag.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace OffpastDiag {
namespace {

constexpr size_t kCap = 128;

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && v[0] == '1' && v[1] == '\0';
}

bool GateOn()
{
    static const bool on = []() {
        if (EnvIsOne("MRT_GCV2_OFFPAST")) {
            return true;
        }
        return DiagGate::TokenOn("offpast");
    }();
    return on;
}

std::atomic<bool> gHeartbeat{ false };

void HeartbeatOnce()
{
    bool expect = false;
    if (gHeartbeat.compare_exchange_strong(expect, true, std::memory_order_relaxed)) {
        std::fprintf(stderr, "[GCV2][offpast] heartbeat env=MRT_GCV2_OFFPAST=1 cap=%zu\n", kCap);
        std::fflush(stderr);
    }
}

struct Snap {
    uintptr_t g0 = 0;
    uintptr_t live = 0;
    uintptr_t bm = 0;
    uint32_t off = 0;
    uint32_t liveBytes = 0;
    uint8_t surv = 0;
    uint8_t rs = 0xff;
    uint8_t g0Null = 1;
    uint8_t bmNull = 1;
    uint8_t sameG0Live = 0;
};

struct Slot {
    BaseObject* target = nullptr;
    RegionInfo* region = nullptr;
    const char* site = "";
    Snap pre{};
    Snap route{};
    Snap compact{};
    Snap fix{};
    uint8_t seenPre = 0;
    uint8_t seenRoute = 0;
    uint8_t seenCompact = 0;
    uint8_t seenFix = 0;
};

Slot gSlots[kCap];
std::atomic<size_t> gN{ 0 };
std::atomic<size_t> gFixUnknown{ 0 };
std::atomic<bool> gAtexit{ false };

void FillSnap(Snap& s, RegionInfo* region, BaseObject* obj)
{
    if (region == nullptr || obj == nullptr) {
        return;
    }
    s.rs = static_cast<uint8_t>(region->GetRouteState());
    s.liveBytes = static_cast<uint32_t>(region->GetLiveByteCount());
    s.off = static_cast<uint32_t>(region->GetAddressOffset(reinterpret_cast<MAddress>(obj)));
    LiveInfo* g0 = region->GetLiveInfo0ForProbe();
    LiveInfo* live = region->GetLiveInfo();
    s.g0 = reinterpret_cast<uintptr_t>(g0);
    s.live = reinterpret_cast<uintptr_t>(live);
    s.g0Null = (g0 == nullptr) ? 1 : 0;
    s.sameG0Live = (g0 != nullptr && g0 == live) ? 1 : 0;
    if (g0 != nullptr) {
        RegionBitmap* bm = g0->markBitmap;
        s.bm = reinterpret_cast<uintptr_t>(bm);
        s.bmNull = (bm == nullptr || reinterpret_cast<MAddress>(bm) == LiveInfo::TEMPORARY_PTR) ? 1 : 0;
        s.surv = g0->IsSurvivedObject(static_cast<size_t>(s.off)) ? 1 : 0;
    } else {
        s.bm = 0;
        s.bmNull = 1;
        s.surv = 0;
    }
}

void PrintSnap(const char* tag, const Slot& sl, const Snap& s)
{
    std::fprintf(stderr,
                 "[GCV2][offpast] %s target=%p region=%p site=%s off=%u liveBytes=%u rs=%u "
                 "surv=%u g0Null=%u bmNull=%u sameG0Live=%u g0=%p live=%p bm=%p\n",
                 tag, static_cast<void*>(sl.target), static_cast<void*>(sl.region), sl.site,
                 s.off, s.liveBytes, static_cast<unsigned>(s.rs), static_cast<unsigned>(s.surv),
                 static_cast<unsigned>(s.g0Null), static_cast<unsigned>(s.bmNull),
                 static_cast<unsigned>(s.sameG0Live), reinterpret_cast<void*>(s.g0),
                 reinterpret_cast<void*>(s.live), reinterpret_cast<void*>(s.bm));
}

Slot* Find(BaseObject* obj)
{
    const size_t n = gN.load(std::memory_order_acquire);
    for (size_t i = 0; i < n && i < kCap; ++i) {
        if (gSlots[i].target == obj) {
            return &gSlots[i];
        }
    }
    return nullptr;
}

void DumpAtexit()
{
    const size_t n = gN.load(std::memory_order_relaxed);
    size_t preSurv = 0;
    size_t preDead = 0;
    size_t fixKnown = 0;
    size_t flipped = 0;
    size_t g0Changed = 0;
    size_t bmChanged = 0;
    for (size_t i = 0; i < n && i < kCap; ++i) {
        const Slot& sl = gSlots[i];
        if (sl.seenPre) {
            if (sl.pre.surv) {
                ++preSurv;
            } else {
                ++preDead;
            }
        }
        if (sl.seenFix) {
            ++fixKnown;
            if (sl.seenPre && sl.pre.surv && !sl.fix.surv) {
                ++flipped;
            }
            if (sl.seenPre && sl.pre.g0 != sl.fix.g0) {
                ++g0Changed;
            }
            if (sl.seenPre && sl.pre.bm != sl.fix.bm) {
                ++bmChanged;
            }
        }
    }
    std::fprintf(stderr,
                 "[GCV2][offpast] summary tracked=%zu preSurv=%zu preDead=%zu fixKnown=%zu "
                 "fixUnknown=%zu flippedSurv1to0=%zu g0Changed=%zu bmChanged=%zu\n",
                 n, preSurv, preDead, fixKnown, gFixUnknown.load(std::memory_order_relaxed),
                 flipped, g0Changed, bmChanged);
    std::fflush(stderr);
}

void EnsureAtexit()
{
    bool expect = false;
    if (gAtexit.compare_exchange_strong(expect, true, std::memory_order_relaxed)) {
        std::atexit(DumpAtexit);
    }
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void NotePregrant(BaseObject* obj, const char* site)
{
    if (!GateOn() || obj == nullptr) {
        return;
    }
    HeartbeatOnce();
    EnsureAtexit();
    if (Find(obj) != nullptr) {
        return;
    }
    size_t i = gN.fetch_add(1, std::memory_order_acq_rel);
    if (i >= kCap) {
        return;
    }
    Slot& sl = gSlots[i];
    sl.target = obj;
    sl.region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (sl.region == nullptr) {
        sl.region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    }
    sl.site = (site != nullptr) ? site : "";
    FillSnap(sl.pre, sl.region, obj);
    sl.seenPre = 1;
    if (i < 64) {
        PrintSnap("pregrant", sl, sl.pre);
    }
}

void NoteRouteEnter(RegionInfo* region)
{
    if (!GateOn() || region == nullptr) {
        return;
    }
    HeartbeatOnce();
    const size_t n = gN.load(std::memory_order_acquire);
    size_t printed = 0;
    for (size_t i = 0; i < n && i < kCap; ++i) {
        Slot& sl = gSlots[i];
        if (sl.region != region || sl.target == nullptr) {
            continue;
        }
        FillSnap(sl.route, region, sl.target);
        sl.seenRoute = 1;
        if (printed < 16) {
            PrintSnap("route", sl, sl.route);
            ++printed;
        }
    }
}

void NoteCompactDone(RegionInfo* region)
{
    if (!GateOn() || region == nullptr) {
        return;
    }
    HeartbeatOnce();
    const size_t n = gN.load(std::memory_order_acquire);
    size_t printed = 0;
    for (size_t i = 0; i < n && i < kCap; ++i) {
        Slot& sl = gSlots[i];
        if (sl.region != region || sl.target == nullptr) {
            continue;
        }
        FillSnap(sl.compact, region, sl.target);
        sl.seenCompact = 1;
        if (printed < 16) {
            PrintSnap("compact", sl, sl.compact);
            ++printed;
        }
    }
}

void NoteFixMiss(BaseObject* obj)
{
    if (!GateOn() || obj == nullptr) {
        return;
    }
    HeartbeatOnce();
    EnsureAtexit();
    Slot* sl = Find(obj);
    if (sl == nullptr) {
        size_t unk = gFixUnknown.fetch_add(1, std::memory_order_relaxed) + 1;
        RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
        if (region == nullptr) {
            region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        }
        Snap s{};
        FillSnap(s, region, obj);
        if (unk <= 32) {
            std::fprintf(stderr,
                         "[GCV2][offpast] fix_unknown n=%zu target=%p region=%p off=%u liveBytes=%u "
                         "rs=%u surv=%u g0Null=%u bmNull=%u g0=%p live=%p bm=%p\n",
                         unk, static_cast<void*>(obj), static_cast<void*>(region), s.off, s.liveBytes,
                         static_cast<unsigned>(s.rs), static_cast<unsigned>(s.surv),
                         static_cast<unsigned>(s.g0Null), static_cast<unsigned>(s.bmNull),
                         reinterpret_cast<void*>(s.g0), reinterpret_cast<void*>(s.live),
                         reinterpret_cast<void*>(s.bm));
            std::fflush(stderr);
        }
        return;
    }
    FillSnap(sl->fix, sl->region != nullptr ? sl->region : RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj)),
             obj);
    sl->seenFix = 1;
    PrintSnap("fix", *sl, sl->fix);
    std::fprintf(stderr,
                 "[GCV2][offpast] same_target target=%p pre.surv=%u route.surv=%u compact.surv=%u fix.surv=%u "
                 "pre.g0=%p fix.g0=%p pre.bm=%p fix.bm=%p g0chg=%u bmchg=%u "
                 "pre.off=%u fix.off=%u pre.liveBytes=%u fix.liveBytes=%u pre.rs=%u fix.rs=%u\n",
                 static_cast<void*>(sl->target), static_cast<unsigned>(sl->pre.surv),
                 static_cast<unsigned>(sl->route.surv), static_cast<unsigned>(sl->compact.surv),
                 static_cast<unsigned>(sl->fix.surv), reinterpret_cast<void*>(sl->pre.g0),
                 reinterpret_cast<void*>(sl->fix.g0), reinterpret_cast<void*>(sl->pre.bm),
                 reinterpret_cast<void*>(sl->fix.bm),
                 static_cast<unsigned>(sl->pre.g0 != sl->fix.g0),
                 static_cast<unsigned>(sl->pre.bm != sl->fix.bm), sl->pre.off, sl->fix.off,
                 sl->pre.liveBytes, sl->fix.liveBytes, static_cast<unsigned>(sl->pre.rs),
                 static_cast<unsigned>(sl->fix.rs));
    std::fflush(stderr);
}

} // namespace OffpastDiag
} // namespace MapleRuntime
