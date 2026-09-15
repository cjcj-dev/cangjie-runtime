#include "Heap/z/z_globals.hpp"
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_TENURING_THRESHOLD_H
#define MRT_TENURING_THRESHOLD_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "Heap/z/zPageAge.hpp"

namespace MapleRuntime {

// Act on the computed threshold: age < threshold stays young (in-place, no Route to old).
// Copy dest is still old; stay-young is flip-survive (zRelocate.cpp:1346-1352), not per-age to-space.
constexpr bool kPageAgeAdaptiveTenuring = true;
constexpr uint32_t kMaxTenuringThreshold = untype(PageAge::survivor14);

struct TenuringInputs {
    size_t liveByAge[kPageAgeCount]{};
    size_t youngGarbage = 0;
    size_t youngAllocated = 0;
    size_t softMaxCapacity = 0;
    bool promoteAll = false;
};

inline uint32_t ComputeTenuringThreshold(const TenuringInputs& in)
{
    if (in.promoteAll) {
        return 0;
    }

    size_t youngLiveTotal = 0;
    size_t youngLiveLast = 0;
    double lifeExpectancySum = 0.0;
    uint32_t lifeExpectancySamples = 0;
    uint32_t lastPopulatedAge = 0;

    for (PageAge age : kPageAgeRangeAll) {
        const size_t youngLive = in.liveByAge[untype(age)];
        if (youngLive > 0) {
            lastPopulatedAge = untype(age);
            if (youngLiveLast > 0) {
                lifeExpectancySum += static_cast<double>(youngLive) / static_cast<double>(youngLiveLast);
                ++lifeExpectancySamples;
            }
        }
        youngLiveTotal += youngLive;
        youngLiveLast = youngLive;
    }

    if (youngLiveTotal == 0) {
        return 0;
    }

    const double lifeExpectancy =
        lifeExpectancySamples == 0 ? 1.0 : lifeExpectancySum / static_cast<double>(lifeExpectancySamples);
    const double lifeDecayFactor = 1.0 / lifeExpectancy;
    const double residencyReciprocal =
        in.softMaxCapacity == 0 ? 1.0 :
        static_cast<double>(in.softMaxCapacity) / static_cast<double>(youngLiveTotal);
    const double residencyFactor = std::max(residencyReciprocal, 1.0);
    const double allocatedGarbageRatio =
        static_cast<double>(in.youngAllocated) / static_cast<double>(in.youngGarbage + 1);
    const double youngLog = std::max(std::min(allocatedGarbageRatio, 1.0) * 16.0, 2.0);
    const double logResidency = std::log(residencyFactor) / std::log(youngLog);
    const double thresholdRaw = lifeDecayFactor * logResidency;

    const uint32_t upperBound = std::min(lastPopulatedAge + 1u, kMaxTenuringThreshold);
    const uint32_t lowerBound = std::min(1u, upperBound);
    const uint32_t rounded = static_cast<uint32_t>(std::llround(thresholdRaw));
    return std::min(std::max(rounded, lowerBound), upperBound);
}

inline PageAge ComputeToAge(PageAge fromAge, uint32_t tenuringThreshold)
{
    if (fromAge == PageAge::old) {
        return PageAge::old;
    }
    if (untype(fromAge) >= tenuringThreshold) {
        return PageAge::old;
    }
    return to_pageage(untype(fromAge) + 1);
}

inline bool ShouldPromoteAge(uint8_t youngAge, uint32_t tenuringThreshold)
{
    return ComputeToAge(to_pageage(youngAge), tenuringThreshold) == PageAge::old;
}

} // namespace MapleRuntime

#endif

#ifndef MRT_RELOCATION_SET_SELECTOR_H
#define MRT_RELOCATION_SET_SELECTOR_H

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MapleRuntime {

// RegionManager::MAX_UNIT_COUNT_PER_REGION * UNIT_SIZE (128KB) — no medium tier.
inline constexpr size_t kRelocationMaxSmallRegionBytes = 128 * 1024;
// Analog of ZObjectSizeLimitSmall relative to the group max page (1/8).
inline constexpr size_t kRelocationObjectSizeLimit = 16 * 1024;

enum class RelocRegionKind : uint8_t { Small = 0, Large = 1 };

struct RelocRegionDesc {
    size_t liveBytes = 0;
    size_t capacity = 0;
    RelocRegionKind kind = RelocRegionKind::Small;
    uint32_t id = 0;
    // ZGC zGeneration.cpp:211-213: !is_relocatable pages are never registered.
    // is_allocating is the page birth-sequence predicate (zPage.inline.hpp:180-185).
    bool allocating = false;
};

struct RelocSelectResult {
    std::vector<uint32_t> selectedIds;
};

// ZRelocationSetSelectorGroup::pre_filter_page (zRelocationSetSelector.inline.hpp:75-104)
inline bool PreFilterRelocRegion(const RelocRegionDesc& page)
{
    // zGeneration.cpp:211-213: allocating pages are not relocatable candidates.
    if (page.allocating) {
        return false;
    }
    if (page.kind == RelocRegionKind::Large) {
        return false;
    }
    if (page.capacity == 0) {
        return false;
    }
    const size_t garbage = page.capacity > page.liveBytes ? page.capacity - page.liveBytes : 0;
    const size_t pageFragLimit =
        static_cast<size_t>(static_cast<double>(page.capacity) * (kRelocationFragmentationLimitPercent / 100.0));
    return garbage > pageFragLimit;
}

// ZRelocationSetSelectorGroup::partition_index / semi_sort
// (zRelocationSetSelector.cpp:70-112, zRelocationSetSelector.hpp:81-82).
inline constexpr size_t kRelocationNumPartitionsShift = 11;
inline constexpr size_t kRelocationNumPartitions = size_t{1} << kRelocationNumPartitionsShift;

inline size_t RelocationPartitionIndex(const RelocRegionDesc& page)
{
    // Region capacities are system-page multiples, not necessarily powers of two.
    // Division retains ZGC's per-page partition width without requiring log2i_exact.
    const size_t partitionSize = page.capacity >> kRelocationNumPartitionsShift;
    assert(partitionSize != 0);
    const size_t index = page.liveBytes / partitionSize;
    assert(index < kRelocationNumPartitions);
    return index;
}

inline void SemiSortRelocationPages(std::vector<RelocRegionDesc>& pages)
{
    size_t partitions[kRelocationNumPartitions]{};
    for (const RelocRegionDesc& page : pages) {
        ++partitions[RelocationPartitionIndex(page)];
    }

    size_t finger = 0;
    for (size_t& partition : partitions) {
        const size_t slots = partition;
        partition = finger;
        finger += slots;
    }

    std::vector<RelocRegionDesc> sorted(pages.size());
    for (const RelocRegionDesc& page : pages) {
        sorted[partitions[RelocationPartitionIndex(page)]++] = page;
    }
    pages.swap(sorted);
}

// ZRelocationSetSelectorGroup::select_inner (zRelocationSetSelector.cpp:114-196)
inline RelocSelectResult SelectRelocationSet(const std::vector<RelocRegionDesc>& pages)
{
    RelocSelectResult out;
    std::vector<RelocRegionDesc> live;
    live.reserve(pages.size());
    for (const RelocRegionDesc& p : pages) {
        if (PreFilterRelocRegion(p)) {
            live.push_back(p);
        }
    }
    SemiSortRelocationPages(live);

    const int npages = static_cast<int>(live.size());
    int selectedFrom = 0;
    int selectedTo = 0;
    size_t fromLiveBytes = 0;
    const double denom = static_cast<double>(kRelocationMaxSmallRegionBytes - kRelocationObjectSizeLimit);

    for (int from = 1; from <= npages; ++from) {
        fromLiveBytes += live[static_cast<size_t>(from - 1)].liveBytes;
        const int to = static_cast<int>(std::ceil(static_cast<double>(fromLiveBytes) / denom));
        const int diffFrom = from - selectedFrom;
        const int diffTo = to - selectedTo;
        const double percentToOfFrom =
            (diffFrom != 0) ? (static_cast<double>(diffTo) / static_cast<double>(diffFrom) * 100.0) : 0.0;
        const double diffReclaimable = 100.0 - percentToOfFrom;
        if (diffReclaimable > kRelocationFragmentationLimitPercent) {
            selectedFrom = from;
            selectedTo = to;
        }
    }

    out.selectedIds.reserve(static_cast<size_t>(selectedFrom));
    for (int i = 0; i < selectedFrom; ++i) {
        out.selectedIds.push_back(live[static_cast<size_t>(i)].id);
    }
    return out;
}

} // namespace MapleRuntime

#endif
