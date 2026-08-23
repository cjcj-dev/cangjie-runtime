// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TagReuseProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace {

// Always-on stderr so evidence survives when VLOG(REPORT) is gated off (DEFAULT_MRT_REPORT=0).
#define TAGREUSE_LOG(fmt, ...)                                                                                         \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][tag-reuse] " fmt "\n", ##__VA_ARGS__);                                            \
        std::fflush(stderr);                                                                                           \
    } while (0)
#define STICKY_LOG(fmt, ...)                                                                                           \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][mark-bits-sticky] " fmt "\n", ##__VA_ARGS__);                                      \
        std::fflush(stderr);                                                                                           \
    } while (0)

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

bool InRange(uintptr_t p, uintptr_t start, size_t size)
{
    if (p == 0 || size == 0) {
        return false;
    }
    return p >= start && p < (start + size);
}

std::atomic<uint64_t> gReleaseScanN{0};
std::atomic<uint64_t> gDanglingLiveInfo{0};
std::atomic<uint64_t> gDanglingLiveInfo0{0};
std::atomic<uint64_t> gDanglingRetained{0};
std::atomic<uint64_t> gDanglingTotal{0};
std::atomic<uint64_t> gPositiveBoundStill{0};
std::atomic<uint64_t> gPositiveZoneSlots{0};
std::atomic<uint64_t> gPositiveInRangeHits{0};
std::atomic<uint64_t> gMarkStickyN{0};
std::atomic<uint64_t> gMarkStickyFail{0};
std::atomic<uint64_t> gMarkStickyOk{0};

void NoteDangling(const char* kind, RegionInfo* region, const char* listName, uintptr_t ptr, uintptr_t rangeStart,
                  size_t rangeSize, bool young, unsigned type, bool inCandidateGuess)
{
    gDanglingTotal.fetch_add(1, std::memory_order_relaxed);
    if (std::strcmp(kind, "liveInfo") == 0) {
        gDanglingLiveInfo.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(kind, "liveInfo0") == 0) {
        gDanglingLiveInfo0.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(kind, "retained") == 0) {
        gDanglingRetained.fetch_add(1, std::memory_order_relaxed);
    }
    static std::atomic<uint64_t> dumpLeft{128};
    uint64_t left = dumpLeft.load(std::memory_order_relaxed);
    if (left == 0 || !dumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
        return;
    }
    ptrdiff_t off = static_cast<ptrdiff_t>(ptr - rangeStart);
    TAGREUSE_LOG("DANGLING kind=%s region=%p list=%s type=%u young=%u candGuess=%u "
                 "livePtr=%#zx range=[%#zx,+%zu) offset=%td regionStart=%#zx",
                 kind, static_cast<void*>(region), listName, type, young ? 1u : 0u, inCandidateGuess ? 1u : 0u, ptr,
                 rangeStart, rangeSize, off, region->GetRegionStart());
}

} // namespace

bool TagReuseProbe::TagReuseEnabled()
{
    static const bool on = false /* pinned:MRT_GCV2_TAG_REUSE */;
    return on;
}

bool TagReuseProbe::MarkBitsStickyEnabled()
{
    static const bool on = false /* pinned:MRT_GCV2_MARK_BITS_STICKY */;
    return on;
}

void TagReuseProbe::ScanBeforeRelease(uintptr_t rangeStart, size_t rangeSize, uint16_t previousTagId,
                                      uintptr_t liveInfoZoneStart, uintptr_t liveInfoZonePos,
                                      uintptr_t bitmapZoneStart, uintptr_t bitmapZonePos)
{
    if (!TagReuseEnabled()) {
        return;
    }
    (void)bitmapZoneStart;
    (void)bitmapZonePos;
    uint64_t n = gReleaseScanN.fetch_add(1, std::memory_order_relaxed) + 1;

    static std::atomic<bool> armedLogged{false};
    if (!armedLogged.exchange(true, std::memory_order_relaxed)) {
        TAGREUSE_LOG("ARMED env=MRT_GCV2_TAG_REUSE=1 range=[%#zx,+%zu) prevTag=%u "
                     "liveZone=[%#zx,%#zx) bitmapZone=[%#zx,%#zx)",
                     rangeStart, rangeSize, static_cast<unsigned>(previousTagId), liveInfoZoneStart, liveInfoZonePos,
                     bitmapZoneStart, bitmapZonePos);
    }

    size_t zoneSlots = 0;
    size_t stillBound = 0;
    size_t inRangeHits = 0;
    if (liveInfoZonePos > liveInfoZoneStart) {
        for (uintptr_t cur = liveInfoZoneStart; cur + sizeof(LiveInfo) <= liveInfoZonePos; cur += sizeof(LiveInfo)) {
            ++zoneSlots;
            if (!InRange(cur, rangeStart, rangeSize)) {
                continue;
            }
            ++inRangeHits;
            LiveInfo* slot = reinterpret_cast<LiveInfo*>(cur);
            RegionInfo* binded = slot->bindedRegion;
            if (binded == nullptr) {
                continue;
            }
            LiveInfo* pub = binded->GetLiveInfo();
            LiveInfo* li0 = binded->GetLiveInfo0ForProbe();
            LiveInfo* ret = binded->GetRetainedLiveInfo();
            if (pub == slot || li0 == slot || ret == slot) {
                ++stillBound;
                static std::atomic<uint64_t> posDump{32};
                uint64_t left = posDump.load(std::memory_order_relaxed);
                if (left > 0 && posDump.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
                    TAGREUSE_LOG("POSITIVE_BOUND liveInfo=%#zx binded=%p pub=%p li0=%p ret=%p type=%u young=%u", cur,
                                 static_cast<void*>(binded), static_cast<void*>(pub), static_cast<void*>(li0),
                                 static_cast<void*>(ret), static_cast<unsigned>(binded->GetRegionType()),
                                 binded->IsYoungRegion() ? 1u : 0u);
                }
            }
        }
    }
    gPositiveZoneSlots.store(zoneSlots, std::memory_order_relaxed);
    gPositiveBoundStill.fetch_add(stillBound, std::memory_order_relaxed);
    gPositiveInRangeHits.fetch_add(inRangeHits, std::memory_order_relaxed);

    if (zoneSlots > 0 && inRangeHits != zoneSlots) {
        TAGREUSE_LOG("RANGE_MISMATCH zoneSlots=%zu inRangeHits=%zu range=[%#zx,+%zu) liveZone=[%#zx,%#zx)", zoneSlots,
                     inRangeHits, rangeStart, rangeSize, liveInfoZoneStart, liveInfoZonePos);
    } else if (zoneSlots > 0 && n <= 3) {
        TAGREUSE_LOG("POSITIVE_RANGE ok zoneSlots=%zu inRangeHits=%zu stillBound=%zu range=[%#zx,+%zu) prevTag=%u "
                     "scanN=%llu",
                     zoneSlots, inRangeHits, stillBound, rangeStart, rangeSize, static_cast<unsigned>(previousTagId),
                     static_cast<unsigned long long>(n));
    } else if (zoneSlots == 0 && n <= 3) {
        TAGREUSE_LOG("POSITIVE_RANGE empty_zone range=[%#zx,+%zu) prevTag=%u scanN=%llu "
                     "(release of unused previous tag is expected before first major flip)",
                     rangeStart, rangeSize, static_cast<unsigned>(previousTagId), static_cast<unsigned long long>(n));
    }

    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& manager = space.GetRegionManager();
    manager.VisitAllManagedRegionsForProbe([&](RegionInfo* region, const char* listName) {
        if (region == nullptr) {
            return;
        }
        LiveInfo* li = region->GetLiveInfo();
        uintptr_t lip = reinterpret_cast<uintptr_t>(li);
        if (InRange(lip, rangeStart, rangeSize)) {
            bool young = region->IsYoungRegion();
            bool candGuess = young && (region->IsFromRegion() || region->IsUnmovableFromRegion() ||
                                       region->GetRegionType() == RegionInfo::RegionType::RECENT_FULL_REGION);
            NoteDangling("liveInfo", region, listName, lip, rangeStart, rangeSize, young,
                         static_cast<unsigned>(region->GetRegionType()), candGuess);
        }
        LiveInfo* li0 = region->GetLiveInfo0ForProbe();
        uintptr_t li0p = reinterpret_cast<uintptr_t>(li0);
        if (InRange(li0p, rangeStart, rangeSize)) {
            NoteDangling("liveInfo0", region, listName, li0p, rangeStart, rangeSize, region->IsYoungRegion(),
                         static_cast<unsigned>(region->GetRegionType()), false);
        }
        LiveInfo* ret = region->GetRetainedLiveInfo();
        uintptr_t retp = reinterpret_cast<uintptr_t>(ret);
        if (InRange(retp, rangeStart, rangeSize)) {
            NoteDangling("retained", region, listName, retp, rangeStart, rangeSize, region->IsYoungRegion(),
                         static_cast<unsigned>(region->GetRegionType()), false);
        }
    });

    uint64_t dangling = gDanglingTotal.load(std::memory_order_relaxed);
    if (dangling > 0) {
        TAGREUSE_LOG("DANGLING_LIVEINFO_CONFIRMED_%llu scanN=%llu li=%llu li0=%llu ret=%llu stillBound=%zu "
                     "zoneSlots=%zu range=[%#zx,+%zu) prevTag=%u",
                     static_cast<unsigned long long>(dangling), static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(gDanglingLiveInfo.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(gDanglingLiveInfo0.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(gDanglingRetained.load(std::memory_order_relaxed)), stillBound,
                     zoneSlots, rangeStart, rangeSize, static_cast<unsigned>(previousTagId));
    } else if ((n & 0xff) == 0 || n <= 4) {
        TAGREUSE_LOG("SUMMARY scanN=%llu dangling=0 stillBound=%zu zoneSlots=%zu inRangeHits=%zu range=[%#zx,+%zu) "
                     "prevTag=%u",
                     static_cast<unsigned long long>(n), stillBound, zoneSlots, inRangeHits, rangeStart, rangeSize,
                     static_cast<unsigned>(previousTagId));
    }
}

bool TagReuseProbe::NoteMarkBitsSticky(RegionInfo* region, size_t offset, bool /*expectMarked*/, const char* site)
{
    // Windows ABI compatibility for an old diagnostic-only export. Product call
    // sites use the generation-bearing overload below; the compatibility entry
    // deliberately performs no mark read.
    (void)region;
    (void)offset;
    (void)site;
    return true;
}

bool TagReuseProbe::NoteMarkBitsSticky(RegionInfo* region, size_t offset, bool /*expectMarked*/, const char* site,
                                       Generation generation)
{
    if (!MarkBitsStickyEnabled() || region == nullptr) {
        return true;
    }
    static std::atomic<bool> armedLogged{false};
    if (!armedLogged.exchange(true, std::memory_order_relaxed)) {
        STICKY_LOG("ARMED env=MRT_GCV2_MARK_BITS_STICKY=1 site=%s", site);
    }
    gMarkStickyN.fetch_add(1, std::memory_order_relaxed);
    bool nowMarked = generation == Generation::Young
        ? region->IsMarkedObject(region->GetMarkView<Generation::Young>(), offset)
        : region->IsMarkedObject(region->GetMarkView<Generation::Old>(), offset);
    if (!nowMarked) {
        gMarkStickyFail.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint64_t> dumpLeft{64};
        uint64_t left = dumpLeft.load(std::memory_order_relaxed);
        if (left > 0 && dumpLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
            RegionBitmap* bitmap = generation == Generation::Young
                ? region->GetMarkBitmap(region->GetMarkView<Generation::Young>())
                : region->GetMarkBitmap(region->GetMarkView<Generation::Old>());
            STICKY_LOG("NOT_STICKY site=%s offset=%zu region=%p type=%u liveInfo=%p bitmap=%p", site, offset,
                       static_cast<void*>(region), static_cast<unsigned>(region->GetRegionType()),
                       static_cast<void*>(region->GetLiveInfo()), static_cast<void*>(bitmap));
        }
        return false;
    }
    gMarkStickyOk.fetch_add(1, std::memory_order_relaxed);
    uint64_t n = gMarkStickyN.load(std::memory_order_relaxed);
    if ((n & 0x3ffff) == 0) {
        STICKY_LOG("SUMMARY n=%llu ok=%llu fail=%llu", static_cast<unsigned long long>(n),
                   static_cast<unsigned long long>(gMarkStickyOk.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(gMarkStickyFail.load(std::memory_order_relaxed)));
    }
    return true;
}

} // namespace MapleRuntime
