// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/F3Why2Diag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace F3Why2Diag {
namespace {

// FysAuditDiag.cpp:29 / :52-56 shape — default off; MRT_GCV2_F3WHY2=1 or token f3why2.
bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        if (EnvIsOne("MRT_GCV2_F3WHY2")) {
            return true;
        }
        return DiagGate::TokenOn("f3why2");
    }();
    return on;
}

constexpr size_t kRegionSetCap = 1u << 20;
constexpr size_t kSampleCap = 64;

std::atomic<size_t> g_enter{ 0 };
std::atomic<size_t> g_clsKnownEmpty{ 0 };
std::atomic<size_t> g_clsKnownEmptyMarked{ 0 };
std::atomic<size_t> g_clsAuthLivePositive{ 0 };
std::atomic<size_t> g_clsAuthZero{ 0 };
std::atomic<size_t> g_clsNoAuth{ 0 };
std::atomic<size_t> g_clsNeverExamined{ 0 };
std::atomic<size_t> g_clsHasBitmap{ 0 };
std::atomic<size_t> g_clsYoung{ 0 };
std::atomic<size_t> g_clsLarge{ 0 };
std::atomic<size_t> g_clsRouteForwarded{ 0 };
std::atomic<size_t> g_clsRouteOther{ 0 };
std::atomic<size_t> g_sumValidObjs{ 0 };
std::atomic<size_t> g_sumMarkedObjs{ 0 };
std::atomic<size_t> g_regionsWithMarkGt0{ 0 };

std::atomic<size_t> g_collectSetSize{ 0 };
std::atomic<size_t> g_collectSetSat{ 0 };
RegionInfo* g_collectSet[kRegionSetCap] {};

std::atomic<size_t> g_f3RgHits{ 0 };
std::atomic<size_t> g_f3RgJoinHit{ 0 };
std::atomic<size_t> g_f3RgJoinMiss{ 0 };
std::atomic<size_t> g_f3RgMark1{ 0 };
std::atomic<size_t> g_f3RgMark0{ 0 };
std::atomic<size_t> g_f3RgKnownEmpty{ 0 };
std::atomic<size_t> g_f3RgAuth{ 0 };
std::atomic<size_t> g_f3RgLiveBytesSum{ 0 };
std::atomic<size_t> g_f3RgSampleLogged{ 0 };

// ORDER: live emptied while marks still 1 (cleared-early), vs marks never shared live.
std::atomic<size_t> g_orderSamples{ 0 };
std::atomic<size_t> g_orderLivePosBefore{ 0 };      // liveBefore > 0
std::atomic<size_t> g_orderMarkedPosBefore{ 0 };    // markedBefore > 0
std::atomic<size_t> g_orderLive0AfterReset{ 0 };    // liveAfterReset == 0
std::atomic<size_t> g_orderMarkedPosAfterReset{ 0 }; // markedAfterReset > 0  ⇒ residual after clear live
std::atomic<size_t> g_orderMarked0AfterInv{ 0 };    // markedAfterInvalidate == 0
std::atomic<size_t> g_orderClearedEarly{ 0 };       // live>0 before, live==0 after reset, marks>0 after reset
std::atomic<size_t> g_orderNeverHomologous{ 0 };    // live==0 before AND marks>0 before (never shared book)
std::atomic<size_t> g_orderSampleLogged{ 0 };

std::atomic<bool> g_atexit{ false };
std::atomic<size_t> g_sampleLogged{ 0 };

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

bool InsertRegion(RegionInfo* region)
{
    if (region == nullptr) {
        return false;
    }
    size_t idx = (reinterpret_cast<uintptr_t>(region) >> 4) & (kRegionSetCap - 1);
    for (size_t probe = 0; probe < 64; ++probe) {
        size_t i = (idx + probe) & (kRegionSetCap - 1);
        RegionInfo* cur = __atomic_load_n(&g_collectSet[i], std::memory_order_acquire);
        if (cur == region) {
            return true;
        }
        if (cur == nullptr) {
            RegionInfo* expected = nullptr;
            if (__atomic_compare_exchange_n(&g_collectSet[i], &expected, region, false, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                size_t n = g_collectSetSize.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n >= kRegionSetCap - 1024) {
                    g_collectSetSat.fetch_add(1, std::memory_order_relaxed);
                }
                return true;
            }
            if (expected == region) {
                return true;
            }
        }
    }
    g_collectSetSat.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool ContainsRegion(RegionInfo* region)
{
    if (region == nullptr) {
        return false;
    }
    size_t idx = (reinterpret_cast<uintptr_t>(region) >> 4) & (kRegionSetCap - 1);
    for (size_t probe = 0; probe < 64; ++probe) {
        size_t i = (idx + probe) & (kRegionSetCap - 1);
        RegionInfo* cur = __atomic_load_n(&g_collectSet[i], std::memory_order_acquire);
        if (cur == region) {
            return true;
        }
        if (cur == nullptr) {
            return false;
        }
    }
    return false;
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void CountMarks(RegionInfo* region, size_t& validOut, size_t& markedOut)
{
    validOut = 0;
    markedOut = 0;
    if (!GateOn()) {
        return;
    }
    if (region == nullptr || region->IsLargeRegion()) {
        if (region != nullptr && region->IsLargeRegion() && region->IsMarkedObject(static_cast<size_t>(0))) {
            markedOut = 1;
            validOut = 1;
        }
        return;
    }
    size_t start = region->GetRegionStart();
    size_t alloc = region->GetRegionAllocPtr();
    if (alloc <= start) {
        return;
    }
    uintptr_t pos = start;
    size_t steps = 0;
    constexpr size_t kMaxSteps = 4096;
    while (pos < alloc && steps < kMaxSteps) {
        BaseObject* o = from_region_addr(pos);
        if (!Collector::PlausibleManagedObjectGate("f3why2-count", o)) {
            break;
        }
        size_t sz = o->GetSize();
        if (sz == 0) {
            break;
        }
        ++validOut;
        if (region->IsMarkedObject(o)) {
            ++markedOut;
        }
        pos += sz;
        ++steps;
    }
}

void NoteCollectEnter(RegionInfo* region)
{
    if (!GateOn()) {
        return;
    }
    EnsureAtexit();
    size_t n = g_enter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (region == nullptr) {
        return;
    }
    (void)InsertRegion(region);

    const bool knownEmpty = region->IsKnownEmpty();
    const bool auth = region->IsLiveCountAuthoritative();
    const uint64_t liveBytes = region->GetLiveByteCount();
    const bool young = region->IsYoungRegion();
    const bool large = region->IsLargeRegion();
    const auto route = region->GetRouteState();
    RegionBitmap* mb = region->GetMarkBitmap();
    const bool neverExamined = knownEmpty && mb == nullptr && region->GetRegionAllocPtr() > region->GetRegionStart();

    if (knownEmpty) {
        g_clsKnownEmpty.fetch_add(1, std::memory_order_relaxed);
    }
    if (auth && liveBytes == 0) {
        g_clsAuthZero.fetch_add(1, std::memory_order_relaxed);
    } else if (auth && liveBytes > 0) {
        g_clsAuthLivePositive.fetch_add(1, std::memory_order_relaxed);
    } else if (!auth) {
        g_clsNoAuth.fetch_add(1, std::memory_order_relaxed);
    }
    if (neverExamined) {
        g_clsNeverExamined.fetch_add(1, std::memory_order_relaxed);
    }
    if (mb != nullptr) {
        g_clsHasBitmap.fetch_add(1, std::memory_order_relaxed);
    }
    if (young) {
        g_clsYoung.fetch_add(1, std::memory_order_relaxed);
    }
    if (large) {
        g_clsLarge.fetch_add(1, std::memory_order_relaxed);
    }
    if (route == RegionInfo::RouteState::FORWARDED) {
        g_clsRouteForwarded.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_clsRouteOther.fetch_add(1, std::memory_order_relaxed);
    }

    size_t validObjs = 0;
    size_t markedObjs = 0;
    if (knownEmpty || n <= kSampleCap) {
        CountMarks(region, validObjs, markedObjs);
        g_sumValidObjs.fetch_add(validObjs, std::memory_order_relaxed);
        g_sumMarkedObjs.fetch_add(markedObjs, std::memory_order_relaxed);
        if (markedObjs > 0) {
            g_regionsWithMarkGt0.fetch_add(1, std::memory_order_relaxed);
            if (knownEmpty) {
                g_clsKnownEmptyMarked.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    size_t slog = g_sampleLogged.fetch_add(1, std::memory_order_relaxed);
    if (slog < kSampleCap) {
        std::fprintf(stderr,
                     "[GCV2][f3why2][collect-enter] n=%zu region=%p start=%#zx alloc=%#zx type=%u route=%u "
                     "young=%u large=%u live=%llu auth=%u knownEmpty=%u neverExamined=%u bitmap=%p "
                     "validObjs=%zu markedObjs=%zu gc=%zu\n",
                     n, region, region->GetRegionStart(), region->GetRegionAllocPtr(),
                     static_cast<unsigned>(region->GetRegionType()), static_cast<unsigned>(route),
                     static_cast<unsigned>(young), static_cast<unsigned>(large),
                     static_cast<unsigned long long>(liveBytes), static_cast<unsigned>(auth),
                     static_cast<unsigned>(knownEmpty), static_cast<unsigned>(neverExamined), mb, validObjs, markedObjs,
                     g_gcCount);
        std::fflush(stderr);
    }
}

void NoteF3RegionGarbage(RegionInfo* latestRegion, BaseObject* latest)
{
    if (!GateOn()) {
        return;
    }
    EnsureAtexit();
    size_t n = g_f3RgHits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (latestRegion == nullptr) {
        g_f3RgJoinMiss.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool join = ContainsRegion(latestRegion);
    if (join) {
        g_f3RgJoinHit.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_f3RgJoinMiss.fetch_add(1, std::memory_order_relaxed);
    }

    const bool knownEmpty = latestRegion->IsKnownEmpty();
    const bool auth = latestRegion->IsLiveCountAuthoritative();
    const uint64_t liveBytes = latestRegion->GetLiveByteCount();
    if (knownEmpty) {
        g_f3RgKnownEmpty.fetch_add(1, std::memory_order_relaxed);
    }
    if (auth) {
        g_f3RgAuth.fetch_add(1, std::memory_order_relaxed);
    }
    g_f3RgLiveBytesSum.fetch_add(static_cast<size_t>(liveBytes & 0xffffffffu), std::memory_order_relaxed);

    size_t validObjs = 0;
    size_t markedObjs = 0;
    bool doCount = (n <= kSampleCap) || ((n & 63u) == 0);
    if (doCount) {
        CountMarks(latestRegion, validObjs, markedObjs);
        if (markedObjs > 0) {
            g_f3RgMark1.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_f3RgMark0.fetch_add(1, std::memory_order_relaxed);
        }
    }

    int objMarked = -1;
    if (latest != nullptr && Heap::IsHeapAddress(latest)) {
        objMarked = latestRegion->IsMarkedObject(latest) ? 1 : 0;
    }

    size_t slog = g_f3RgSampleLogged.fetch_add(1, std::memory_order_relaxed);
    if (slog < kSampleCap) {
        std::fprintf(stderr,
                     "[GCV2][f3why2][f3-join] n=%zu region=%p latest=%p join=%u type=%u route=%u "
                     "live=%llu auth=%u knownEmpty=%u bitmap=%p validObjs=%zu markedObjs=%zu "
                     "objMarked=%d gc=%zu\n",
                     n, latestRegion, latest, static_cast<unsigned>(join),
                     static_cast<unsigned>(latestRegion->GetRegionType()),
                     static_cast<unsigned>(latestRegion->GetRouteState()),
                     static_cast<unsigned long long>(liveBytes), static_cast<unsigned>(auth),
                     static_cast<unsigned>(knownEmpty), latestRegion->GetMarkBitmap(), validObjs, markedObjs, objMarked,
                     g_gcCount);
        std::fflush(stderr);
    }
}

void NoteForwardOrder(RegionInfo* region, uint64_t liveBefore, size_t markedBefore, uint64_t liveAfterReset,
                      size_t markedAfterReset, size_t markedAfterInvalidate)
{
    if (!GateOn()) {
        return;
    }
    EnsureAtexit();
    size_t n = g_orderSamples.fetch_add(1, std::memory_order_relaxed) + 1;
    if (liveBefore > 0) {
        g_orderLivePosBefore.fetch_add(1, std::memory_order_relaxed);
    }
    if (markedBefore > 0) {
        g_orderMarkedPosBefore.fetch_add(1, std::memory_order_relaxed);
    }
    if (liveAfterReset == 0) {
        g_orderLive0AfterReset.fetch_add(1, std::memory_order_relaxed);
    }
    if (markedAfterReset > 0) {
        g_orderMarkedPosAfterReset.fetch_add(1, std::memory_order_relaxed);
    }
    if (markedAfterInvalidate == 0) {
        g_orderMarked0AfterInv.fetch_add(1, std::memory_order_relaxed);
    }
    // 清早了: had live during forward, reset to 0, marks still readable after reset
    if (liveBefore > 0 && liveAfterReset == 0 && markedAfterReset > 0) {
        g_orderClearedEarly.fetch_add(1, std::memory_order_relaxed);
    }
    // 根本没同源: already live==0 before reset but marks present
    if (liveBefore == 0 && markedBefore > 0) {
        g_orderNeverHomologous.fetch_add(1, std::memory_order_relaxed);
    }

    size_t slog = g_orderSampleLogged.fetch_add(1, std::memory_order_relaxed);
    if (slog < kSampleCap) {
        std::fprintf(stderr,
                     "[GCV2][livesame][order] n=%zu region=%p liveBefore=%llu markedBefore=%zu "
                     "liveAfterReset=%llu markedAfterReset=%zu markedAfterInv=%zu young=%u\n",
                     n, region, static_cast<unsigned long long>(liveBefore), markedBefore,
                     static_cast<unsigned long long>(liveAfterReset), markedAfterReset, markedAfterInvalidate,
                     region != nullptr ? static_cast<unsigned>(region->IsYoungRegion()) : 0u);
        std::fflush(stderr);
    }
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    const size_t enter = g_enter.load(std::memory_order_relaxed);
    const size_t knownEmpty = g_clsKnownEmpty.load(std::memory_order_relaxed);
    const size_t knownEmptyMarked = g_clsKnownEmptyMarked.load(std::memory_order_relaxed);
    const size_t authLivePos = g_clsAuthLivePositive.load(std::memory_order_relaxed);
    const size_t authZero = g_clsAuthZero.load(std::memory_order_relaxed);
    const size_t noAuth = g_clsNoAuth.load(std::memory_order_relaxed);
    const size_t neverExam = g_clsNeverExamined.load(std::memory_order_relaxed);
    const size_t hasBm = g_clsHasBitmap.load(std::memory_order_relaxed);
    const size_t young = g_clsYoung.load(std::memory_order_relaxed);
    const size_t large = g_clsLarge.load(std::memory_order_relaxed);
    const size_t routeFwd = g_clsRouteForwarded.load(std::memory_order_relaxed);
    const size_t routeOther = g_clsRouteOther.load(std::memory_order_relaxed);
    const size_t sumValid = g_sumValidObjs.load(std::memory_order_relaxed);
    const size_t sumMarked = g_sumMarkedObjs.load(std::memory_order_relaxed);
    const size_t regMarkGt0 = g_regionsWithMarkGt0.load(std::memory_order_relaxed);
    const size_t setSize = g_collectSetSize.load(std::memory_order_relaxed);
    const size_t setSat = g_collectSetSat.load(std::memory_order_relaxed);
    const size_t f3Hits = g_f3RgHits.load(std::memory_order_relaxed);
    const size_t f3Join = g_f3RgJoinHit.load(std::memory_order_relaxed);
    const size_t f3Miss = g_f3RgJoinMiss.load(std::memory_order_relaxed);
    const size_t f3Mark1 = g_f3RgMark1.load(std::memory_order_relaxed);
    const size_t f3Mark0 = g_f3RgMark0.load(std::memory_order_relaxed);
    const size_t f3Ke = g_f3RgKnownEmpty.load(std::memory_order_relaxed);
    const size_t f3Auth = g_f3RgAuth.load(std::memory_order_relaxed);

    const size_t ordN = g_orderSamples.load(std::memory_order_relaxed);
    const size_t ordLivePos = g_orderLivePosBefore.load(std::memory_order_relaxed);
    const size_t ordMkPos = g_orderMarkedPosBefore.load(std::memory_order_relaxed);
    const size_t ordLive0 = g_orderLive0AfterReset.load(std::memory_order_relaxed);
    const size_t ordMkAfter = g_orderMarkedPosAfterReset.load(std::memory_order_relaxed);
    const size_t ordMk0Inv = g_orderMarked0AfterInv.load(std::memory_order_relaxed);
    const size_t ordEarly = g_orderClearedEarly.load(std::memory_order_relaxed);
    const size_t ordNever = g_orderNeverHomologous.load(std::memory_order_relaxed);

    std::fprintf(stderr,
                 "[GCV2][f3why2] point=%s enter=%zu knownEmpty=%zu knownEmpty_marked=%zu "
                 "auth_live_pos=%zu auth_zero=%zu no_auth=%zu never_examined=%zu has_bitmap=%zu "
                 "young=%zu large=%zu route_fwd=%zu route_other=%zu "
                 "sum_valid=%zu sum_marked=%zu regions_mark_gt0=%zu "
                 "collect_set=%zu collect_sat=%zu "
                 "f3_rg=%zu f3_join_hit=%zu f3_join_miss=%zu f3_mark1=%zu f3_mark0=%zu "
                 "f3_knownEmpty=%zu f3_auth=%zu\n",
                 point != nullptr ? point : "?", enter, knownEmpty, knownEmptyMarked, authLivePos, authZero, noAuth,
                 neverExam, hasBm, young, large, routeFwd, routeOther, sumValid, sumMarked, regMarkGt0, setSize, setSat,
                 f3Hits, f3Join, f3Miss, f3Mark1, f3Mark0, f3Ke, f3Auth);
    std::fprintf(stderr,
                 "[GCV2][livesame][order] point=%s samples=%zu livePosBefore=%zu markedPosBefore=%zu "
                 "live0AfterReset=%zu markedPosAfterReset=%zu marked0AfterInv=%zu "
                 "cleared_early=%zu never_homologous=%zu\n",
                 point != nullptr ? point : "?", ordN, ordLivePos, ordMkPos, ordLive0, ordMkAfter, ordMk0Inv, ordEarly,
                 ordNever);
    std::fflush(stderr);
}

} // namespace F3Why2Diag
} // namespace MapleRuntime
