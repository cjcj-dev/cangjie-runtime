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

#include "Heap/z/zArray.hpp"
#include "Heap/z/zPageAge.hpp"
#include "Heap/z/zPageType.hpp"

namespace MapleRuntime {

class ZPage;

class ZRelocationSetSelectorGroupStats {
    friend class ZRelocationSetSelectorGroup;
private:
    size_t _npages_candidates = 0;
    size_t _total = 0;
    size_t _live = 0;
    size_t _empty = 0;
    size_t _npages_selected = 0;
    size_t _relocate = 0;
public:
    ZRelocationSetSelectorGroupStats();
    size_t npages_candidates() const;
    size_t total() const;
    size_t live() const;
    size_t empty() const;
    size_t npages_selected() const;
    size_t relocate() const;
};

class ZRelocationSetSelectorStats {
    friend class ZRelocationSetSelector;
private:
    ZRelocationSetSelectorGroupStats _small[kPageAgeCount];
    ZRelocationSetSelectorGroupStats _medium[kPageAgeCount];
    ZRelocationSetSelectorGroupStats _large[kPageAgeCount];
    size_t _has_relocatable_pages = 0;
public:
    const ZRelocationSetSelectorGroupStats& small(PageAge age) const;
    const ZRelocationSetSelectorGroupStats& medium(PageAge age) const;
    const ZRelocationSetSelectorGroupStats& large(PageAge age) const;
    bool has_relocatable_pages() const;
};

class ZRelocationSetSelectorGroup {
private:
    static constexpr int NumPartitionsShift = 11;
    static constexpr int NumPartitions = int(1) << NumPartitionsShift;

    const char* const _name;
    const ZPageType _page_type;
    const size_t _max_page_size;
    const size_t _object_size_limit;
    const double _fragmentation_limit;
    const size_t _page_fragmentation_limit;
    ZArray<ZPage*> _live_pages;
    ZArray<ZPage*> _not_selected_pages;
    size_t _forwarding_entries;
    ZRelocationSetSelectorGroupStats _stats[kPageAgeCount];

    bool is_disabled();
    bool is_selectable();
    size_t partition_index(const ZPage* page) const;
    void semi_sort();
    void select_inner();
    bool pre_filter_page(const ZPage* page, size_t live_bytes) const;

public:
    ZRelocationSetSelectorGroup(const char* name, ZPageType page_type, size_t max_page_size,
                                size_t object_size_limit, double fragmentation_limit);
    void register_live_page(ZPage* page);
    void register_empty_page(ZPage* page);
    void select();
    const ZArray<ZPage*>* selected_pages() const;
    const ZArray<ZPage*>* not_selected_pages() const;
    size_t forwarding_entries() const;
    const ZRelocationSetSelectorGroupStats& stats(PageAge age) const;
};

class ZRelocationSetSelector {
private:
    ZRelocationSetSelectorGroup _small;
    ZRelocationSetSelectorGroup _medium;
    ZRelocationSetSelectorGroup _large;
    ZArray<ZPage*> _empty_pages;
    size_t total() const;
    size_t empty() const;
    size_t relocate() const;
public:
    explicit ZRelocationSetSelector(double fragmentation_limit);
    void register_live_page(ZPage* page);
    void register_empty_page(ZPage* page);
    bool should_free_empty_pages(int bulk) const;
    const ZArray<ZPage*>* empty_pages() const;
    void clear_empty_pages();
    void select();
    const ZArray<ZPage*>* selected_small() const;
    const ZArray<ZPage*>* selected_medium() const;
    const ZArray<ZPage*>* not_selected_small() const;
    const ZArray<ZPage*>* not_selected_medium() const;
    const ZArray<ZPage*>* not_selected_large() const;
    size_t forwarding_entries() const;
    ZRelocationSetSelectorStats stats() const;
};

inline constexpr size_t kRelocationMaxSmallRegionBytes = 128 * 1024;
inline constexpr size_t kRelocationObjectSizeLimit = 16 * 1024;

enum class RelocRegionKind : uint8_t { Small = 0, Large = 1 };

struct RelocRegionDesc {
    size_t liveBytes = 0;
    size_t capacity = 0;
    RelocRegionKind kind = RelocRegionKind::Small;
    uint32_t id = 0;
    bool allocating = false;
};

struct RelocSelectResult {
    std::vector<uint32_t> selectedIds;
};

inline bool PreFilterRelocRegion(const RelocRegionDesc& page, double fragmentationLimit)
{
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
        static_cast<size_t>(static_cast<double>(page.capacity) * (fragmentationLimit / 100.0));
    return garbage > pageFragLimit;
}

inline constexpr size_t kRelocationNumPartitionsShift = 11;
inline constexpr size_t kRelocationNumPartitions = size_t{1} << kRelocationNumPartitionsShift;

inline size_t RelocationPartitionIndex(const RelocRegionDesc& page)
{
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

inline RelocSelectResult SelectRelocationSet(const std::vector<RelocRegionDesc>& pages, double fragmentationLimit)
{
    RelocSelectResult out;
    std::vector<RelocRegionDesc> live;
    live.reserve(pages.size());
    for (const RelocRegionDesc& p : pages) {
        if (PreFilterRelocRegion(p, fragmentationLimit)) {
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
        if (diffReclaimable > fragmentationLimit) {
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
