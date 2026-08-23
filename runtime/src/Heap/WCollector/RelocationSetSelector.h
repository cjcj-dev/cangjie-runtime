#ifndef MRT_RELOCATION_SET_SELECTOR_H
#define MRT_RELOCATION_SET_SELECTOR_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MapleRuntime {

// Compile-time port of ZFragmentationLimit (z_globals.hpp) — not an MRT_* env.
inline constexpr bool kUseRelocationSetSelector = true;
inline constexpr double kRelocationFragmentationLimitPercent = 25.0;
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
    // is_allocating ≡ HasMarkStartAllocGap (zPage.inline.hpp:180-185).
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

// ZRelocationSetSelectorGroup::select_inner (zRelocationSetSelector.cpp:114-196)
inline RelocSelectResult SelectRelocationSet(const std::vector<RelocRegionDesc>& pages)
{
    RelocSelectResult out;
    if (!kUseRelocationSetSelector) {
        return out;
    }

    std::vector<RelocRegionDesc> live;
    live.reserve(pages.size());
    for (const RelocRegionDesc& p : pages) {
        if (PreFilterRelocRegion(p)) {
            live.push_back(p);
        }
    }
    std::stable_sort(live.begin(), live.end(),
                     [](const RelocRegionDesc& a, const RelocRegionDesc& b) { return a.liveBytes < b.liveBytes; });

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
