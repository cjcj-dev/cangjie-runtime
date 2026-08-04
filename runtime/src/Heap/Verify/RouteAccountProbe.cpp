// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RouteAccountProbe.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/Panic.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/LiveInfo.h"

namespace MapleRuntime {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

std::atomic<uint64_t> gRouteElseN{0};
std::atomic<uint64_t> gRouteElseMaxAbs{0};
std::atomic<int64_t> gRouteElseSumAbs{0};
std::atomic<uint64_t> gRouteElseNonZero{0};
std::atomic<uint64_t> gRouteElseSizeLike{0}; // |delta| in {8,16,24,32,40,48,64,80,96}

std::atomic<uint64_t> gReserveN{0};
std::atomic<uint64_t> gReserveMaxAbs{0};
std::atomic<int64_t> gReserveSumAbs{0};
std::atomic<uint64_t> gReserveNonZero{0};
std::atomic<uint64_t> gReserveSizeLike{0};
std::atomic<uint64_t> gReserveBitmapLt{0}; // bitmap < counter → hole
std::atomic<uint64_t> gReserveBitmapGt{0}; // bitmap > counter → over-reserve / CHECK risk

std::atomic<uint64_t> gMarkNullLiveInfo{0};
std::atomic<uint64_t> gMarkNullBitmap{0};
std::atomic<uint64_t> gMarkTempLiveInfo{0};
std::atomic<uint64_t> gMarkTempBitmap{0};
std::atomic<uint64_t> gMarkOk{0};
std::atomic<uint64_t> gMarkArmed{0};

bool SizeLike(uint64_t absDelta)
{
    switch (absDelta) {
        case 8:
        case 16:
        case 24:
        case 32:
        case 40:
        case 48:
        case 64:
        case 80:
        case 96:
            return true;
        default:
            return false;
    }
}

void NoteAbs(std::atomic<uint64_t>& n, std::atomic<uint64_t>& maxAbs, std::atomic<int64_t>& sumAbs,
             std::atomic<uint64_t>& nonZero, std::atomic<uint64_t>& sizeLike, int64_t delta)
{
    n.fetch_add(1, std::memory_order_relaxed);
    uint64_t ad = static_cast<uint64_t>(delta < 0 ? -delta : delta);
    sumAbs.fetch_add(static_cast<int64_t>(ad), std::memory_order_relaxed);
    if (ad != 0) {
        nonZero.fetch_add(1, std::memory_order_relaxed);
        if (SizeLike(ad)) {
            sizeLike.fetch_add(1, std::memory_order_relaxed);
        }
    }
    uint64_t prev = maxAbs.load(std::memory_order_relaxed);
    while (ad > prev && !maxAbs.compare_exchange_weak(prev, ad, std::memory_order_relaxed)) {
    }
}

void DumpSummaryIfNeeded()
{
    // Periodic summary every 256 reserve samples so long ALOT runs leave a trail before crash.
    uint64_t n = gReserveN.load(std::memory_order_relaxed);
    if (n == 0 || (n & 0xff) != 0) {
        return;
    }
    uint64_t elseN = gRouteElseN.load(std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][route-account] SUMMARY reserve_n=%llu reserve_nonzero=%llu reserve_max_abs=%llu "
         "reserve_bitmap_lt=%llu reserve_bitmap_gt=%llu reserve_size_like=%llu "
         "else_n=%llu else_nonzero=%llu else_max_abs=%llu mark_ok=%llu mark_null_li=%llu "
         "mark_null_bm=%llu mark_temp_li=%llu mark_temp_bm=%llu",
         static_cast<unsigned long long>(n),
         static_cast<unsigned long long>(gReserveNonZero.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gReserveMaxAbs.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gReserveBitmapLt.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gReserveBitmapGt.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gReserveSizeLike.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(elseN),
         static_cast<unsigned long long>(gRouteElseNonZero.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gRouteElseMaxAbs.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gMarkOk.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gMarkNullLiveInfo.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gMarkNullBitmap.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gMarkTempLiveInfo.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gMarkTempBitmap.load(std::memory_order_relaxed)));
}

} // namespace

bool RouteAccountProbe::AccountEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_ROUTE_ACCOUNT");
    return on;
}

bool RouteAccountProbe::MarkBitmapNullEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_MARK_BITMAP_NULL");
    return on;
}

void RouteAccountProbe::NoteGetRouteElse(uint64_t preLiveBytes, uint32_t toRegion1UsedBytes, RegionInfo* fromRegion)
{
    if (!AccountEnabled()) {
        return;
    }
    int64_t delta = static_cast<int64_t>(preLiveBytes) - static_cast<int64_t>(toRegion1UsedBytes);
    NoteAbs(gRouteElseN, gRouteElseMaxAbs, gRouteElseSumAbs, gRouteElseNonZero, gRouteElseSizeLike, delta);

    size_t fromCounter = 0;
    size_t bitmapLive = 0;
    size_t bitmapRecomp = 0;
    if (fromRegion != nullptr) {
        fromCounter = fromRegion->GetLiveByteCount();
        LiveInfo* li = fromRegion->GetLiveInfo();
        if (li == nullptr && fromRegion->GetRetainedLiveInfo() != nullptr) {
            li = fromRegion->GetRetainedLiveInfo();
        }
        // Ghost route uses liveInfo0; prefer that when present via GetPreLiveBytes path.
        // GetBitmapLiveBytes on active liveInfo is still the dual of the counter for this region.
        if (li != nullptr) {
            bitmapLive = li->GetBitmapLiveBytes();
            bitmapRecomp = li->RecomputeBitmapLiveBytes();
        }
    }

    static std::atomic<uint64_t> dumpLeft{64};
    uint64_t left = dumpLeft.load(std::memory_order_relaxed);
    if (left > 0 && dumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
        VLOG(REPORT,
             "[GCV2][route-account] GET_ROUTE_ELSE preLive=%llu to1Used=%llu delta=%lld "
             "fromCounter=%zu bitmapLive=%zu bitmapRecomp=%zu region=%p",
             static_cast<unsigned long long>(preLiveBytes),
             static_cast<unsigned long long>(toRegion1UsedBytes), static_cast<long long>(delta), fromCounter,
             bitmapLive, bitmapRecomp, static_cast<void*>(fromRegion));
    }
    if (EnvIsOne("MRT_GCV2_ROUTE_ACCOUNT_FATAL") && preLiveBytes >= toRegion1UsedBytes &&
        toRegion1UsedBytes == 0) {
        // soft: only dump; CHECK for toRegion2 is product path
    }
    DumpSummaryIfNeeded();
}

void RouteAccountProbe::NoteRouteReserve(RegionInfo* fromRegion, size_t fromBytesCounter, size_t usedBytes1,
                                         size_t usedBytes2, bool twoRegion)
{
    if (!AccountEnabled()) {
        return;
    }
    size_t bitmapLive = 0;
    size_t bitmapRecomp = 0;
    LiveInfo* li = nullptr;
    if (fromRegion != nullptr) {
        li = fromRegion->GetLiveInfo();
        if (li == nullptr) {
            // After PrepareForwardableRegion, active liveInfo may still be set; ghost uses liveInfo0.
            // GetLiveInfo is the mark-period pointer used to build the bitmaps that feed GetPreLiveBytes.
            li = fromRegion->GetLiveInfo();
        }
        if (li != nullptr) {
            bitmapLive = li->GetBitmapLiveBytes();
            bitmapRecomp = li->RecomputeBitmapLiveBytes();
        }
    }
    int64_t delta = static_cast<int64_t>(bitmapLive) - static_cast<int64_t>(fromBytesCounter);
    NoteAbs(gReserveN, gReserveMaxAbs, gReserveSumAbs, gReserveNonZero, gReserveSizeLike, delta);
    if (bitmapLive < fromBytesCounter) {
        gReserveBitmapLt.fetch_add(1, std::memory_order_relaxed);
    } else if (bitmapLive > fromBytesCounter) {
        gReserveBitmapGt.fetch_add(1, std::memory_order_relaxed);
    }

    static std::atomic<bool> armedLogged{false};
    if (!armedLogged.exchange(true, std::memory_order_relaxed)) {
        VLOG(REPORT,
             "[GCV2][route-account] ARMED env=MRT_GCV2_ROUTE_ACCOUNT=1 first_reserve "
             "fromCounter=%zu bitmapLive=%zu bitmapRecomp=%zu used1=%zu used2=%zu two=%u region=%p",
             fromBytesCounter, bitmapLive, bitmapRecomp, usedBytes1, usedBytes2, static_cast<unsigned>(twoRegion),
             static_cast<void*>(fromRegion));
    }

    static std::atomic<uint64_t> dumpLeft{64};
    bool interesting = (bitmapLive != fromBytesCounter) || (bitmapRecomp != bitmapLive);
    uint64_t left = dumpLeft.load(std::memory_order_relaxed);
    if (interesting && left > 0 && dumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
        VLOG(REPORT,
             "[GCV2][route-account] RESERVE_DELTA fromCounter=%zu bitmapLive=%zu bitmapRecomp=%zu "
             "delta=%lld used1=%zu used2=%zu two=%u region=%p",
             fromBytesCounter, bitmapLive, bitmapRecomp, static_cast<long long>(delta), usedBytes1, usedBytes2,
             static_cast<unsigned>(twoRegion), static_cast<void*>(fromRegion));
    }

    if (EnvIsOne("MRT_GCV2_ROUTE_ACCOUNT_FATAL") && bitmapLive != fromBytesCounter) {
        CHECK_DETAIL(false,
                     "MRT_GCV2_ROUTE_ACCOUNT_FATAL: bitmapLive=%zu fromCounter=%zu bitmapRecomp=%zu region=%p",
                     bitmapLive, fromBytesCounter, bitmapRecomp, static_cast<void*>(fromRegion));
    }
    DumpSummaryIfNeeded();
}

bool RouteAccountProbe::NoteMarkBitmapCheck(RegionInfo* region, size_t offset, const char* site)
{
    if (!MarkBitmapNullEnabled() || region == nullptr) {
        return true;
    }
    static std::atomic<bool> armedLogged{false};
    if (!armedLogged.exchange(true, std::memory_order_relaxed)) {
        gMarkArmed.store(1, std::memory_order_relaxed);
        VLOG(REPORT, "[GCV2][mark-bitmap-null] ARMED env=MRT_GCV2_MARK_BITMAP_NULL=1 site=%s", site);
    }

    LiveInfo* liveInfo = __atomic_load_n(&region->GetLiveInfoForProbe(), std::memory_order_acquire);
    // Fallback: use public GetLiveInfo (treats TEMPORARY as null).
    LiveInfo* pub = region->GetLiveInfo();
    if (pub == nullptr) {
        // Distinguish TEMPORARY vs true null via raw load helper if available.
        gMarkNullLiveInfo.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint64_t> dumpLeft{32};
        uint64_t left = dumpLeft.load(std::memory_order_relaxed);
        if (left > 0 && dumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
            VLOG(REPORT,
                 "[GCV2][mark-bitmap-null] NULL_LIVEINFO site=%s offset=%zu region=%p type=%u start=%#zx",
                 site, offset, static_cast<void*>(region), static_cast<unsigned>(region->GetRegionType()),
                 region->GetRegionStart());
        }
        return false;
    }
    (void)liveInfo;
    RegionBitmap* bitmap = __atomic_load_n(&pub->markBitmap, std::memory_order_acquire);
    if (reinterpret_cast<MAddress>(bitmap) == LiveInfo::TEMPORARY_PTR) {
        gMarkTempBitmap.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint64_t> dumpLeft{32};
        uint64_t left = dumpLeft.load(std::memory_order_relaxed);
        if (left > 0 && dumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
            VLOG(REPORT,
                 "[GCV2][mark-bitmap-null] TEMP_BITMAP site=%s offset=%zu region=%p liveInfo=%p",
                 site, offset, static_cast<void*>(region), static_cast<void*>(pub));
        }
        return false;
    }
    if (bitmap == nullptr) {
        gMarkNullBitmap.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint64_t> dumpLeft{32};
        uint64_t left = dumpLeft.load(std::memory_order_relaxed);
        if (left > 0 && dumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
            VLOG(REPORT,
                 "[GCV2][mark-bitmap-null] NULL_BITMAP site=%s offset=%zu region=%p liveInfo=%p",
                 site, offset, static_cast<void*>(region), static_cast<void*>(pub));
        }
        return false;
    }
    gMarkOk.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace MapleRuntime
