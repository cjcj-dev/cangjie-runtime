#ifndef MRT_RELOCATION_SET_SELECTOR_INLINE_H
#define MRT_RELOCATION_SET_SELECTOR_INLINE_H

#include "Heap/z/zRelocationSetSelector.hpp"

#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zPageAge.inline.hpp"

namespace MapleRuntime {

inline size_t ZRelocationSetSelectorGroupStats::npages_candidates() const { return _npages_candidates; }
inline size_t ZRelocationSetSelectorGroupStats::total() const { return _total; }
inline size_t ZRelocationSetSelectorGroupStats::live() const { return _live; }
inline size_t ZRelocationSetSelectorGroupStats::empty() const { return _empty; }
inline size_t ZRelocationSetSelectorGroupStats::npages_selected() const { return _npages_selected; }
inline size_t ZRelocationSetSelectorGroupStats::relocate() const { return _relocate; }

inline bool ZRelocationSetSelectorStats::has_relocatable_pages() const { return _has_relocatable_pages != 0; }

inline const ZRelocationSetSelectorGroupStats& ZRelocationSetSelectorStats::small(PageAge age) const
{
    return _small[untype(age)];
}
inline const ZRelocationSetSelectorGroupStats& ZRelocationSetSelectorStats::medium(PageAge age) const
{
    return _medium[untype(age)];
}
inline const ZRelocationSetSelectorGroupStats& ZRelocationSetSelectorStats::large(PageAge age) const
{
    return _large[untype(age)];
}

inline bool ZRelocationSetSelectorGroup::pre_filter_page(const ZPage* page, size_t live_bytes) const
{
    if (page->is_small()) {
        const size_t garbage = page->size() - live_bytes;
        return garbage > _page_fragmentation_limit;
    }
    if (page->is_medium()) {
        const size_t size = page->size();
        const size_t garbage = size - live_bytes;
        const size_t page_fragmentation_limit =
            _max_page_size == 0 ? 0 : static_cast<size_t>(static_cast<double>(_page_fragmentation_limit) *
                                                          (static_cast<double>(size) / static_cast<double>(_max_page_size)));
        return garbage > page_fragmentation_limit;
    }
    return false;
}

inline void ZRelocationSetSelectorGroup::register_live_page(ZPage* page)
{
    const size_t live = page->live_bytes();
    if (pre_filter_page(page, live)) {
        _live_pages.append(page);
    } else if (page->is_young()) {
        _not_selected_pages.append(page);
    }
    const size_t size = page->size();
    const uint32_t age = untype(page->age());
    _stats[age]._npages_candidates++;
    _stats[age]._total += size;
    _stats[age]._live += live;
}

inline void ZRelocationSetSelectorGroup::append_selected(ZPage* page, size_t nentries)
{
    _live_pages.append(page);
    _forwarding_entries += nentries;
}

inline void ZRelocationSetSelectorGroup::register_empty_page(ZPage* page)
{
    const size_t size = page->size();
    const uint32_t age = untype(page->age());
    _stats[age]._npages_candidates++;
    _stats[age]._total += size;
    _stats[age]._empty += size;
}

inline const ZArray<ZPage*>* ZRelocationSetSelectorGroup::selected_pages() const { return &_live_pages; }
inline const ZArray<ZPage*>* ZRelocationSetSelectorGroup::not_selected_pages() const { return &_not_selected_pages; }
inline size_t ZRelocationSetSelectorGroup::forwarding_entries() const { return _forwarding_entries; }
inline const ZRelocationSetSelectorGroupStats& ZRelocationSetSelectorGroup::stats(PageAge age) const
{
    return _stats[untype(age)];
}

inline void ZRelocationSetSelector::register_live_page(ZPage* page)
{
    const ZPageType type = page->type();
    if (type == ZPageType::small) {
        _small.register_live_page(page);
    } else if (type == ZPageType::medium) {
        _medium.register_live_page(page);
    } else {
        _large.register_live_page(page);
    }
}

inline void ZRelocationSetSelector::add_selected_small(ZPage* page, size_t nentries)
{
    CHECK_DETAIL(page->is_relocatable(),
                 "selected page must be relocatable start=%#zx", page->GetRegionStart());
    _small.append_selected(page, nentries);
}

inline void ZRelocationSetSelector::register_empty_page(ZPage* page)
{
    const ZPageType type = page->type();
    if (type == ZPageType::small) {
        _small.register_empty_page(page);
    } else if (type == ZPageType::medium) {
        _medium.register_empty_page(page);
    } else {
        _large.register_empty_page(page);
    }
    _empty_pages.append(page);
}

inline bool ZRelocationSetSelector::should_free_empty_pages(int bulk) const
{
    return _empty_pages.length() >= bulk && _empty_pages.is_nonempty();
}
inline const ZArray<ZPage*>* ZRelocationSetSelector::empty_pages() const { return &_empty_pages; }
inline void ZRelocationSetSelector::clear_empty_pages() { _empty_pages.clear(); }

inline size_t ZRelocationSetSelector::total() const
{
    size_t sum = 0;
    for (PageAge age : kPageAgeRangeAll) {
        sum += _small.stats(age).total() + _medium.stats(age).total() + _large.stats(age).total();
    }
    return sum;
}
inline size_t ZRelocationSetSelector::empty() const
{
    size_t sum = 0;
    for (PageAge age : kPageAgeRangeAll) {
        sum += _small.stats(age).empty() + _medium.stats(age).empty() + _large.stats(age).empty();
    }
    return sum;
}
inline size_t ZRelocationSetSelector::relocate() const
{
    size_t sum = 0;
    for (PageAge age : kPageAgeRangeAll) {
        sum += _small.stats(age).relocate() + _medium.stats(age).relocate() + _large.stats(age).relocate();
    }
    return sum;
}

inline const ZArray<ZPage*>* ZRelocationSetSelector::selected_small() const { return _small.selected_pages(); }
inline const ZArray<ZPage*>* ZRelocationSetSelector::selected_medium() const { return _medium.selected_pages(); }
inline const ZArray<ZPage*>* ZRelocationSetSelector::not_selected_small() const { return _small.not_selected_pages(); }
inline const ZArray<ZPage*>* ZRelocationSetSelector::not_selected_medium() const { return _medium.not_selected_pages(); }
inline const ZArray<ZPage*>* ZRelocationSetSelector::not_selected_large() const { return _large.not_selected_pages(); }
inline size_t ZRelocationSetSelector::forwarding_entries() const
{
    return _small.forwarding_entries() + _medium.forwarding_entries();
}

} // namespace MapleRuntime
#endif
