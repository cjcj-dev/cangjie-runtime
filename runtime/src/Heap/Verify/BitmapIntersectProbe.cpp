// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/BitmapIntersectProbe.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/LogFile.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

// Count 8B granules where both bitmaps have the object-start bit set (IsMarked).
// Uses word-level AND then popcount of bits that are set in both — object-start bits
// only matter for H1b (double-count of the same object). Full multi-bit object masks
// would over-count object body bits; we only test the head bit of each 8B granule.
size_t CountIntersectGranules(RegionBitmap* mark, RegionBitmap* resurrect)
{
    if (mark == nullptr || resurrect == nullptr) {
        return 0;
    }
    size_t count = mark->wordCnt.load(std::memory_order_acquire);
    size_t rcount = resurrect->wordCnt.load(std::memory_order_acquire);
    size_t n = count < rcount ? count : rcount;
    size_t granules = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t both = mark->markWords[i].load(std::memory_order_acquire) &
            resurrect->markWords[i].load(std::memory_order_acquire);
        granules += static_cast<size_t>(__builtin_popcountll(both));
    }
    return granules;
}

size_t CountMarkedGranules(RegionBitmap* bm)
{
    if (bm == nullptr) {
        return 0;
    }
    size_t n = bm->wordCnt.load(std::memory_order_acquire);
    size_t granules = 0;
    for (size_t i = 0; i < n; ++i) {
        granules += static_cast<size_t>(__builtin_popcountll(bm->markWords[i].load(std::memory_order_acquire)));
    }
    return granules;
}

} // namespace

bool BitmapIntersectProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_BITMAP_INTERSECT");
    return on;
}

size_t BitmapIntersectProbe::ScanAfterResurrection(RegionManager& manager, size_t resurrectedObjectsReported)
{
    if (!Enabled()) {
        return 0;
    }

    size_t regionsScanned = 0;
    size_t regionsWithBoth = 0;
    size_t regionsWithResurrect = 0;
    size_t regionsWithMark = 0;
    size_t intersectGranules = 0;
    size_t markGranules = 0;
    size_t resurrectGranules = 0;
    size_t largeBoth = 0;

    // Same address walk as ForEachObjUnsafe — covers every live region unit.
    uintptr_t heapStart = manager.GetRegionHeapStart();
    uintptr_t inactive = manager.GetInactiveZone();
    for (uintptr_t regionAddr = heapStart; regionAddr < inactive;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        uintptr_t nextAddr = region->GetRegionEnd();
        if (nextAddr <= regionAddr || nextAddr > inactive) {
            regionAddr += RegionInfo::UNIT_SIZE;
            continue;
        }
        regionAddr = nextAddr;
        if (!region->IsValidRegion() || region->IsFreeRegion() || region->IsGarbageRegion()) {
            continue;
        }
        ++regionsScanned;

        if (region->IsLargeRegion()) {
            // Large regions use flags, not bitmaps.
            bool m = region->IsMarkedObject(static_cast<size_t>(0));
            bool r = region->IsResurrectedObject(static_cast<size_t>(0));
            if (m) {
                ++regionsWithMark;
            }
            if (r) {
                ++regionsWithResurrect;
            }
            if (m && r) {
                ++largeBoth;
                ++intersectGranules;
            }
            continue;
        }

        RegionBitmap* mark = region->GetMarkBitmap();
        RegionBitmap* resurrect = region->GetResurrectBitmap();
        if (mark != nullptr) {
            ++regionsWithMark;
            markGranules += CountMarkedGranules(mark);
        }
        if (resurrect != nullptr) {
            ++regionsWithResurrect;
            resurrectGranules += CountMarkedGranules(resurrect);
        }
        if (mark != nullptr && resurrect != nullptr) {
            ++regionsWithBoth;
            intersectGranules += CountIntersectGranules(mark, resurrect);
        }
    }

    VLOG(REPORT,
         "[GCV2][bitmap-intersect] SUMMARY resurrectedObjectsReported=%zu "
         "regionsScanned=%zu regionsWithMark=%zu regionsWithResurrect=%zu regionsWithBoth=%zu "
         "markGranules=%zu resurrectGranules=%zu intersectGranules=%zu largeBoth=%zu "
         "env=MRT_GCV2_BITMAP_INTERSECT=1",
         resurrectedObjectsReported, regionsScanned, regionsWithMark, regionsWithResurrect, regionsWithBoth,
         markGranules, resurrectGranules, intersectGranules, largeBoth);

    return intersectGranules;
}

} // namespace MapleRuntime
