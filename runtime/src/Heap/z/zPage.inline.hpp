// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZPAGE_INLINE_H
#define MRT_ZPAGE_INLINE_H

#include "Heap/z/zPage.hpp"
#include "Heap/z/zLiveMap.inline.hpp"
#include "Heap/z/zSafeDelete.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"
#include "Heap/z/zRememberedSet.inline.hpp"

namespace MapleRuntime {

inline bool ZPage::IsCompacted() const
    {
        auto owner = forwarding_for_page(const_cast<ZPage*>(this));
        return owner && owner->is_done() && owner->in_place();
    }

inline bool ZPage::IsRoutingState()
    {
        auto owner = forwarding_for_page(this);
        return owner && owner->is_claimed() && !owner->is_done();
    }

inline ZLiveMap& ZPage::livemap()
{
    return _livemap;
}

inline const ZLiveMap& ZPage::livemap() const
{
    return _livemap;
}



inline void ZPage::StampCensusBoundary()
    {
        uintptr_t offset = GetRegionAllocPtr() - GetRegionStart();
        _scratch.censusBoundaryOffset =
            static_cast<uint32_t>(std::min<uintptr_t>(offset, std::numeric_limits<uint32_t>::max()));
    }

// ---- ZPage livemap surface ----

// zPage.inline.hpp:72-101 object_alignment_shift: large pages hold one object
// at start; small pages use the minimum object alignment.
inline int ZPage::object_alignment_shift() const
{
    switch (type()) {
        case ZPageType::small:
            return ZObjectAlignmentSmallShift;
        case ZPageType::medium:
            return ZObjectAlignmentMediumShift;
        case ZPageType::large:
            return ZObjectAlignmentLargeShift;
        default:
            return ZObjectAlignmentSmallShift;
    }
}

inline size_t ZPage::object_alignment() const
{
    return size_t(1) << object_alignment_shift();
}

// zPage.inline.hpp:57-70 object_max_count.
inline uint32_t ZPage::object_max_count_for(ZPageType type, size_t size)
{
    if (type == ZPageType::large) {
        return 1;
    }
    const int shift = type == ZPageType::medium ? ZObjectAlignmentMediumShift : ZObjectAlignmentSmallShift;
    const uint32_t n = static_cast<uint32_t>(size >> shift);
    return n == 0 ? 1u : n;
}

inline uint32_t ZPage::object_max_count() const
{
    if (!_virtual.is_null()) {
        return object_max_count_for(type(), _virtual.size());
    }
    if (type() == ZPageType::large) {
        return 1;
    }
    return static_cast<uint32_t>(GetRegionSize() >> object_alignment_shift());
}

// zPage.inline.hpp:188-195 is_in: [start, top).
inline bool ZPage::is_in(zaddress addr) const
{
    const MAddress address = raw(addr);
    return address >= GetRegionStart() && address < GetRegionAllocPtr();
}

// zPage.inline.hpp:223-226.
inline bool ZPage::is_marked() const
{
    DCHECK_D(IsRelocatable(), "Invalid page state");
    return livemap().is_marked(generation_id());
}

// zPage.inline.hpp:228-230.
inline BitMap::idx_t ZPage::bit_index(zaddress addr) const
{
    return (GetAddressOffset(raw(addr)) >> object_alignment_shift()) * 2;
}

// zPage.inline.hpp:232-235.
inline MAddress ZPage::offset_from_bit_index(BitMap::idx_t index) const
{
    const uintptr_t l_offset = ((index / 2) << object_alignment_shift());
    return GetRegionStart() + l_offset;
}

// zPage.inline.hpp:237-240.
inline BaseObject* ZPage::object_from_bit_index(BitMap::idx_t index) const
{
    return from_region_addr(offset_from_bit_index(index));
}

// zPage.inline.hpp:242-252.
inline bool ZPage::is_live_bit_set(zaddress addr) const
{
    DCHECK_D(IsRelocatable(), "Invalid page state");
    const BitMap::idx_t index = bit_index(addr);
    return livemap().get(generation_id(), index);
}

inline bool ZPage::is_strong_bit_set(zaddress addr) const
{
    DCHECK_D(IsRelocatable(), "Invalid page state");
    const BitMap::idx_t index = bit_index(addr);
    return livemap().get(generation_id(), index + 1);
}

// zPage.inline.hpp:254-260: an allocating page is implicitly live.
inline bool ZPage::is_object_live(zaddress addr) const
{
    return IsAllocating() || is_live_bit_set(addr);
}

inline bool ZPage::is_object_strongly_live(zaddress addr) const
{
    return IsAllocating() || is_strong_bit_set(addr);
}

// zPage.inline.hpp:262-282: marking-only readers.
inline bool ZPage::is_object_marked_live(zaddress addr) const
{
    return is_object_live(addr);
}

inline bool ZPage::is_object_marked_strong(zaddress addr) const
{
    return is_object_strongly_live(addr);
}

inline bool ZPage::is_object_marked(zaddress addr, bool finalizable) const
{
    return finalizable ? is_object_marked_live(addr) : is_object_marked_strong(addr);
}

// zPage.inline.hpp:284-294.
inline bool ZPage::mark_object(zaddress addr, bool finalizable, bool& inc_live)
{
    DCHECK_D(IsRelocatable(), "Invalid page state");
    DCHECK_D(is_in(addr), "Invalid address");

    // Set mark bit
    const BitMap::idx_t index = bit_index(addr);
    return livemap().set(generation_id(), index, finalizable, inc_live);
}

// zPage.inline.hpp:296-317.
inline void ZPage::inc_live(uint32_t objects, size_t bytes)
{
    livemap().inc_live(objects, bytes);
}

inline uint32_t ZPage::live_objects() const
{
    return livemap().live_objects();
}

inline size_t ZPage::live_bytes() const
{
    return livemap().live_bytes();
}

// zPage.inline.hpp:319-331.
template <typename Function>
inline void ZPage::object_iterate(Function function)
{
    auto do_bit = [&](BitMap::idx_t index) -> bool {
        BaseObject* const obj = object_from_bit_index(index);

        // Apply function
        function(obj);

        return true;
    };

    livemap().iterate(generation_id(), do_bit);
}

// zPage.inline.hpp:371-386.
inline MAddress ZPage::find_base_unsafe(MAddress p)
{
    if (IsLargeRegion()) {
        return GetRegionStart();
    }

    // Note: when thinking about excluding looking at the index corresponding to
    // the field address p, it's important to note that for medium pages both p
    // and it's associated base could map to the same index.
    const BitMap::idx_t index = bit_index(to_zaddress(p));
    const BitMap::idx_t base_index = livemap().find_base_bit(index);
    if (base_index == BitMap::idx_t(-1)) {
        return 0;
    } else {
        return offset_from_bit_index(base_index);
    }
}

// zPage.inline.hpp:388-392.
inline MAddress ZPage::find_base(MAddress p)
{
    DCHECK_D(is_marked(), "Should be marked");
    return find_base_unsafe(p);
}

inline void ZPage::remember(volatile zpointer* p)
{
    if (!_remembered_set.is_initialized()) {
        return;
    }
    _remembered_set.set_current(local_offset(reinterpret_cast<MAddress>(p)));
}

inline bool ZPage::is_remembered(volatile zpointer* p)
{
    if (!_remembered_set.is_initialized()) {
        return false;
    }
    return _remembered_set.at_current(local_offset(reinterpret_cast<MAddress>(p)));
}

inline bool ZPage::was_remembered(volatile zpointer* p)
{
    if (!_remembered_set.is_initialized()) {
        return false;
    }
    return _remembered_set.at_previous(local_offset(reinterpret_cast<MAddress>(p)));
}

inline void ZPage::clear_remset_bit_non_par_current(uintptr_t l_offset)
{
    _remembered_set.unset_non_par_current(l_offset);
}

inline void ZPage::clear_remset_range_non_par_current(uintptr_t l_offset, size_t size)
{
    _remembered_set.unset_range_non_par_current(l_offset, size);
}

inline ZBitMap::ReverseIterator ZPage::remset_reverse_iterator_previous()
{
    return _remembered_set.iterator_reverse_previous();
}

inline ZRememberedSet::Iterator ZPage::remset_iterator_limited_current(uintptr_t l_offset, size_t size)
{
    return _remembered_set.iterator_limited_current(l_offset, size);
}

inline ZRememberedSet::Iterator ZPage::remset_iterator_limited_previous(uintptr_t l_offset, size_t size)
{
    return _remembered_set.iterator_limited_previous(l_offset, size);
}

inline bool ZPage::is_remset_cleared_current() const
{
    return _remembered_set.is_cleared_current();
}

inline bool ZPage::is_remset_cleared_previous() const
{
    return _remembered_set.is_cleared_previous();
}

inline void ZPage::clear_remset_previous()
{
    _remembered_set.clear_previous();
}

inline void ZPage::swap_remset_bitmaps()
{
    _remembered_set.swap_remset_bitmaps();
}

inline void* ZPage::remset_current()
{
    return _remembered_set.current();
}

template<typename Function>
inline void ZPage::oops_do_remembered(Function function)
{
    _remembered_set.iterate_previous([&](uintptr_t l_offset) {
        function(reinterpret_cast<volatile zpointer*>(global_offset(l_offset)));
    });
}

template<typename Function>
inline void ZPage::oops_do_remembered_in_live(Function function)
{
    ZRememberedSetContainingInLiveIterator iter(this);
    for (ZRememberedSetContaining containing; iter.next(&containing);) {
        function(reinterpret_cast<volatile zpointer*>(containing._field_addr));
    }
    iter.print_statistics();
}

template<typename Function>
inline void ZPage::oops_do_current_remembered(Function function)
{
    _remembered_set.iterate_current([&](uintptr_t l_offset) {
        function(reinterpret_cast<volatile zpointer*>(global_offset(l_offset)));
    });
}

// zPage.cpp:115-117.
inline void ZPage::reset_livemap()
{
    livemap().reset();
}

inline ALWAYS_INLINE size_t ZPage::GetAddressOffset(MAddress address) const
    {
        DCHECK(GetRegionStart() <= address);
        return (address - GetRegionStart());
    }

inline void ZPage::RetirePage(ZPage* region, std::function<void()> retire)
    {
        CHECK(ZPageTable::heap_table().get(region->GetRegionStart()) == region);
        ZPageTable::heap_table().remove(region);
        // zPageAllocator.cpp:2248-2250 safe_destroy_page: deferred while any
        // page iterator is active, immediate otherwise. The retire hook runs
        // from ~ZPage when the deferred delete lands.
        region->_retireHook = std::move(retire);
        safeDestroy.schedule_delete(region);
    }

inline void ZPage::RetireDescriptor(ZPage* page)
    {
        CHECK(page != nullptr);
        CHECK(ZPageTable::heap_table().get(page->GetRegionStart()) != page);
        safeDestroy.schedule_delete(page);
    }

inline void ZPage::EnableSafeDestroy()
    {
        safeDestroy.enable_deferred_delete();
    }

inline void ZPage::DisableSafeDestroy()
    {
        safeDestroy.disable_deferred_delete();
    }

inline void ZPage::InitializeSegments(uintptr_t metadataEnd, const std::vector<ZVirtualMemory>& ranges)
    {
        std::vector<ReservedSegment> segments;
        for (const ZVirtualMemory& range : ranges) {
            segments.push_back(ReservedSegment{ untype(ZOffset::address_unsafe(range.start())), range.size() });
        }
        InitializeSegments(metadataEnd, segments);
    }

inline void ZPage::InitializeSegments(uintptr_t metadataEnd, const std::vector<ReservedSegment>& segments)
    {
        heapStartAddress = metadataEnd;
        reservedSegments.clear();
        for (const auto& range : segments) {
            CHECK(IsRepresentableLow48Range(range.start, range.size));
            reservedSegments.push_back(ReservedSegment{ range.start, range.size });
        }
    }

inline size_t ZPage::GranuleIndex(uintptr_t address)
    {
        return ContainsReservedRange(address, 1)
            ? untype(ZAddress::offset(to_zaddress_unsafe(address))) >> ZGranuleSizeShift
            : INVALID_IDX;
    }

inline bool ZPage::ContainsReservedRange(uintptr_t start, size_t size)
    {
        for (const auto& segment : reservedSegments) {
            if (start >= segment.start && start < segment.End() && size <= segment.End() - start) {
                return true;
            }
        }
        return false;
    }

inline void ZPage::VisitPageOwners(const std::function<void(ZPage*)>& visitor)
    {
        // zPageTable.cpp:83-98: the iterator's lifetime brackets safe destroy,
        // including nested iteration and exceptional callback exits.
        SafeDestroyScope iteration;
        ZPageTableIterator iter(&ZPageTable::heap_table());
        ZPage* page = nullptr;
        while (iter.next(&page)) {
            visitor(page);
        }
    }

inline MAddress ZPage::GranuleAddress(size_t idx)
    {
        CHECK(idx < (ZAddressOffsetMax >> ZGranuleSizeShift));
        return untype(ZOffset::address_unsafe(static_cast<zoffset>(idx << ZGranuleSizeShift)));
    }

inline ZPage* ZPage::InitRegion(size_t granuleIndex, size_t pageSize, ZPageType uclass, PageAge age)
    {
        const MAddress start = GranuleAddress(granuleIndex);
        ZPage* region = new ZPage(uclass, age,
                                            ZVirtualMemory(ZAddress::offset(to_zaddress_unsafe(start)),
                                                           pageSize));
        region->InitRegion(pageSize, uclass, age);
        return region;
    }

inline void ZPage::WaitCopiedBeforePayloadWipe(ZPage* region, const char* site)
    {
        if (region == nullptr) {
            return;
        }
        (void)site;
        ZForwarding::WaitPageDone(forwarding_for_page(region));
    }



inline bool ZPage::IsEmpty() const
    {
        MRT_ASSERT(IsSmallRegion(), "wrong region type");
        return GetRegionAllocPtr() == GetRegionStart();
    }

inline size_t ZPage::GetRegionSize() const { return size(); }

inline size_t ZPage::GetRegionSizeForDetachCheck() const { return size(); }

inline size_t ZPage::GetGhostRegionSize() const
    {
        // The old extent follows the forwarding incarnation. If no carrier is
        // installed (idle/test setup), the only valid extent is the page's
        // current own size.
        ZForwarding* carrier = GetFromPageCarrier();
        return carrier == nullptr ? GetRegionSize() : carrier->size();
    }

inline size_t ZPage::GetAvailableSize() const
    {
        MRT_ASSERT(IsSmallRegion(), "wrong region type");
        return GetRegionEnd() - GetRegionAllocPtr();
    }

inline void ZPage::RetirePageMemory()
    {
        InitZPage(GetRegionSize(), ZPageType::small, PageAge::old, false);
    }





















inline bool ZPage::IsCompactRouteDestination(MAddress address) const
    {
        // The generation relocation set owns the sole from-to mapping.
        auto owner = forwarding_for_page(this);
        return IsCompacted() && owner && owner->find_from_by_to(address, nullptr);
    }





inline bool ZPage::RetainForwarding()
    {
        auto owner = forwarding_for_page(this);
        return owner && owner->retain_page(&generation_relocate_queue());
    }

inline void ZPage::ReleaseForwarding()
    {
        auto owner = forwarding_for_page(this);
        CHECK(owner);
        owner->release_page();
    }

inline bool ZPage::ClaimForwarding()
    {
        auto owner = forwarding_for_page(this);
        return owner && owner->claim();
    }

inline void ZPage::MarkForwardingDone()
    {
        auto owner = forwarding_for_page(this);
        if (owner && ZForwarding::CurrentPageWork() != owner) owner->mark_done();
    }

inline bool ZPage::IsForwardingDone() const
    {
        auto owner = forwarding_for_page(this);
        return owner && owner->is_done();
    }


inline int32_t ZPage::ForwardingRefCount() const
    {
        auto owner = forwarding_for_page(this);
        return owner ? owner->ref_count().load(std::memory_order_acquire) : 0;
    }

inline bool ZPage::ForwardingClaimed() const
    {
        auto owner = forwarding_for_page(this);
        return owner && owner->claimed().load(std::memory_order_acquire);
    }







inline void ZPage::SetInGhostRegion(uint8_t flag)
    {
        (void)flag;
    }

// ZPage::clone_for_promotion + ZPage::reset(age) (zPage.cpp:64-72, 103-113)
// on the reused slot: the young livemap is parked for the carrier's readers
// and the slot continues with a fresh map; readers see one or the other,
// never a missing map.
inline void ZPage::PromoteYoungRegion()
    {
        CHECK_DETAIL(IsYoungRegion(), "cannot promote an old region %p", this);
        reset(PageAge::old);
        reset_livemap();
    }

inline uint8_t ZPage::GetYoungAge() const
    {
        return _age == PageAge::old ? uint8_t{0} : static_cast<uint8_t>(untype(_age));
    }



// ZPage::is_allocating / is_relocatable (zPage.inline.hpp:180-186).
inline bool ZPage::IsAllocating() const
{
    return BirthSequence() == GetSnapshotEpoch();
}

inline bool ZPage::IsRelocatable() const
{
    return BirthSequence() < GetSnapshotEpoch();
}

inline bool ZPage::IsPinnedRegion() const
    {
        const ZPageRole role = GetRegionRole();
        return role == ZPageRole::OldPinned || role == ZPageRole::RecentPinned;
    }

inline bool ZPage::IsUnmovableFromRegion() const
    {
        const ZPageRole role = GetRegionRole();
        return role == ZPageRole::UnmovableFrom;
    }

inline bool ZPage::IsValidRegion() const
    {
        return is_small() || is_medium() || is_large();
    }

// zRelocationSetSelector.cpp:114-196 / zGeneration.cpp:216-221: a relocatable
// page that was marked this cycle and has no live bytes is garbage. A page not
// marked this cycle is not known empty here (kept for the selector's ZGC
// convergence in the page-descriptor package).
inline bool ZPage::IsKnownEmpty() const
    {
        if (IsAllocating()) {
            return false;
        }
        return is_marked() && live_bytes() == 0;
    }

inline bool ZPage::IsKnownYoungEmpty() const
    {
        if (IsAllocating()) {
            return false;
        }
        return is_marked() && live_bytes() == 0;
    }

inline bool ZPage::IsSafeKnownEmpty()
    {
        return IsKnownEmpty();
    }

inline bool ZPage::IsSafeKnownYoungEmpty()
    {
        return IsKnownYoungEmpty();
    }











inline void ZPage::BumpRegionLifeId()
    {
        RegionLifeId old = _scratch.regionLifeId.load(std::memory_order_relaxed);
        for (;;) {
            if (UNLIKELY(old == std::numeric_limits<RegionLifeId>::max())) {
                LOG(RTLOG_FATAL,
                    "[LIFECLOCK][REGION_LIFE_ID_OVERFLOW] region=%p life=%llu; wraparound is forbidden",
                    this, static_cast<unsigned long long>(old));
                return;
            }
            if (_scratch.regionLifeId.compare_exchange_weak(old, old + 1, std::memory_order_release,
                                                            std::memory_order_relaxed)) {
                return;
            }
        }
    }

inline void ZPage::InitZPage(size_t pageSize, ZPageType uClass, PageAge age, bool live)
    {
        CHECK(ContainsReservedRange(GetRegionStart(), pageSize));
        CHECK(ZPageTable::heap_table().get(GetRegionStart()) == nullptr);
        CHECK_DETAIL(GetRegionRole() == ZPageRole::None, "reinitializing a region still carrying a role");

        // Invalidate every old-life carrier before clearing any of its payload.
        // Readers either retain the old page (detachgate) or observe this bump and
        // reject the old incarnation; there is no wraparound fallback.
        BumpRegionLifeId();
        {
            uint8_t cur = __atomic_load_n(&_scratch.regionLifeSequence, __ATOMIC_RELAXED);
            uint8_t next = static_cast<uint8_t>((cur + 1) & 0x7f);
            __atomic_store_n(&_scratch.regionLifeSequence, next, __ATOMIC_RELEASE);
        }
        // Retire the forwarding owner before detaching its compact table.
        _scratch.fwdOwner.store(nullptr, std::memory_order_release);
        WaitCopiedBeforePayloadWipe(this, "InitZPage");
        delete _scratch.retiredLivemap;
        _scratch.retiredLivemap = nullptr;
        _top = to_zoffset_end(start());
        reset(age);
        if (age == PageAge::old && !_remembered_set.is_initialized()) {
            remset_alloc();
        }
        if (live) {
            reset_livemap();
        }
        _scratch.regionRole.store(ZPageRole::None, std::memory_order_relaxed);
        _scratch.censusBoundaryOffset = 0;

        // routedest: this is the reuse edge named in the defect. TakeRegion has already run
        // ClearPageMemory over this payload; if a published route still names this region, the
        // route now answers into zeroed (or freshly re-allocated) memory. Count it here
        // rather than at ClearPageMemory because this is the one call that runs exactly once per
        // reuse. The hold is deliberately NOT cleared: reaching this point while held means
        // a reclaim gate was bypassed, and leaving the flag set keeps the region out of the
        // next collection set instead of silently papering over the escape.
        SetInGhostRegion(0);
        (void)uClass;
        (void)live;
    }

inline void ZPage::InitRegion(size_t pageSize, ZPageType uClass, PageAge age)
    {
        InitZPage(pageSize, uClass, age, true);
        (void)pageSize;
    }

inline ZGenerationId ZPage::generation_id() const
{
    return _generation_id;
}

inline unsigned ZPage::RelocateObserve() const
{
    auto owner = forwarding_for_page(const_cast<ZPage*>(this));
    if (!owner) {
        return 0;
    }
    unsigned v = 1;
    if (owner->is_claimed()) {
        v |= 2;
    }
    if (owner->is_done()) {
        v |= 4;
    }
    if (owner->in_place()) {
        v |= 8;
    }
    return v;
}

inline std::atomic<uint64_t>& ZPage::EnrolBeforeFlip()
{
    static std::atomic<uint64_t> n{ 0 };
    return n;
}

inline std::atomic<uint64_t>& ZPage::EnrolAfterFlip()
{
    static std::atomic<uint64_t> n{ 0 };
    return n;
}

inline bool ZPage::IsYoungRegion() const
{
    return generation_id() == ZGenerationId::young;
}

inline Generation ZPage::GetOwnerGeneration() const
{
    return IsYoungRegion() ? Generation::Young : Generation::Old;
}

inline MAddress ZPage::GetRegionStart() const
{
    if (!_virtual.is_null()) {
        return untype(ZOffset::address_unsafe(_virtual.start()));
    }
    return 0;
}

inline MAddress ZPage::GetRegionEnd() const { return _virtual.is_null() ? 0 : ZAddressHeapBase + untype(end()); }

inline MAddress ZPage::GetRegionAllocPtr() const { return _virtual.is_null() ? 0 : ZAddressHeapBase + untype(top()); }

inline bool ZPage::IsSmallRegion() const { return is_small(); }

inline bool ZPage::IsLargeRegion() const { return is_large(); }

template<typename Function>
inline void ZGenerationPagesParallelIterator::do_pages(Function function)
{
    _iterator.do_pages([&](ZPage* page) {
        if (page->generation_id() == _generation_id) {
            return function(page);
        }
        return true;
    });
}

} // namespace MapleRuntime
#endif
