// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGIONINFO_INLINE_H
#define MRT_REGIONINFO_INLINE_H

#include "Heap/z/zPage.hpp"
#include "Heap/z/zLiveMap.inline.hpp"
#include "Heap/z/zSafeDelete.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"

namespace MapleRuntime {

inline bool ZPage::IsCompacted() const
    {
        auto owner = ForwardingTable::RetainPageOwner(const_cast<ZPage*>(this));
        return owner && owner->is_done() && owner->in_place();
    }

inline bool ZPage::IsRoutingState()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->is_claimed() && !owner->is_done();
    }

inline ZLiveMap* ZPage::livemap() const
{
    return __atomic_load_n(&_scratch.livemap, std::memory_order_acquire);
}

inline ZForwarding* ZPage::GetFromPageCarrier() const
    {
        ZForwarding* carrier = ForwardingTable::RetainPageOwner(this).get();
        return carrier != nullptr && carrier->page() == this ? carrier : nullptr;
    }

inline bool ZPage::HasFromPageMetadata() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from != nullptr && (from->lifeId == GetRegionLifeId());
    }

inline ZLiveMap* ZPage::FromPageLiveMap() const
{
    const ZForwarding::FromPageView* from = GetFromPageView();
    return from == nullptr ? nullptr : from->livemap;
}

inline Generation ZPage::GetRouteMarkGeneration() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from == nullptr ? GetOwnerGeneration() : static_cast<Generation>(from->owner);
    }

inline bool ZPage::IsFromPageAllocating() const
{
    const ZForwarding::FromPageView* from = GetFromPageView();
    return from != nullptr && from->birthSequence == from->epoch;
}

// The from page is read through the livemap the carrier retained; the page
// identity (generation, geometry) is the one published with it.
inline bool ZPage::IsFromPageSurvivedObject(size_t offset) const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            return false;
        }
        if (IsFromPageAllocating()) {
            return true;
        }
        if (IsLargeRegion()) {
            return from->largeMarked != 0;
        }
        ZLiveMap* map = from->livemap;
        if (map == nullptr) {
            return false;
        }
        const ZGenerationId id = static_cast<Generation>(from->owner) == Generation::Young
            ? ZGenerationId::young : ZGenerationId::old;
        return map->get(id, bit_index(to_zaddress(GetRegionStart() + offset)));
    }

inline bool ZPage::IsRouteSurvivedObject(size_t offset)
    {
        if (!HasFromPageMetadata()) {
            return is_object_live(to_zaddress(GetRegionStart() + offset));
        }
        return IsFromPageSurvivedObject(offset);
    }

inline bool ZPage::IsRouteMarkedObject(size_t offset)
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            return is_object_strongly_live(to_zaddress(GetRegionStart() + offset));
        }
        if (IsFromPageAllocating()) {
            return true;
        }
        if (IsLargeRegion()) {
            return from->largeMarked != 0;
        }
        ZLiveMap* map = from->livemap;
        if (map == nullptr) {
            return false;
        }
        const ZGenerationId id = static_cast<Generation>(from->owner) == Generation::Young
            ? ZGenerationId::young : ZGenerationId::old;
        return map->get(id, bit_index(to_zaddress(GetRegionStart() + offset)) + 1);
    }

inline bool ZPage::IsRouteKnownEmpty()
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            if (IsYoungRegion()) {
                return IsKnownYoungEmpty();
            }
            return IsKnownEmpty();
        }
        if (IsFromPageAllocating()) {
            return false;
        }
        ZLiveMap* map = from->livemap;
        const ZGenerationId id = static_cast<Generation>(from->owner) == Generation::Young
            ? ZGenerationId::young : ZGenerationId::old;
        return map != nullptr && map->is_marked(id) && map->live_bytes() == 0;
    }

inline void ZPage::BindFromPageLiveMapIfNull()
    {
        if (FromPageLiveMap() != nullptr) {
            return;
        }
        ZLiveMap* live = livemap();
        if (live == nullptr) {
            return;
        }
        const RegionLifeId life = GetRegionLifeId();
        CHECK_DETAIL(ForwardingTable::PublishFromPageView(
                         this, live, GetSnapshotEpoch(), GetRegionAllocPtr(), BirthSequence(),
                         static_cast<uint8_t>(GetOwnerGeneration()),
                         static_cast<uint8_t>(IsLargeRegion() && is_marked() &&
                                              is_live_bit_set(to_zaddress(GetRegionStart()))),
                         life),
                     "from-page forwarding carrier missing while binding live face region=%p", this);
    }

inline void ZPage::StampCensusBoundary()
    {
        uintptr_t offset = GetRegionAllocPtr() - GetRegionStart();
        _scratch.censusBoundaryOffset =
            static_cast<uint32_t>(std::min<uintptr_t>(offset, std::numeric_limits<uint32_t>::max()));
    }

// zPage.cpp:42: _livemap(object_max_count()). Constructed once per page life.
inline void ZPage::InitializeLiveMap()
    {
        CHECK(livemap() == nullptr);
        ZLiveMap* live = new ZLiveMap(object_max_count());
        __atomic_store_n(&_scratch.livemap, live, std::memory_order_release);
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
inline uint32_t ZPage::object_max_count() const
{
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
    return livemap()->is_marked(generation_id());
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
    return livemap()->get(generation_id(), index);
}

inline bool ZPage::is_strong_bit_set(zaddress addr) const
{
    DCHECK_D(IsRelocatable(), "Invalid page state");
    const BitMap::idx_t index = bit_index(addr);
    return livemap()->get(generation_id(), index + 1);
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
    return livemap()->set(generation_id(), index, finalizable, inc_live);
}

// zPage.inline.hpp:296-317.
inline void ZPage::inc_live(uint32_t objects, size_t bytes)
{
    livemap()->inc_live(objects, bytes);
}

inline uint32_t ZPage::live_objects() const
{
    return livemap()->live_objects();
}

inline size_t ZPage::live_bytes() const
{
    return livemap()->live_bytes();
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

    livemap()->iterate(generation_id(), do_bit);
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
    const BitMap::idx_t base_index = livemap()->find_base_bit(index);
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

// zPage.cpp:115-117.
inline void ZPage::reset_livemap()
{
    livemap()->reset();
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
        // page iterator is active, immediate otherwise.
        safeDestroy.schedule_delete(new PageRetirement{ std::move(retire) });
    }

inline void ZPage::EnableSafeDestroy()
    {
        safeDestroy.enable_deferred_delete();
    }

inline void ZPage::DisableSafeDestroy()
    {
        safeDestroy.disable_deferred_delete();
    }

inline size_t ZPage::IndexedUnitCount(const std::vector<ZVirtualMemory>& ranges)
    {
        std::vector<UnitSegment> segments;
        for (const ZVirtualMemory& range : ranges) {
            segments.push_back(UnitSegment{ untype(ZOffset::address_unsafe(range.start())), range.size(), 0 });
        }
        return IndexedUnitCount(segments);
    }

inline size_t ZPage::IndexedUnitCount(const std::vector<UnitSegment>& segments)
    {
        CHECK(UNIT_SIZE != 0 && (UNIT_SIZE & (UNIT_SIZE - 1)) == 0);
        size_t count = 0;
        uintptr_t previousEnd = 0;
        for (const auto& range : segments) {
            CHECK(range.size != 0 && IsRepresentableLow48Range(range.start, range.size));
            CHECK(range.start >= previousEnd);
            CHECK(range.start % UNIT_SIZE == 0 && range.size % UNIT_SIZE == 0);
            CHECK(CheckedAddSize(count, range.size / UNIT_SIZE + 1, count));
            previousEnd = range.End();
        }
        CHECK(count != 0 && count - 1 < std::numeric_limits<uint32_t>::max());
        return count - 1;
    }

inline void ZPage::InitializeSegments(uintptr_t metadataEnd, const std::vector<ZVirtualMemory>& ranges)
    {
        std::vector<UnitSegment> segments;
        for (const ZVirtualMemory& range : ranges) {
            segments.push_back(UnitSegment{ untype(ZOffset::address_unsafe(range.start())), range.size(), 0 });
        }
        InitializeSegments(metadataEnd, segments);
    }

inline void ZPage::InitializeSegments(uintptr_t metadataEnd, const std::vector<UnitSegment>& segments)
    {
        totalUnitCount = IndexedUnitCount(segments);
        heapStartAddress = metadataEnd;
        unitSegments.clear();
        size_t index = 0;
        for (const auto& range : segments) {
            CHECK(IsRepresentableLow48Range(range.start, range.size));
            unitSegments.push_back(UnitSegment{ range.start, range.size, index });
            index += range.size / UNIT_SIZE + 1;
        }
        ZPageTable::heap_table().map().Reset();
        CHECK(ZPageTable::heap_table().initialize(segments.front().start,
                                                 segments.back().End() - segments.front().start, UNIT_SIZE));
    }

inline size_t ZPage::FindUnitIndex(uintptr_t address)
    {
        auto next = std::upper_bound(unitSegments.begin(), unitSegments.end(), address,
            [](uintptr_t addr, const UnitSegment& segment) { return addr < segment.start; });
        if (next == unitSegments.begin()) {
            return INVALID_IDX;
        }
        const auto& segment = *std::prev(next);
        return address < segment.End()
            ? segment.firstIndex + (address - segment.start) / UNIT_SIZE : INVALID_IDX;
    }

inline bool ZPage::ContainsUnitRange(uintptr_t start, size_t size)
    {
        for (const auto& segment : unitSegments) {
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

inline ZPage* ZPage::GetZPage(uint32_t idx)
{
    return ZPageTable::heap_table().get(GetUnitAddress(idx));
}

inline ZPage* ZPage::GetGhostFromRegionAt(uintptr_t allocAddr)
    {
        ZPage* region = ZPageTable::heap_table().get(allocAddr);
        if (region == nullptr || !region->IsGhostFromRegion()) {
            return nullptr;
        }
#if defined(MRT_GC_UNIT_TESTS)
        RunGhostLookupTestHook(region);
#endif
        return region;
    }

inline MAddress ZPage::GetUnitAddress(size_t idx)
    {
        CHECK(idx < totalUnitCount);
        for (const auto& segment : unitSegments) {
            if (idx >= segment.firstIndex && idx - segment.firstIndex < segment.size / UNIT_SIZE) {
                return segment.start + (idx - segment.firstIndex) * UNIT_SIZE;
            }
        }
        LOG(RTLOG_FATAL, "unit index denotes a reservation boundary: %zu", idx);
        return 0;
    }

inline void ZPage::InitFreeRegion(size_t unitIdx, size_t nUnit)
    {
        (void)unitIdx;
        (void)nUnit;
    }

inline ZPageType ZPageTypeFor(size_t nUnit, ZPageType uclass)
{
    if (uclass == ZPageType::large) {
        return ZPageType::large;
    }
    const size_t bytes = nUnit * ZPage::UNIT_SIZE;
    if (ZPageSizeMediumEnabled && bytes >= ZPageSizeMediumMin && bytes <= ZPageSizeMediumMax) {
        return ZPageType::medium;
    }
    return ZPageType::small;
}

inline ZPage* ZPage::InitRegion(size_t unitIdx, size_t nUnit, ZPageType uclass, PageAge age)
    {
        const MAddress start = GetUnitAddress(unitIdx);
        ZPage* region = new ZPage(ZPageTypeFor(nUnit, uclass), age,
                                            ZVirtualMemory(ZAddress::offset(to_zaddress_unsafe(start)),
                                                           nUnit * UNIT_SIZE));
        region->InitRegion(nUnit, uclass, age);
        return region;
    }

inline ZPage* ZPage::InitRegionAt(uintptr_t addr, size_t nUnit, ZPageType uclass)
    {
        size_t idx = ZPage::GetUnitIdxAt(addr);
        return InitRegion(idx, nUnit, uclass);
    }

inline void ZPage::WaitCopiedBeforePayloadWipe(ZPage* region, const char* site)
    {
        if (region == nullptr) {
            return;
        }
        (void)site;
        ZForwardingLife::WaitPageDone(region->_scratch.fwdOwner.load(std::memory_order_acquire));
    }

inline void ZPage::ClearUnits(size_t idx, size_t cnt)
    {
        uintptr_t unitAddress = ZPage::GetUnitAddress(idx);
        size_t size = cnt * ZPage::UNIT_SIZE;
        CHECK(ContainsUnitRange(unitAddress, size));
        ZPage* wipeRegion = Heap::page(unitAddress);
        WaitCopiedBeforePayloadWipe(wipeRegion, "ClearUnits");

        DLOG(REGION, "clear dirty units[%zu+%zu, %zu) @[%#zx+%zu, %#zx)", idx, cnt, idx + cnt, unitAddress, size,
             unitAddress + size);

        MapleRuntime::MemorySet(unitAddress, size, 0, size);
    }

inline bool ZPage::IsEmpty() const
    {
        MRT_ASSERT(IsSmallRegion(), "wrong region type");
        return GetRegionAllocPtr() == GetRegionStart();
    }

inline size_t ZPage::GetRegionSize() const
    {
        MAddress regionStart = GetRegionStart();
        DCHECK(_scratch.regionEnd > regionStart);
        return _scratch.regionEnd - regionStart;
    }

inline size_t ZPage::GetRegionSizeForDetachCheck() const
    {
        const MAddress start = GetRegionStart();
        const MAddress end = _scratch.regionEnd;
        return end > start && ContainsUnitRange(start, end - start) ? end - start : UNIT_SIZE;
    }

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

inline void ZPage::InitFreeUnits()
    {
        InitZPage(GetUnitCount(), ZPageType::small, PageAge::old, false);
    }





















inline bool ZPage::IsCompactRouteDestination(MAddress address) const
    {
        // The generation relocation set owns the sole from-to mapping.
        auto owner = ForwardingTable::RetainPageOwner(this);
        return IsCompacted() && owner && owner->find_from_by_to(address, nullptr);
    }





    template<Generation G>
inline void ZPage::PublishFromPageMetadata()
    {
        const RegionLifeId life = GetRegionLifeId();
        CHECK_DETAIL(ForwardingTable::PublishFromPageView(
                         this, livemap(), GetSnapshotEpoch(), GetRegionAllocPtr(), BirthSequence(),
                         static_cast<uint8_t>(G),
                         static_cast<uint8_t>(IsLargeRegion() && is_marked() &&
                                              is_live_bit_set(to_zaddress(GetRegionStart()))),
                         life),
                     "forwarding carrier missing at from-page publication region=%p", this);
    }

    template<Generation G>
inline __attribute__((always_inline)) void ZPage::PublishForwardingCarrier()
    {
        PublishFromPageMetadata<G>();
        SetInGhostRegion(1);
        _scratch.nextRegionIdx0 = _scratch.nextRegionIdx;
    }

    template<Generation G>
inline void ZPage::PrepareForwardableRegion()
    {
        CHECK(IsFromRegion());
        CHECK(is_small());
        CHECK(_scratch.inGhostFromRegion == 0);
        (void)IsForwardingDone();
        // The preceding generation reset removed its forwarding set.
        ClearRelocationResiduals();
        // PORT_ZFORWARDING step 1: same event, recorded address-keyed as well.  Populated in
        // parallel with the region machinery so the two answers can be compared before either is
        // trusted; nothing reads it for decisions yet.
        CHECK_DETAIL(ForwardingTable::InstallPublicationBeforeCopy(GetRegionStart(), GetRegionSize(), this, G),
                     "forwarding table install failed before relocation region=%p range=[%#zx,%#zx)",
                     this, static_cast<size_t>(GetRegionStart()), static_cast<size_t>(GetRegionEnd()));
        // enrolphase: which side of the relocate-start flip does this enrolment land on?
        //
        // OpenJDK installs the relocation set once, in the concurrent select_relocation_set
        // (zGeneration.cpp:254 ZRelocationSet::install), and only then flips
        // (relocate_start -> flip_relocate_start, :918 -> :922).  Post-flip the set is closed, so
        // "painted with the current colour after the flip" implies "will not move this cycle".
        //
        // EvacuateYoungRegions calls PrepareForwardTable<Young> twice -- WCollector.cpp:6483 and
        // again at :6952 -- with the young flip between them.  An enrolment on the far side leaves
        // a window in which a region is still NORMAL while the current colour is already the new
        // one, and a value painted there is load-good but names an object that is about to move.
        // That matches the measured FORWARD population exactly (afterFlip=1, slotGood=1, hasTo=1,
        // 20/20), and it is why moving only the first flip (kFlipAfterFromSpace) changed nothing.
        //
        // The staleness predicate is not the hole: over ~2^20 non-NORMAL targets per run, across
        // six runs, zero escaped it.
        // gc_unit fixtures do not run Heap::Init, so CollectorProxy has no
        // current collector for this diagnostic-only phase sample.  Skip only
        // in the test configuration; product (macro off) always samples.
#if !defined(MRT_GC_UNIT_TESTS)
        NoteEnrolPhase();
#endif
        // Shared boundary: publish immutable from-page metadata, forwarding
        // construction token, and ghost membership through one product edge.
        PublishForwardingCarrier<G>();

    }

inline void ZPage::ClearGhostRegionBit()
    {
        if (IsGhostFromRegion()) {
            SetInGhostRegion(0);
        }
    }

inline void ZPage::ClearGhostFromRegionBits()
    {
        SetInGhostRegion(0);
    }

inline void ZPage::DispelGhostFromRegion()
    {
        // fwdinflight: this is one of the three edges that retire from-side route state, and
        // it is unconditional -- nothing here waits for a reader. ZGC's equivalent,
        // ZForwarding::detach_page (zForwarding.cpp:171-181), blocks until _ref_count is zero.
        // Count what we would be invalidating. Default off; never blocks.

        // portmutreloc: hold the forwarding drain across the whole body. It is held
        // run while a retained reader is inside the route lookup or a mutator copy.
        InPlaceClaimScope drain(this, ZForwardingLife::Retire::DISPEL_GHOST);
        // PORT_ZFORWARDING step 1: the retirement edge.  ZGC's equivalent is refcount-driven
        // (ZForwarding::detach_page waits for _ref_count == 0); recording the removal here first
        // lets step 3 change *when* it happens without changing *where*.
        const size_t nUnit = GetGhostRegionUnitCount();
        ClearGhostFromRegionBits();
        dispelGhostCount.fetch_add(1, std::memory_order_relaxed);
        // fysfixb: name who clears the ghost bit (PrepareFromRegionList peer path).
        VLOG(REPORT,
             "[GCV2][ghost-dispel] region=%p start=%#zx nUnit=%zu live=%zu route=%u young=%u",
             this, GetRegionStart(), nUnit, livemap()->live_bytes(),
              IsForwardingDone() ? 1u : 0u,
             static_cast<unsigned>(IsYoungRegion()));
        // The old top/livemap disappeared with the forwarding carrier above;
        // only page-owned ghost/route state is reset in this body.
    }

inline bool ZPage::IsGhostFromRegion() const
    {
        const bool ghost = _scratch.regionStateBitField.GetAtomicValue(
            RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1) != 0;
        if (!ghost) {
            return false;
        }
        return __atomic_load_n(&_scratch.ghostLifeId, __ATOMIC_ACQUIRE) == GetRegionLifeId();
    }

inline void ZPage::AssertGhostClearedAfterReuse(size_t nUnit) const
    {
        CHECK(!IsGhostFromRegion());
        size_t baseIdx = GetUnitIdx();
        for (size_t i = 1; i < nUnit; i++) {
            MAddress addr = GetUnitAddress(baseIdx + i);
            CHECK(!InGhostFromRegion(from_region_addr(addr)));
        }
    }


inline bool ZPage::RetainForwarding()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->retain_page();
    }

inline void ZPage::ReleaseForwarding()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        CHECK(owner);
        owner->release_page();
    }

inline bool ZPage::ClaimForwarding()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->claim();
    }

inline void ZPage::MarkForwardingDone()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        if (owner && ZForwardingLife::CurrentPageWork() != owner.get()) owner->mark_done();
    }

inline bool ZPage::IsForwardingDone() const
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->is_done();
    }


inline int32_t ZPage::ForwardingRefCount() const
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner ? owner->ref_count().load(std::memory_order_acquire) : 0;
    }

inline bool ZPage::ForwardingClaimed() const
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->claimed().load(std::memory_order_acquire);
    }







inline void ZPage::SetInGhostRegion(uint8_t flag)
    {
        const RegionLifeId life = GetRegionLifeId();
        __atomic_store_n(&_scratch.ghostLifeId, life, __ATOMIC_RELEASE);
        _scratch.regionStateBitField.SetAtomicValue(RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1, flag);
        if (flag != 0) {
        }
    }

// ZPage::clone_for_promotion + ZPage::reset(age) (zPage.cpp:64-72, 103-113)
// on the reused slot: the young livemap is parked for the carrier's readers
// and the slot continues with a fresh map; readers see one or the other,
// never a missing map.
inline void ZPage::PromoteYoungRegion()
    {
        CHECK_DETAIL(IsYoungRegion(), "cannot promote an old region %p", this);
        CHECK_DETAIL(_scratch.retiredLivemap == nullptr, "region %p promoted twice in one life", this);
        ZLiveMap* fresh = new ZLiveMap(object_max_count());
        SetYoungRegionFlag(0);
        SetYoungAge(0);
        ResetPageSequence();
        _scratch.retiredLivemap = livemap();
        __atomic_store_n(&_scratch.livemap, fresh, std::memory_order_release);
    }

inline void ZPage::SetYoungAge(uint8_t age)
    {
        CHECK(age <= MAX_YOUNG_AGE);
        _age = age == 0 ? PageAge::old : static_cast<PageAge>(age);
        _scratch.regionStateBitField.SetAtomicValue(RegionStateBitPos::YOUNG_AGE_FLAG, YOUNG_AGE_BIT_LENGTH, age);
    }

inline uint8_t ZPage::GetYoungAge() const
    {
        return static_cast<uint8_t>(_scratch.regionStateBitField.GetAtomicValue(
                                        RegionStateBitPos::YOUNG_AGE_FLAG, YOUNG_AGE_BIT_LENGTH) >>
                                    RegionStateBitPos::YOUNG_AGE_FLAG);
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

inline int32_t ZPage::IncRawPointerObjectCount()
    {
        int32_t oldCount = __atomic_fetch_add(&_scratch.rawPointerObjectCount, 1, __ATOMIC_SEQ_CST);
        CHECK_DETAIL(oldCount >= 0, "region %p has wrong raw pointer count %d", this);
        CHECK_DETAIL(oldCount < MAX_RAW_POINTER_COUNT, "inc raw-pointer-count overflow");
        return oldCount;
    }

inline int32_t ZPage::DecRawPointerObjectCount()
    {
        int32_t oldCount = __atomic_fetch_sub(&_scratch.rawPointerObjectCount, 1, __ATOMIC_SEQ_CST);
        CHECK_DETAIL(oldCount > 0, "dec raw-pointer-count underflow, please check whether releaseRawData is overused.");
        return oldCount;
    }

inline bool ZPage::CompareAndSwapRawPointerObjectCount(int32_t expectVal, int32_t newVal)
    {
        return __atomic_compare_exchange_n(&_scratch.rawPointerObjectCount, &expectVal, newVal, false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_ACQUIRE);
    }

inline uintptr_t ZPage::Alloc(size_t size)
    {
        size_t limit = GetRegionEnd();
        if (_scratch.allocPtr + size <= limit) {
            uintptr_t addr = _scratch.allocPtr;
            _scratch.allocPtr += size;
            return addr;
        } else {
            return 0;
        }
    }

inline uintptr_t ZPage::AtomicAlloc(size_t size)
    {
        // zPage.inline.hpp:451-479: reject an out-of-page top before CAS.
        // Failed allocations must never move the published allocation frontier.
        uintptr_t addr = __atomic_load_n(&_scratch.allocPtr, __ATOMIC_ACQUIRE);
        const uintptr_t limit = GetRegionEnd();
        for (;;) {
            if (addr > limit || size > limit - addr) {
                return 0;
            }
            const uintptr_t next = addr + size;
            if (__atomic_compare_exchange_n(&_scratch.allocPtr, &addr, next, false,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return addr;
            }
        }
    }

inline bool ZPage::UndoAllocObjectAtomic(uintptr_t addr, size_t size)
    {
        uintptr_t expected = addr + size;
        return __atomic_compare_exchange_n(&_scratch.allocPtr, &expected, addr, false, __ATOMIC_ACQ_REL,
                                           __ATOMIC_ACQUIRE);
    }

inline bool ZPage::IsPinnedRegion() const
    {
        return OnNamedList("old pinned regions") || OnNamedList("recent pinned regions");
    }

inline ZPage* ZPage::GetPrevRegion() const
    {
        if (UNLIKELY(_scratch.prevRegionIdx == NULLPTR_IDX)) {
            return nullptr;
        }
        return ZPageTable::heap_table().get(GetUnitAddress(_scratch.prevRegionIdx));
    }

inline void ZPage::SetPrevRegion(const ZPage* r)
    {
        if (UNLIKELY(r == nullptr)) {
            _scratch.prevRegionIdx = NULLPTR_IDX;
            return;
        }
        size_t prevIdx = r->GetUnitIdx();
        MRT_ASSERT(prevIdx < NULLPTR_IDX, "exceeds the maximum limit for region info");
        _scratch.prevRegionIdx = static_cast<uint32_t>(prevIdx);
    }

inline ZPage* ZPage::GetNextRegion() const
    {
        if (UNLIKELY(_scratch.nextRegionIdx == NULLPTR_IDX)) {
            return nullptr;
        }
        DCHECK(_scratch.nextRegionIdx < totalUnitCount);
        return ZPageTable::heap_table().get(GetUnitAddress(_scratch.nextRegionIdx));
    }

inline ZPage* ZPage::GetNextGhostRegion() const
    {
        if (UNLIKELY(_scratch.nextRegionIdx0 == NULLPTR_IDX)) {
            return nullptr;
        }
        DCHECK(_scratch.nextRegionIdx0 < totalUnitCount);
        return ZPageTable::heap_table().get(GetUnitAddress(_scratch.nextRegionIdx0));
    }

inline void ZPage::SetNextRegion(const ZPage* r)
    {
        if (UNLIKELY(r == nullptr)) {
            _scratch.nextRegionIdx = NULLPTR_IDX;
            return;
        }
        size_t nextIdx = r->GetUnitIdx();
        MRT_ASSERT(nextIdx < NULLPTR_IDX, "exceeds the maximum limit for region info");
        _scratch.nextRegionIdx = static_cast<uint32_t>(nextIdx);
    }

inline bool ZPage::IsUnmovableFromRegion() const
    {
        return OnNamedList("escaped from regions") || OnNamedList("raw pointer pinned regions");
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
inline void ZPage::RemoveFromList()
    {
        ZPage* prev = GetPrevRegion();
        ZPage* next = GetNextRegion();
        if (prev != nullptr) {
            prev->SetNextRegion(next);
        }
        if (next != nullptr) {
            next->SetPrevRegion(prev);
        }
        this->SetNextRegion(nullptr);
        this->SetPrevRegion(nullptr);
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

inline void ZPage::InitZPage(size_t nUnit, ZPageType uClass, PageAge age, bool live)
    {
        CHECK(ContainsUnitRange(GetRegionStart(), nUnit * UNIT_SIZE));
        CHECK(ZPageTable::heap_table().get(GetRegionStart()) == nullptr);
        CHECK_DETAIL(GetRegionListOwner() == nullptr, "reinitializing a region still owned by a list");

        // Invalidate every old-life carrier before clearing any of its payload.
        // Readers either retain the old page (detachgate) or observe this bump and
        // reject the old incarnation; there is no wraparound fallback.
        BumpRegionLifeId();
        {
            uint8_t cur = __atomic_load_n(&_scratch.regionLifeSequence, __ATOMIC_RELAXED);
            uint8_t next = static_cast<uint8_t>((cur + 1) & 0x7f);
            __atomic_store_n(&_scratch.regionLifeSequence, next, __ATOMIC_RELEASE);
        }
        // See DispelGhostFromRegion: retire the route before detaching its compact table.
        ForwardingTable::ClearPageOwner(this);
        WaitCopiedBeforePayloadWipe(this, "InitZPage");
        // ZPageAllocator::safe_destroy_page: the previous page life's livemap
        // (and a promotion's parked young map) goes with it (~ZPage / ~CHeapBitMap).
        delete livemap();
        __atomic_store_n(&_scratch.livemap, static_cast<ZLiveMap*>(nullptr), std::memory_order_release);
        delete _scratch.retiredLivemap;
        _scratch.retiredLivemap = nullptr;
        SetYoungRegionFlag(0);
        _scratch.allocPtr = GetRegionStart();
        _scratch.regionEnd = _scratch.allocPtr + nUnit * ZPage::UNIT_SIZE;
        // ZPage::reset(age), zPage.cpp:103-108: establish identity before page-table publication.
        SetYoungRegionFlag(age != PageAge::old);
        SetYoungAge(age == PageAge::old ? 0 : static_cast<uint8_t>(untype(age)));
        ResetPageSequence();
        _scratch.prevRegionIdx = NULLPTR_IDX;
        _scratch.nextRegionIdx = NULLPTR_IDX;
        // Ghost walk (PrepareFromRegionList) follows nextRegionIdx0. A reused
        // region that still named its previous-life successor kept a retired
        // from-space chain alive across InitRegion (RegionManager.h:782).
        _scratch.nextRegionIdx0 = NULLPTR_IDX;
        _scratch.regionListOwner.store(nullptr, std::memory_order_relaxed);
        _scratch.censusBoundaryOffset = 0;

        // routedest: this is the reuse edge named in the defect. TakeRegion has already run
        // ClearUnits over this payload; if a published route still names this region, the
        // route now answers into zeroed (or freshly re-allocated) memory. Count it here
        // rather than at ClearUnits because this is the one call that runs exactly once per
        // reuse. The hold is deliberately NOT cleared: reaching this point while held means
        // a reclaim gate was bypassed, and leaving the flag set keeps the region out of the
        // next collection set instead of silently papering over the escape.
        SetRegionListOwner(nullptr);

        SetInGhostRegion(0);
        __atomic_store_n(&_scratch.rawPointerObjectCount, 0, __ATOMIC_SEQ_CST);
        (void)uClass;
        if (live) {
            InitializeLiveMap();
        }
    }

inline void ZPage::InitRegion(size_t nUnit, ZPageType uClass, PageAge age)
    {
        InitZPage(nUnit, uClass, age, true);
        (void)nUnit;
    }

} // namespace MapleRuntime
#endif

namespace MapleRuntime {
inline ZGenerationId ZPage::generation_id() const
{
    return _generation_id;
}
}

#include "Heap/Allocator/ZPage.h"

namespace MapleRuntime {
inline Generation ZPage::GetOwnerGeneration() const
    {
        return IsYoungRegion() ? Generation::Young : Generation::Old;
    }
}

namespace MapleRuntime {
inline bool ZPage::IsYoungRegion() const
    {
        return generation_id() == ZGenerationId::young;
    }
}

namespace MapleRuntime {
inline MAddress ZPage::GetRegionStart() const
{
    if (!_virtual.is_null()) {
        return untype(ZOffset::address_unsafe(_virtual.start()));
    }
    return _scratch.allocPtr;
}
}

namespace MapleRuntime {
inline MAddress ZPage::GetRegionEnd() const { return _scratch.regionEnd; }
}

namespace MapleRuntime {
inline MAddress ZPage::GetRegionAllocPtr() const { return _scratch.allocPtr; }
}

namespace MapleRuntime {
inline bool ZPage::IsSmallRegion() const { return is_small(); }
}

namespace MapleRuntime {
inline bool ZPage::IsLargeRegion() const { return is_large(); }
}

namespace MapleRuntime {
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
}
