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

inline bool RegionInfo::IsCompacted() const
    {
        auto owner = ForwardingTable::RetainPageOwner(const_cast<RegionInfo*>(this));
        return owner && owner->is_done() && owner->in_place();
    }

inline bool RegionInfo::IsRoutingState()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->is_claimed() && !owner->is_done();
    }

inline ZLiveMap* RegionInfo::livemap() const
{
    return __atomic_load_n(&metadata.livemap, std::memory_order_acquire);
}

inline ZForwarding* RegionInfo::GetFromPageCarrier() const
    {
        ZForwarding* carrier = ForwardingTable::RetainPageOwner(this).get();
        return carrier != nullptr && carrier->page() == this ? carrier : nullptr;
    }

inline bool RegionInfo::HasFromPageMetadata() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from != nullptr && (from->lifeId == GetRegionLifeId());
    }

inline ZLiveMap* RegionInfo::FromPageLiveMap() const
{
    const ZForwarding::FromPageView* from = GetFromPageView();
    return from == nullptr ? nullptr : from->livemap;
}

inline Generation RegionInfo::GetRouteMarkGeneration() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from == nullptr ? GetOwnerGeneration() : static_cast<Generation>(from->owner);
    }

inline bool RegionInfo::IsFromPageAllocating() const
{
    const ZForwarding::FromPageView* from = GetFromPageView();
    return from != nullptr && from->birthSequence == from->epoch;
}

// The from page is read through the livemap the carrier retained; the page
// identity (generation, geometry) is the one published with it.
inline bool RegionInfo::IsFromPageSurvivedObject(size_t offset) const
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

inline bool RegionInfo::IsRouteSurvivedObject(size_t offset)
    {
        if (!HasFromPageMetadata()) {
            return is_object_live(to_zaddress(GetRegionStart() + offset));
        }
        return IsFromPageSurvivedObject(offset);
    }

inline bool RegionInfo::IsRouteMarkedObject(size_t offset)
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

inline bool RegionInfo::IsRouteKnownEmpty()
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

inline void RegionInfo::BindFromPageLiveMapIfNull()
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

inline void RegionInfo::StampCensusBoundary()
    {
        uintptr_t offset = GetRegionAllocPtr() - GetRegionStart();
        metadata.censusBoundaryOffset =
            static_cast<uint32_t>(std::min<uintptr_t>(offset, std::numeric_limits<uint32_t>::max()));
    }

// zPage.cpp:42: _livemap(object_max_count()). Constructed once per page life.
inline void RegionInfo::InitializeLiveMap()
    {
        CHECK(livemap() == nullptr);
        ZLiveMap* live = new ZLiveMap(object_max_count());
        __atomic_store_n(&metadata.livemap, live, std::memory_order_release);
    }

// ---- ZPage livemap surface ----

// zPage.inline.hpp:72-101 object_alignment_shift: large pages hold one object
// at start; small pages use the minimum object alignment.
inline int RegionInfo::object_alignment_shift() const
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

inline size_t RegionInfo::object_alignment() const
{
    return size_t(1) << object_alignment_shift();
}

// zPage.inline.hpp:57-70 object_max_count.
inline uint32_t RegionInfo::object_max_count() const
{
    if (type() == ZPageType::large) {
        return 1;
    }
    return static_cast<uint32_t>(GetRegionSize() >> object_alignment_shift());
}

// zPage.inline.hpp:188-195 is_in: [start, top).
inline bool RegionInfo::is_in(zaddress addr) const
{
    const MAddress address = raw(addr);
    return address >= GetRegionStart() && address < GetRegionAllocPtr();
}

// zPage.inline.hpp:223-226.
inline bool RegionInfo::is_marked() const
{
    DCHECK_D(IsRelocatable(), "Invalid page state");
    return livemap()->is_marked(generation_id());
}

// zPage.inline.hpp:228-230.
inline BitMap::idx_t RegionInfo::bit_index(zaddress addr) const
{
    return (GetAddressOffset(raw(addr)) >> object_alignment_shift()) * 2;
}

// zPage.inline.hpp:232-235.
inline MAddress RegionInfo::offset_from_bit_index(BitMap::idx_t index) const
{
    const uintptr_t l_offset = ((index / 2) << object_alignment_shift());
    return GetRegionStart() + l_offset;
}

// zPage.inline.hpp:237-240.
inline BaseObject* RegionInfo::object_from_bit_index(BitMap::idx_t index) const
{
    return from_region_addr(offset_from_bit_index(index));
}

// zPage.inline.hpp:242-252.
inline bool RegionInfo::is_live_bit_set(zaddress addr) const
{
    DCHECK_D(IsRelocatable(), "Invalid page state");
    const BitMap::idx_t index = bit_index(addr);
    return livemap()->get(generation_id(), index);
}

inline bool RegionInfo::is_strong_bit_set(zaddress addr) const
{
    DCHECK_D(IsRelocatable(), "Invalid page state");
    const BitMap::idx_t index = bit_index(addr);
    return livemap()->get(generation_id(), index + 1);
}

// zPage.inline.hpp:254-260: an allocating page is implicitly live.
inline bool RegionInfo::is_object_live(zaddress addr) const
{
    return IsAllocating() || is_live_bit_set(addr);
}

inline bool RegionInfo::is_object_strongly_live(zaddress addr) const
{
    return IsAllocating() || is_strong_bit_set(addr);
}

// zPage.inline.hpp:262-282: marking-only readers.
inline bool RegionInfo::is_object_marked_live(zaddress addr) const
{
    return is_object_live(addr);
}

inline bool RegionInfo::is_object_marked_strong(zaddress addr) const
{
    return is_object_strongly_live(addr);
}

inline bool RegionInfo::is_object_marked(zaddress addr, bool finalizable) const
{
    return finalizable ? is_object_marked_live(addr) : is_object_marked_strong(addr);
}

// zPage.inline.hpp:284-294.
inline bool RegionInfo::mark_object(zaddress addr, bool finalizable, bool& inc_live)
{
    DCHECK_D(IsRelocatable(), "Invalid page state");
    DCHECK_D(is_in(addr), "Invalid address");

    // Set mark bit
    const BitMap::idx_t index = bit_index(addr);
    return livemap()->set(generation_id(), index, finalizable, inc_live);
}

// zPage.inline.hpp:296-317.
inline void RegionInfo::inc_live(uint32_t objects, size_t bytes)
{
    livemap()->inc_live(objects, bytes);
}

inline uint32_t RegionInfo::live_objects() const
{
    return livemap()->live_objects();
}

inline size_t RegionInfo::live_bytes() const
{
    return livemap()->live_bytes();
}

// zPage.inline.hpp:319-331.
template <typename Function>
inline void RegionInfo::object_iterate(Function function)
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
inline MAddress RegionInfo::find_base_unsafe(MAddress p)
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
inline MAddress RegionInfo::find_base(MAddress p)
{
    DCHECK_D(is_marked(), "Should be marked");
    return find_base_unsafe(p);
}

// zPage.cpp:115-117.
inline void RegionInfo::reset_livemap()
{
    livemap()->reset();
}

inline ALWAYS_INLINE size_t RegionInfo::GetAddressOffset(MAddress address) const
    {
        DCHECK(GetRegionStart() <= address);
        return (address - GetRegionStart());
    }

inline void RegionInfo::RetirePage(RegionInfo* region, std::function<void()> retire)
    {
        CHECK(ZPageTable::heap_table().get(region->GetRegionStart()) == region);
        ZPageTable::heap_table().remove(region);
        // zPageAllocator.cpp:2248-2250 safe_destroy_page: deferred while any
        // page iterator is active, immediate otherwise.
        safeDestroy.schedule_delete(new PageRetirement{ std::move(retire) });
    }

inline void RegionInfo::EnableSafeDestroy()
    {
        safeDestroy.enable_deferred_delete();
    }

inline void RegionInfo::DisableSafeDestroy()
    {
        safeDestroy.disable_deferred_delete();
    }

inline size_t RegionInfo::IndexedUnitCount(const std::vector<ZVirtualMemory>& ranges)
    {
        std::vector<UnitSegment> segments;
        for (const ZVirtualMemory& range : ranges) {
            segments.push_back(UnitSegment{ untype(ZOffset::address_unsafe(range.start())), range.size(), 0 });
        }
        return IndexedUnitCount(segments);
    }

inline size_t RegionInfo::IndexedUnitCount(const std::vector<UnitSegment>& segments)
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

inline void RegionInfo::InitializeSegments(uintptr_t metadataEnd, const std::vector<ZVirtualMemory>& ranges)
    {
        std::vector<UnitSegment> segments;
        for (const ZVirtualMemory& range : ranges) {
            segments.push_back(UnitSegment{ untype(ZOffset::address_unsafe(range.start())), range.size(), 0 });
        }
        InitializeSegments(metadataEnd, segments);
    }

inline void RegionInfo::InitializeSegments(uintptr_t metadataEnd, const std::vector<UnitSegment>& segments)
    {
        UnitInfo::totalUnitCount = IndexedUnitCount(segments);
        UnitInfo::heapStartAddress = metadataEnd;
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

inline size_t RegionInfo::FindUnitIndex(uintptr_t address)
    {
        auto next = std::upper_bound(unitSegments.begin(), unitSegments.end(), address,
            [](uintptr_t addr, const UnitSegment& segment) { return addr < segment.start; });
        if (next == unitSegments.begin()) {
            return UnitInfo::INVALID_IDX;
        }
        const auto& segment = *std::prev(next);
        return address < segment.End()
            ? segment.firstIndex + (address - segment.start) / UNIT_SIZE : UnitInfo::INVALID_IDX;
    }

inline bool RegionInfo::ContainsUnitRange(uintptr_t start, size_t size)
    {
        for (const auto& segment : unitSegments) {
            if (start >= segment.start && start < segment.End() && size <= segment.End() - start) {
                return true;
            }
        }
        return false;
    }

inline void RegionInfo::VisitPageOwners(const std::function<void(RegionInfo*)>& visitor)
    {
        // zPageTable.cpp:83-98: the iterator's lifetime brackets safe destroy,
        // including nested iteration and exceptional callback exits.
        SafeDestroyScope iteration;
        ZPageTableIterator iter(&ZPageTable::heap_table());
        RegionInfo* page = nullptr;
        while (iter.next(&page)) {
            visitor(page);
        }
    }

inline ALWAYS_INLINE RegionInfo* RegionInfo::TryGetRegionInfoAt(uintptr_t allocAddr)
    {
        return ZPageTable::heap_table().get(allocAddr);
    }

inline RegionInfo* RegionInfo::GetRegionInfoAt(uintptr_t allocAddr)
    {
        RegionInfo* region = TryGetRegionInfoAt(allocAddr);
        CHECK_DETAIL(region != nullptr, "heap address %#zx has no owning region", allocAddr);
        return region;
    }

inline RegionInfo* RegionInfo::GetGhostFromRegionAt(uintptr_t allocAddr)
    {
        const size_t idx = FindUnitIndex(allocAddr);
        if (idx == UnitInfo::INVALID_IDX) {
            return nullptr;
        }
        UnitInfo* unit = UnitInfo::GetUnitInfo(idx);
        if (unit->GetMetadata().regionStateBitField.GetAtomicValue(
                RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1) == 0) {
            return nullptr;
        }
        RegionInfo* region = LoadUnitRole0(unit) == UnitRole::SUBORDINATE_UNIT
            ? unit->GetMetadata().ownerRegion0 : reinterpret_cast<RegionInfo*>(unit);
        if (region == nullptr ||
            __atomic_load_n(&unit->GetMetadata().ghostLifeId, __ATOMIC_ACQUIRE) !=
                region->GetRegionLifeId()) {
            return nullptr;
        }
#if defined(MRT_GC_UNIT_TESTS)
        RunGhostLookupTestHook(region);
#endif
        return region;
    }

inline void RegionInfo::InitFreeRegion(size_t unitIdx, size_t nUnit)
    {
        RegionInfo* region = reinterpret_cast<RegionInfo*>(RegionInfo::UnitInfo::GetUnitInfo(unitIdx));
        region->InitRegionInfo(nUnit, UnitRole::FREE_UNITS);
    }

inline ZPageType RegionInfoTypeFor(size_t nUnit, RegionInfo::UnitRole uclass)
{
    if (uclass == RegionInfo::UnitRole::LARGE_SIZED_UNITS) {
        return ZPageType::large;
    }
    const size_t bytes = nUnit * RegionInfo::UNIT_SIZE;
    if (ZPageSizeMediumEnabled && bytes >= ZPageSizeMediumMin && bytes <= ZPageSizeMediumMax) {
        return ZPageType::medium;
    }
    return ZPageType::small;
}

inline RegionInfo* RegionInfo::InitRegion(size_t unitIdx, size_t nUnit, RegionInfo::UnitRole uclass, PageAge age)
    {
        const MAddress start = GetUnitAddress(unitIdx);
        RegionInfo* region = new RegionInfo(RegionInfoTypeFor(nUnit, uclass), age,
                                            ZVirtualMemory(ZAddress::offset(to_zaddress_unsafe(start)),
                                                           nUnit * UNIT_SIZE));
        region->InitRegion(nUnit, uclass, age);
        return region;
    }

inline RegionInfo* RegionInfo::InitRegionAt(uintptr_t addr, size_t nUnit, RegionInfo::UnitRole uclass)
    {
        size_t idx = RegionInfo::UnitInfo::GetUnitIdxAt(addr);
        return InitRegion(idx, nUnit, uclass);
    }

inline void RegionInfo::WaitCopiedBeforePayloadWipe(RegionInfo* region, const char* site)
    {
        if (region == nullptr) {
            return;
        }
        (void)site;
        ZForwardingLife::WaitPageDone(region->metadata.fwdOwner.load(std::memory_order_acquire));
    }

inline void RegionInfo::ClearUnits(size_t idx, size_t cnt)
    {
        uintptr_t unitAddress = RegionInfo::GetUnitAddress(idx);
        size_t size = cnt * RegionInfo::UNIT_SIZE;
        CHECK(ContainsUnitRange(unitAddress, size));
        RegionInfo* wipeRegion = RegionInfo::TryGetRegionInfoAt(unitAddress);
        WaitCopiedBeforePayloadWipe(wipeRegion, "ClearUnits");

        DLOG(REGION, "clear dirty units[%zu+%zu, %zu) @[%#zx+%zu, %#zx)", idx, cnt, idx + cnt, unitAddress, size,
             unitAddress + size);

        MapleRuntime::MemorySet(unitAddress, size, 0, size);
    }

inline bool RegionInfo::IsEmpty() const
    {
        MRT_ASSERT(IsSmallRegion(), "wrong region type");
        return GetRegionAllocPtr() == GetRegionStart();
    }

inline size_t RegionInfo::GetRegionSize() const
    {
        MAddress regionStart = GetRegionStart();
        DCHECK(metadata.regionEnd > regionStart);
        return metadata.regionEnd - regionStart;
    }

inline size_t RegionInfo::GetRegionSizeForDetachCheck() const
    {
        const MAddress start = GetRegionStart();
        const MAddress end = metadata.regionEnd;
        return end > start && ContainsUnitRange(start, end - start) ? end - start : UNIT_SIZE;
    }

inline size_t RegionInfo::GetGhostRegionSize() const
    {
        // The old extent follows the forwarding incarnation. If no carrier is
        // installed (idle/test setup), the only valid extent is the page's
        // current own size.
        ZForwarding* carrier = GetFromPageCarrier();
        return carrier == nullptr ? GetRegionSize() : carrier->size();
    }

inline size_t RegionInfo::GetAvailableSize() const
    {
        MRT_ASSERT(IsSmallRegion(), "wrong region type");
        return GetRegionEnd() - GetRegionAllocPtr();
    }

inline void RegionInfo::InitFreeUnits()
    {
        InitRegionInfo(GetUnitCount(), UnitRole::FREE_UNITS);
    }





















inline bool RegionInfo::IsCompactRouteDestination(MAddress address) const
    {
        // The generation relocation set owns the sole from-to mapping.
        auto owner = ForwardingTable::RetainPageOwner(this);
        return IsCompacted() && owner && owner->find_from_by_to(address, nullptr);
    }





    template<Generation G>
inline void RegionInfo::PublishFromPageMetadata()
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
inline __attribute__((always_inline)) void RegionInfo::PublishForwardingCarrier()
    {
        SetUnitRole0(static_cast<UnitRole>(metadata.unitRole));
        PublishFromPageMetadata<G>();
        // zForwarding.inline.hpp:67-70 — construction token = 1. Late retain
        // after detach (count 0) is refused; carrier and token are published
        // by this single product operation.

        // zRelocationSet.cpp:110-118 / zVerify.cpp:601: the forwarding still
        // reads this source page's livemap. Publication does not reset it;
        // detach/reuse owns that transition after source-page consumers finish.
        // Always install ghost membership, including a zero-live page. This is
        // what keeps the from-page carrier reachable until forwarding drain.
        SetInGhostRegion(1);
        metadata.nextRegionIdx0 = metadata.nextRegionIdx;

        size_t nUnit = GetUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 1; i < nUnit; i++) {
            UnitMetadata& mdata = array[i].GetMetadata();
            CHECK(static_cast<UnitRole>(mdata.unitRole) == UnitRole::SUBORDINATE_UNIT);
            CHECK(mdata.ownerRegion == this);
            CHECK(mdata.inGhostFromRegion == 0);
            array[i].SetUnitRole0(UnitRole::SUBORDINATE_UNIT);
            mdata.ownerRegion0 = this;
            array[i].SetInGhostRegion(1, GetRegionLifeId());
        }
    }

    template<Generation G>
inline void RegionInfo::PrepareForwardableRegion()
    {
        CHECK(IsFromRegion());
        CHECK(static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS);
        CHECK(metadata.inGhostFromRegion == 0);
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

inline void RegionInfo::ClearGhostRegionBit()
    {
        if (IsGhostFromRegion()) {
            size_t nUnit = GetUnitCount();
            UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
            UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
            for (size_t i = 0; i < nUnit; i++) {
                array[i].SetInGhostRegion(0, GetRegionLifeId());
            }
        }
    }

inline void RegionInfo::ClearGhostFromRegionBits()
    {
        const size_t nUnit = GetGhostRegionUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 0; i < nUnit; i++) {
            array[i].SetInGhostRegion(0, GetRegionLifeId());
        }
    }

inline void RegionInfo::DispelGhostFromRegion()
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

inline bool RegionInfo::IsGhostFromRegion() const
    {
        const bool ghost = metadata.regionStateBitField.GetAtomicValue(
            RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1) != 0;
        if (!ghost) {
            return false;
        }
        return __atomic_load_n(&metadata.ghostLifeId, __ATOMIC_ACQUIRE) == GetRegionLifeId();
    }

inline void RegionInfo::AssertGhostClearedAfterReuse(size_t nUnit) const
    {
        CHECK(!IsGhostFromRegion());
        size_t baseIdx = GetUnitIdx();
        for (size_t i = 1; i < nUnit; i++) {
            MAddress addr = GetUnitAddress(baseIdx + i);
            CHECK(!InGhostFromRegion(from_region_addr(addr)));
        }
    }


inline bool RegionInfo::RetainForwarding()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->retain_page();
    }

inline void RegionInfo::ReleaseForwarding()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        CHECK(owner);
        owner->release_page();
    }

inline bool RegionInfo::ClaimForwarding()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->claim();
    }

inline void RegionInfo::MarkForwardingDone()
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        if (owner && ZForwardingLife::CurrentPageWork() != owner.get()) owner->mark_done();
    }

inline bool RegionInfo::IsForwardingDone() const
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->is_done();
    }


inline int32_t RegionInfo::ForwardingRefCount() const
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner ? owner->ref_count().load(std::memory_order_acquire) : 0;
    }

inline bool RegionInfo::ForwardingClaimed() const
    {
        auto owner = ForwardingTable::RetainPageOwner(this);
        return owner && owner->claimed().load(std::memory_order_acquire);
    }

inline void RegionInfo::SetRegionType(RegionType type)
    {
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::REGION_TYPE_FLAG, BIT_LENGTH,
                                                    static_cast<uint8_t>(type));
    }

inline void RegionInfo::SetTraceRegionFlag(uint8_t flag)
    {
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::TRACE_REGION_FLAG, 1, flag);
    }



inline void RegionInfo::SetInGhostRegion(uint8_t flag)
    {
        const RegionLifeId life = GetRegionLifeId();
        __atomic_store_n(&metadata.ghostLifeId, life, __ATOMIC_RELEASE);
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1, flag);
        if (flag != 0) {
        }
    }

// ZPage::clone_for_promotion + ZPage::reset(age) (zPage.cpp:64-72, 103-113)
// on the reused slot: the young livemap is parked for the carrier's readers
// and the slot continues with a fresh map; readers see one or the other,
// never a missing map.
inline void RegionInfo::PromoteYoungRegion()
    {
        CHECK_DETAIL(IsYoungRegion(), "cannot promote an old region %p", this);
        CHECK_DETAIL(metadata.retiredLivemap == nullptr, "region %p promoted twice in one life", this);
        ZLiveMap* fresh = new ZLiveMap(object_max_count());
        SetYoungRegionFlag(0);
        SetYoungAge(0);
        ResetPageSequence();
        metadata.retiredLivemap = livemap();
        __atomic_store_n(&metadata.livemap, fresh, std::memory_order_release);
    }

inline void RegionInfo::SetYoungAge(uint8_t age)
    {
        CHECK(age <= MAX_YOUNG_AGE);
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::YOUNG_AGE_FLAG, YOUNG_AGE_BIT_LENGTH, age);
    }

inline uint8_t RegionInfo::GetYoungAge() const
    {
        return static_cast<uint8_t>(metadata.regionStateBitField.GetAtomicValue(
                                        RegionStateBitPos::YOUNG_AGE_FLAG, YOUNG_AGE_BIT_LENGTH) >>
                                    RegionStateBitPos::YOUNG_AGE_FLAG);
    }

inline RegionInfo::RegionType RegionInfo::GetRegionType() const
    {
        return static_cast<RegionType>(
            metadata.regionStateBitField.GetAtomicValue(RegionStateBitPos::REGION_TYPE_FLAG, BIT_LENGTH));
    }

// ZPage::is_allocating / is_relocatable (zPage.inline.hpp:180-186).
inline bool RegionInfo::IsAllocating() const
{
    return BirthSequence() == GetSnapshotEpoch();
}

inline bool RegionInfo::IsRelocatable() const
{
    return BirthSequence() < GetSnapshotEpoch();
}

inline int32_t RegionInfo::IncRawPointerObjectCount()
    {
        int32_t oldCount = __atomic_fetch_add(&metadata.rawPointerObjectCount, 1, __ATOMIC_SEQ_CST);
        CHECK_DETAIL(oldCount >= 0, "region %p has wrong raw pointer count %d", this);
        CHECK_DETAIL(oldCount < MAX_RAW_POINTER_COUNT, "inc raw-pointer-count overflow");
        return oldCount;
    }

inline int32_t RegionInfo::DecRawPointerObjectCount()
    {
        int32_t oldCount = __atomic_fetch_sub(&metadata.rawPointerObjectCount, 1, __ATOMIC_SEQ_CST);
        CHECK_DETAIL(oldCount > 0, "dec raw-pointer-count underflow, please check whether releaseRawData is overused.");
        return oldCount;
    }

inline bool RegionInfo::CompareAndSwapRawPointerObjectCount(int32_t expectVal, int32_t newVal)
    {
        return __atomic_compare_exchange_n(&metadata.rawPointerObjectCount, &expectVal, newVal, false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_ACQUIRE);
    }

inline uintptr_t RegionInfo::Alloc(size_t size)
    {
        size_t limit = GetRegionEnd();
        if (metadata.allocPtr + size <= limit) {
            uintptr_t addr = metadata.allocPtr;
            metadata.allocPtr += size;
            return addr;
        } else {
            return 0;
        }
    }

inline uintptr_t RegionInfo::AtomicAlloc(size_t size)
    {
        // zPage.inline.hpp:451-479: reject an out-of-page top before CAS.
        // Failed allocations must never move the published allocation frontier.
        uintptr_t addr = __atomic_load_n(&metadata.allocPtr, __ATOMIC_ACQUIRE);
        const uintptr_t limit = GetRegionEnd();
        for (;;) {
            if (addr > limit || size > limit - addr) {
                return 0;
            }
            const uintptr_t next = addr + size;
            if (__atomic_compare_exchange_n(&metadata.allocPtr, &addr, next, false,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return addr;
            }
        }
    }

inline bool RegionInfo::UndoAllocObjectAtomic(uintptr_t addr, size_t size)
    {
        uintptr_t expected = addr + size;
        return __atomic_compare_exchange_n(&metadata.allocPtr, &expected, addr, false, __ATOMIC_ACQ_REL,
                                           __ATOMIC_ACQUIRE);
    }

inline bool RegionInfo::IsPinnedRegion() const
    {
        return (static_cast<RegionType>(metadata.regionType) == RegionType::FULL_PINNED_REGION) ||
            (static_cast<RegionType>(metadata.regionType) == RegionType::RECENT_PINNED_REGION);
    }

inline RegionInfo* RegionInfo::GetPrevRegion() const
    {
        if (UNLIKELY(metadata.prevRegionIdx == NULLPTR_IDX)) {
            return nullptr;
        }
        return TryGetRegionInfoAt(GetUnitAddress(metadata.prevRegionIdx));
    }

inline void RegionInfo::SetPrevRegion(const RegionInfo* r)
    {
        if (UNLIKELY(r == nullptr)) {
            metadata.prevRegionIdx = NULLPTR_IDX;
            return;
        }
        size_t prevIdx = r->GetUnitIdx();
        MRT_ASSERT(prevIdx < NULLPTR_IDX, "exceeds the maximum limit for region info");
        metadata.prevRegionIdx = static_cast<uint32_t>(prevIdx);
    }

inline RegionInfo* RegionInfo::GetNextRegion() const
    {
        if (UNLIKELY(metadata.nextRegionIdx == NULLPTR_IDX)) {
            return nullptr;
        }
        DCHECK(metadata.nextRegionIdx < UnitInfo::totalUnitCount);
        return TryGetRegionInfoAt(GetUnitAddress(metadata.nextRegionIdx));
    }

inline RegionInfo* RegionInfo::GetNextGhostRegion() const
    {
        if (UNLIKELY(metadata.nextRegionIdx0 == NULLPTR_IDX)) {
            return nullptr;
        }
        DCHECK(metadata.nextRegionIdx0 < UnitInfo::totalUnitCount);
        return TryGetRegionInfoAt(GetUnitAddress(metadata.nextRegionIdx0));
    }

inline void RegionInfo::SetNextRegion(const RegionInfo* r)
    {
        if (UNLIKELY(r == nullptr)) {
            metadata.nextRegionIdx = NULLPTR_IDX;
            return;
        }
        size_t nextIdx = r->GetUnitIdx();
        MRT_ASSERT(nextIdx < NULLPTR_IDX, "exceeds the maximum limit for region info");
        metadata.nextRegionIdx = static_cast<uint32_t>(nextIdx);
    }

inline bool RegionInfo::IsUnmovableFromRegion() const
    {
        RegionType type = GetRegionType();
        return type == RegionType::UNMOVABLE_FROM_REGION || type == RegionType::RAW_POINTER_PINNED_REGION;
    }

inline bool RegionInfo::IsValidRegion() const
    {
        return static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS ||
            static_cast<UnitRole>(metadata.unitRole) == UnitRole::LARGE_SIZED_UNITS;
    }

// zRelocationSetSelector.cpp:114-196 / zGeneration.cpp:216-221: a relocatable
// page that was marked this cycle and has no live bytes is garbage. A page not
// marked this cycle is not known empty here (kept for the selector's ZGC
// convergence in the page-descriptor package).
inline bool RegionInfo::IsKnownEmpty() const
    {
        if (IsAllocating()) {
            return false;
        }
        return is_marked() && live_bytes() == 0;
    }

inline bool RegionInfo::IsKnownYoungEmpty() const
    {
        if (IsAllocating()) {
            return false;
        }
        return is_marked() && live_bytes() == 0;
    }

inline bool RegionInfo::IsSafeKnownEmpty()
    {
        return IsKnownEmpty();
    }

inline bool RegionInfo::IsSafeKnownYoungEmpty()
    {
        return IsKnownYoungEmpty();
    }
inline void RegionInfo::RemoveFromList()
    {
        RegionInfo* prev = GetPrevRegion();
        RegionInfo* next = GetNextRegion();
        if (prev != nullptr) {
            prev->SetNextRegion(next);
        }
        if (next != nullptr) {
            next->SetPrevRegion(prev);
        }
        this->SetNextRegion(nullptr);
        this->SetPrevRegion(nullptr);
    }












inline RegionInfo::UnitRole RegionInfo::LoadUnitRole0(UnitInfo* unit)
    {
        return static_cast<UnitRole>(
            unit->GetMetadata().unitRoleBitField.GetAtomicValue(BIT_LENGTH, BIT_LENGTH) >> BIT_LENGTH);
    }

inline void RegionInfo::BumpRegionLifeId()
    {
        RegionLifeId old = metadata.regionLifeId.load(std::memory_order_relaxed);
        for (;;) {
            if (UNLIKELY(old == std::numeric_limits<RegionLifeId>::max())) {
                LOG(RTLOG_FATAL,
                    "[LIFECLOCK][REGION_LIFE_ID_OVERFLOW] region=%p life=%llu; wraparound is forbidden",
                    this, static_cast<unsigned long long>(old));
                return;
            }
            if (metadata.regionLifeId.compare_exchange_weak(old, old + 1, std::memory_order_release,
                                                            std::memory_order_relaxed)) {
                return;
            }
        }
    }

inline void RegionInfo::InitRegionInfo(size_t nUnit, UnitRole uClass, PageAge age)
    {
        CHECK(ContainsUnitRange(GetRegionStart(), nUnit * UNIT_SIZE));
        CHECK(TryGetRegionInfoAt(GetRegionStart()) == nullptr);
        CHECK_DETAIL(GetRegionListOwner() == nullptr, "reinitializing a region still owned by a list");

        SetUnitRole(UnitRole::FREE_UNITS);
        // Invalidate every old-life carrier before clearing any of its payload.
        // Readers either retain the old page (detachgate) or observe this bump and
        // reject the old incarnation; there is no wraparound fallback.
        BumpRegionLifeId();
        {
            uint8_t cur = __atomic_load_n(&metadata.regionLifeSequence, __ATOMIC_RELAXED);
            uint8_t next = static_cast<uint8_t>((cur + 1) & 0x7f);
            __atomic_store_n(&metadata.regionLifeSequence, next, __ATOMIC_RELEASE);
        }
        // See DispelGhostFromRegion: retire the route before detaching its compact table.
        ForwardingTable::ClearPageOwner(this);
        WaitCopiedBeforePayloadWipe(this, "InitRegionInfo");
        // ZPageAllocator::safe_destroy_page: the previous page life's livemap
        // (and a promotion's parked young map) goes with it (~ZPage / ~CHeapBitMap).
        delete livemap();
        __atomic_store_n(&metadata.livemap, static_cast<ZLiveMap*>(nullptr), std::memory_order_release);
        delete metadata.retiredLivemap;
        metadata.retiredLivemap = nullptr;
        SetYoungRegionFlag(0);
        metadata.allocPtr = GetRegionStart();
        metadata.regionEnd = metadata.allocPtr + nUnit * RegionInfo::UNIT_SIZE;
        // ZPage::reset(age), zPage.cpp:103-108: establish identity before page-table publication.
        SetYoungRegionFlag(age != PageAge::old);
        SetYoungAge(age == PageAge::old ? 0 : static_cast<uint8_t>(untype(age)));
        ResetPageSequence();
        metadata.prevRegionIdx = NULLPTR_IDX;
        metadata.nextRegionIdx = NULLPTR_IDX;
        // Ghost walk (PrepareFromRegionList) follows nextRegionIdx0. A reused
        // region that still named its previous-life successor kept a retired
        // from-space chain alive across InitRegion (RegionManager.h:782).
        metadata.nextRegionIdx0 = NULLPTR_IDX;
        metadata.regionListOwner.store(nullptr, std::memory_order_relaxed);
        metadata.censusBoundaryOffset = 0;

        // routedest: this is the reuse edge named in the defect. TakeRegion has already run
        // ClearUnits over this payload; if a published route still names this region, the
        // route now answers into zeroed (or freshly re-allocated) memory. Count it here
        // rather than at ClearUnits because this is the one call that runs exactly once per
        // reuse. The hold is deliberately NOT cleared: reaching this point while held means
        // a reclaim gate was bypassed, and leaving the flag set keeps the region out of the
        // next collection set instead of silently papering over the escape.
        SetRegionType(RegionType::FREE_REGION);
        SetTraceRegionFlag(0);
        SetNotRelocatableThisCycle(0);
        // Ghost lives in unit metadata, not payload: ClearUnits cannot clear it.
        // TakeRegion reuses garbage without DispelGhostFromRegion.
        SetInGhostRegion(0);
        __atomic_store_n(&metadata.rawPointerObjectCount, 0, __ATOMIC_SEQ_CST);
        // ZPage::ZPage (zPage.cpp:33-42): _type is initialized before
        // _livemap(object_max_count()), which reads it. The role is the page
        // type here, so it is written before the livemap is sized from it.
        SetUnitRole(uClass);
        if (uClass != UnitRole::FREE_UNITS) {
            InitializeLiveMap();
        }
    }

inline void RegionInfo::InitRegion(size_t nUnit, UnitRole uClass, PageAge age)
    {
        InitRegionInfo(nUnit, uClass, age);
        CHECK(uClass != UnitRole::FREE_UNITS);
        ZPageTable::heap_table().insert(this);
    }

} // namespace MapleRuntime
#endif

namespace MapleRuntime {
inline ZGenerationId RegionInfo::generation_id() const
{
    ZGenerationId generation;
    __atomic_load(&metadata._generation_id, &generation, __ATOMIC_ACQUIRE);
    return generation;
}
}

#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
inline Generation RegionInfo::GetOwnerGeneration() const
    {
        return IsYoungRegion() ? Generation::Young : Generation::Old;
    }
}

namespace MapleRuntime {
inline bool RegionInfo::IsYoungRegion() const
    {
        return generation_id() == ZGenerationId::young;
    }
}

namespace MapleRuntime {
inline MAddress RegionInfo::GetRegionStart() const
{
    if (!_virtual.is_null()) {
        return untype(ZOffset::address_unsafe(_virtual.start()));
    }
    return metadata.allocPtr;
}
}

namespace MapleRuntime {
inline MAddress RegionInfo::GetRegionEnd() const { return metadata.regionEnd; }
}

namespace MapleRuntime {
inline MAddress RegionInfo::GetRegionAllocPtr() const { return metadata.allocPtr; }
}

namespace MapleRuntime {
inline bool RegionInfo::IsSmallRegion() const { return static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS; }
}

namespace MapleRuntime {
inline bool RegionInfo::IsLargeRegion() const { return static_cast<UnitRole>(metadata.unitRole) == UnitRole::LARGE_SIZED_UNITS; }
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
