// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_FORWARDING_H
#define MRT_Z_FORWARDING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Base/Panic.h"
#include "Heap/z/zForwardingEntry.hpp"
#include "Heap/z/zPageAge.hpp"
#include "Heap/z/zForwardingAllocator.hpp"
#include "Heap/z/zAttachedArray.hpp"

#include "Heap/z/zGeneration.hpp"

namespace MapleRuntime {
class BaseObject;

// loadfc (zBarrier.inline.hpp:327-343): best-effort detection verdict for slow/runtime hand-outs.
// The single relaxed header read classifies the observed word but establishes no lifetime
// guarantee. ZGC structurally cannot hand a from-address back after a slow-path miss
// (zGeneration.inline.hpp:131-140 has no "lookup miss ⇒ return from" exit); detected shapes are:
//   Forwarded   header stateCode=3, a to-version exists and must be found
//   ZeroHeader  payload cleared by reclamation (ClearPageMemory reuse) -- nothing to resolve
enum class HandVerdict : uint8_t { Usable, Forwarded, ZeroHeader };

// Provenance is captured by the runtime entry that owns the slot.  Resolution
// carries it down to the fail-closed exit instead of trying to reconstruct a
// holder from an address after the forwarding lookup has failed.
enum class ForwardingHolderKind : uint8_t {
    HeapRef,
    StackSlot,
    Remset,
    Static,
    Derived,
    StoreBuffer,
    Unknown,
};

enum class ForwardingStage : uint8_t {
    Unknown,
    IncomingNew,
    OverwritePrevious,
};

enum class ForwardingWriterKind : uint8_t {
    Unknown,
    WriteReference,
    AtomicWriteReference,
    AtomicSwapReference,
    CompareAndSwapReference,
    CollectorHeal,
};

enum class ForwardingSourceKind : uint8_t {
    Unknown,
    CallerValue,
    HeapRefField,
};

enum class ForwardingFieldKind : uint8_t {
    Unknown,
    RefField,
    AtomicRefField,
    RootSlot,
};

struct ForwardingProvenance {
    ForwardingHolderKind kind{ ForwardingHolderKind::Unknown };
    const void* holder{ nullptr };
    const void* slot{ nullptr };
    ForwardingStage stage{ ForwardingStage::Unknown };
    ForwardingWriterKind writerKind{ ForwardingWriterKind::Unknown };
    ForwardingSourceKind incomingSourceKind{ ForwardingSourceKind::Unknown };
    const void* sourceSlot{ nullptr };
    const void* workingCopySlot{ nullptr };
    ForwardingFieldKind fieldKind{ ForwardingFieldKind::Unknown };
    size_t fieldOffset{ static_cast<size_t>(-1) };

    static const char* KindName(ForwardingHolderKind kind)
    {
        switch (kind) {
            case ForwardingHolderKind::HeapRef:
                return "heap_ref";
            case ForwardingHolderKind::StackSlot:
                return "stack_slot";
            case ForwardingHolderKind::Remset:
                return "remset";
            case ForwardingHolderKind::Static:
                return "static";
            case ForwardingHolderKind::Derived:
                return "derived";
            case ForwardingHolderKind::StoreBuffer:
                return "store_buffer";
            case ForwardingHolderKind::Unknown:
                return "unknown";
        }
        return "unknown";
    }

    static const char* StageName(ForwardingStage stage)
    {
        switch (stage) {
            case ForwardingStage::IncomingNew:
                return "incoming_new";
            case ForwardingStage::OverwritePrevious:
                return "overwrite_previous";
            case ForwardingStage::Unknown:
                return "unknown";
        }
        return "unknown";
    }

    static const char* WriterName(ForwardingWriterKind writer)
    {
        switch (writer) {
            case ForwardingWriterKind::WriteReference:
                return "write_reference";
            case ForwardingWriterKind::AtomicWriteReference:
                return "atomic_write_reference";
            case ForwardingWriterKind::AtomicSwapReference:
                return "atomic_swap_reference";
            case ForwardingWriterKind::CompareAndSwapReference:
                return "compare_and_swap_reference";
            case ForwardingWriterKind::CollectorHeal:
                return "collector_heal";
            case ForwardingWriterKind::Unknown:
                return "unknown";
        }
        return "unknown";
    }

    static const char* SourceName(ForwardingSourceKind source)
    {
        switch (source) {
            case ForwardingSourceKind::CallerValue:
                return "caller_value";
            case ForwardingSourceKind::HeapRefField:
                return "heap_ref_field";
            case ForwardingSourceKind::Unknown:
                return "unknown";
        }
        return "unknown";
    }

    static const char* FieldName(ForwardingFieldKind field)
    {
        switch (field) {
            case ForwardingFieldKind::RefField:
                return "ref_field";
            case ForwardingFieldKind::AtomicRefField:
                return "atomic_ref_field";
            case ForwardingFieldKind::RootSlot:
                return "root_slot";
            case ForwardingFieldKind::Unknown:
                return "unknown";
        }
        return "unknown";
    }
};

// Public answer to a forwarding lookup. The three miss states deliberately do
// not convert to BaseObject*: a lifecycle failure must remain visible until the
// consumer either handles it explicitly or takes the controlled fail-closed
// path (zForwarding.cpp:183-186; zRelocate.cpp:412-415).
class FindToVersionResult {
public:
    enum class State : uint8_t { Found, NotManaged, NotForwarded, Unavailable };
    enum class UnavailableRoute : uint8_t {
        Unknown,
        LookupUnavailable,
        NoGhostForwarded,
        PublicationRetainFailed,
        GeometricMissForwarded,
        LegacyGeometricMiss,
    };

    struct UnavailableWitness {
        bool forwardedValid{ false };
        bool forwarded{ false };
        bool fromZPageNullValid{ false };
        bool fromZPageNull{ false };
        const char* lookupAnswer{ "not_queried" };
        bool lookupSnapshotValid{ false };
        const char* lookupCause{ "n/a" };
        bool lookupActiveCandidate{ false };
        const char* lookupActiveAnswer{ "n/a" };
        uintptr_t from{ 0 };
        uintptr_t fromRegion{ 0 };
        bool regionSnapshotValid{ false };
        uint8_t regionType{ 0 };
        uint8_t generation{ 0 };
        bool inCurrentRelocationSet{ false };
        uintptr_t tableId{ 0 };
        uint64_t fromPageEpoch{ 0 };
        uint64_t fromPageLifeId{ 0 };
        bool forwardingSnapshotValid{ false };
        uint64_t neverInstalledEvent{ 0 };
        uint8_t gcPhase{ 0xff };
    };

    static FindToVersionResult Found(BaseObject* object)
    {
        CHECK(object != nullptr);
        return FindToVersionResult(State::Found, object);
    }
    static FindToVersionResult NotManaged() { return FindToVersionResult(State::NotManaged, nullptr); }
    static FindToVersionResult NotForwarded() { return FindToVersionResult(State::NotForwarded, nullptr); }
    static FindToVersionResult Unavailable()
    {
        return FindToVersionResult(State::Unavailable, nullptr);
    }
    static FindToVersionResult Unavailable(UnavailableRoute route, const UnavailableWitness& witness)
    {
        return FindToVersionResult(route, witness);
    }

    State state() const { return lookupState; }
    BaseObject* found() const { return lookupState == State::Found ? object : nullptr; }
    bool is_unavailable() const { return lookupState == State::Unavailable; }
    UnavailableRoute unavailable_route() const { return unavailableRoute; }
    bool unavailable_forwarded_valid() const { return unavailableForwardedValid; }
    bool unavailable_forwarded() const { return unavailableForwarded; }
    bool unavailable_from_region_info_null_valid() const { return unavailableFromZPageNullValid; }
    bool unavailable_from_region_info_null() const { return unavailableFromZPageNull; }
    const char* unavailable_lookup_answer() const { return unavailableLookupAnswer; }
    bool unavailable_lookup_snapshot_valid() const { return unavailableLookupSnapshotValid; }
    const char* unavailable_lookup_cause() const { return unavailableLookupCause; }
    bool unavailable_lookup_active_candidate() const { return unavailableLookupActiveCandidate; }
    const char* unavailable_lookup_active_answer() const { return unavailableLookupActiveAnswer; }
    uintptr_t unavailable_from() const { return unavailableFrom; }
    uintptr_t unavailable_from_region() const { return unavailableFromRegion; }
    bool unavailable_region_snapshot_valid() const { return unavailableRegionSnapshotValid; }
    uint8_t unavailable_region_type() const { return unavailablePageKind; }
    uint8_t unavailable_generation() const { return unavailableGeneration; }
    bool unavailable_in_current_relocation_set() const { return unavailableInCurrentRelocationSet; }
    uintptr_t unavailable_table_id() const { return unavailableTableId; }
    uint64_t unavailable_from_page_epoch() const { return unavailableFromPageEpoch; }
    uint64_t unavailable_from_page_life_id() const { return unavailableFromPageLifeId; }
    bool unavailable_forwarding_snapshot_valid() const { return unavailableForwardingSnapshotValid; }
    uint64_t unavailable_never_installed_event() const { return unavailableNeverInstalledEvent; }
    uint8_t unavailable_gc_phase() const { return unavailableGcPhase; }

    const char* unavailable_route_name() const
    {
        switch (unavailableRoute) {
            case UnavailableRoute::LookupUnavailable:
                return "lookup_unavailable";
            case UnavailableRoute::NoGhostForwarded:
                return "no_ghost_forwarded";
            case UnavailableRoute::PublicationRetainFailed:
                return "publication_retain_failed";
            case UnavailableRoute::GeometricMissForwarded:
                return "geometric_miss_forwarded";
            case UnavailableRoute::LegacyGeometricMiss:
                return "legacy_geometric_miss";
            case UnavailableRoute::Unknown:
                return "unknown";
        }
        return "unknown";
    }

    BaseObject* GetOrFailClosed(const char* consumer,
                                const ForwardingProvenance& provenance) const
    {
        const char* forwarded = unavailableForwardedValid ? (unavailableForwarded ? "1" : "0") : "n/a";
        const char* fromZPageNull = unavailableFromZPageNullValid
            ? (unavailableFromZPageNull ? "1" : "0") : "n/a";
        const char* lookup = unavailableLookupSnapshotValid ? unavailableLookupAnswer : "n/a";
        const char* lookupCause = unavailableLookupSnapshotValid ? unavailableLookupCause : "n/a";
        const char* activeCandidate = unavailableLookupSnapshotValid
            ? (unavailableLookupActiveCandidate ? "1" : "0") : "n/a";
        const char* activeLookup = unavailableLookupSnapshotValid ? unavailableLookupActiveAnswer : "n/a";
        const char* regionType = unavailableRegionSnapshotValid ? "present" : "n/a";
        CHECK_DETAIL(lookupState != State::Unavailable,
                     "[FINDTO][fail-closed] consumer=%s forwarding carrier unavailable "
                     "holder_kind=%s holder=%p slot=%p stage=%s writer_kind=%s "
                     "incoming_source_kind=%s source_slot=%p working_copy_slot=%p "
                     "field_type=%s field_offset=%zu from=%p from_region=%p "
                     "region_type=%s(%u) generation=%u in_current_relocation_set=%u table_id=%#zx "
                     "from_page_epoch=%llu lifeId=%llu "
                     "lookup_state=%s route=%s forwarded=%s fromZPage_null=%s lookup=%s "
                     "lookup_snapshot_valid=%u cause=%s active_candidate=%s active_lookup=%s "
                      "never_installed_event=%llu gc_phase=%u",
                     consumer == nullptr ? "unknown" : consumer,
                     ForwardingProvenance::KindName(provenance.kind), provenance.holder, provenance.slot,
                     ForwardingProvenance::StageName(provenance.stage),
                     ForwardingProvenance::WriterName(provenance.writerKind),
                     ForwardingProvenance::SourceName(provenance.incomingSourceKind), provenance.sourceSlot,
                     provenance.workingCopySlot, ForwardingProvenance::FieldName(provenance.fieldKind),
                     provenance.fieldOffset,
                     reinterpret_cast<void*>(unavailableFrom), reinterpret_cast<void*>(unavailableFromRegion),
                     regionType, static_cast<unsigned>(unavailablePageKind),
                     static_cast<unsigned>(unavailableGeneration),
                     unavailableInCurrentRelocationSet ? 1u : 0u,
                     static_cast<size_t>(unavailableTableId),
                     static_cast<unsigned long long>(unavailableFromPageEpoch),
                     static_cast<unsigned long long>(unavailableFromPageLifeId),
                     lookup,
                     unavailable_route_name(),
                     forwarded, fromZPageNull, lookup,
                     static_cast<unsigned>(unavailableLookupSnapshotValid), lookupCause,
                      activeCandidate, activeLookup,
                      static_cast<unsigned long long>(unavailableNeverInstalledEvent),
                     static_cast<unsigned>(unavailableGcPhase));
        return found();
    }

private:
    FindToVersionResult(State state, BaseObject* object)
        : lookupState(state), object(object), unavailableRoute(UnavailableRoute::Unknown),
          unavailableForwardedValid(false), unavailableForwarded(false),
          unavailableFromZPageNullValid(false), unavailableFromZPageNull(false),
          unavailableLookupAnswer("not_queried"), unavailableLookupSnapshotValid(false),
          unavailableLookupCause("n/a"), unavailableLookupActiveCandidate(false),
          unavailableLookupActiveAnswer("n/a"),
          unavailableFrom(0), unavailableFromRegion(0),
          unavailableRegionSnapshotValid(false), unavailablePageKind(0), unavailableGeneration(0),
          unavailableInCurrentRelocationSet(false), unavailableTableId(0),
          unavailableFromPageEpoch(0), unavailableFromPageLifeId(0),
          unavailableForwardingSnapshotValid(false), unavailableNeverInstalledEvent(0),
          unavailableGcPhase(0xff)
    {
    }

    FindToVersionResult(UnavailableRoute route, const UnavailableWitness& witness)
        : lookupState(State::Unavailable), object(nullptr), unavailableRoute(route),
          unavailableForwardedValid(witness.forwardedValid), unavailableForwarded(witness.forwarded),
          unavailableFromZPageNullValid(witness.fromZPageNullValid),
          unavailableFromZPageNull(witness.fromZPageNull),
          unavailableLookupAnswer(witness.lookupAnswer == nullptr ? "unknown" : witness.lookupAnswer),
          unavailableLookupSnapshotValid(witness.lookupSnapshotValid),
          unavailableLookupCause(witness.lookupCause == nullptr ? "unknown" : witness.lookupCause),
          unavailableLookupActiveCandidate(witness.lookupActiveCandidate),
          unavailableLookupActiveAnswer(witness.lookupActiveAnswer == nullptr ? "unknown"
                                                                              : witness.lookupActiveAnswer),
          unavailableFrom(witness.from),
          unavailableFromRegion(witness.fromRegion),
          unavailableRegionSnapshotValid(witness.regionSnapshotValid),
          unavailablePageKind(witness.regionType), unavailableGeneration(witness.generation),
          unavailableInCurrentRelocationSet(witness.inCurrentRelocationSet),
          unavailableTableId(witness.tableId),
          unavailableFromPageEpoch(witness.fromPageEpoch),
          unavailableFromPageLifeId(witness.fromPageLifeId),
          unavailableForwardingSnapshotValid(witness.forwardingSnapshotValid),
          unavailableNeverInstalledEvent(witness.neverInstalledEvent),
          unavailableGcPhase(witness.gcPhase)
    {
    }

    State lookupState;
    BaseObject* object;
    UnavailableRoute unavailableRoute;
    bool unavailableForwardedValid;
    bool unavailableForwarded;
    bool unavailableFromZPageNullValid;
    bool unavailableFromZPageNull;
    const char* unavailableLookupAnswer;
    bool unavailableLookupSnapshotValid;
    const char* unavailableLookupCause;
    bool unavailableLookupActiveCandidate;
    const char* unavailableLookupActiveAnswer;
    uintptr_t unavailableFrom;
    uintptr_t unavailableFromRegion;
    bool unavailableRegionSnapshotValid;
    uint8_t unavailablePageKind;
    uint8_t unavailableGeneration;
    bool unavailableInCurrentRelocationSet;
    uintptr_t unavailableTableId;
    uint64_t unavailableFromPageEpoch;
    uint64_t unavailableFromPageLifeId;
    bool unavailableForwardingSnapshotValid;
    uint64_t unavailableNeverInstalledEvent;
    uint8_t unavailableGcPhase;
};

// Central garbage identification algorithm.


using RegionLifeId = uint64_t;

class ZLiveMap;
class ZRelocateQueue;

// zForwarding.hpp:44-110 — one off-heap object per relocated page.
// _entries is a ZAttachedArray sitting after this object (zAttachedArray.inline.hpp:44-54).
// Source-page retention: _ref_count / _ref_lock / _done (zForwarding.cpp:34-194).
// Forwarding storage belongs to the generation relocation set.
class ZForwarding {
public:
    using AttachedArray = ZAttachedArray<ZForwarding, std::atomic<uint64_t>>;

    enum class Retire : uint32_t {
        DISPEL_GHOST = 0,
        TAKE_GARBAGE = 1,
        RECLAIM_DIRTY = 2,
        RECLAIM_MARK_QUARANTINE = 3,
        RELEASE_REGION = 4,
    };

    class PageWorkScope {
    public:
        explicit PageWorkScope(ZForwarding* forwarding, bool complete = false);
        ~PageWorkScope();
        PageWorkScope(const PageWorkScope&) = delete;
        PageWorkScope& operator=(const PageWorkScope&) = delete;
    private:
        ZForwarding* previous;
        ZForwarding* forwarding;
        bool complete;
    };
    static ZForwarding* CurrentPageWork();
    static void WaitPageDone(ZForwarding* forwarding);
    static constexpr uint32_t kAlignShift = 3;

    struct Receipt {
        enum class Status : uint8_t {
            INSTALLED,
            EXISTING,
        };

        MAddress address;
        bool installed;
        Status status;
    };

    // zForwarding.cpp:55-84 — the old top and livemap belong to the
    // forwarding/from-page incarnation, not to the reusable page metadata.
    // The carrier is installed before relocation and retired as a unit after
    // the last from-page reader drains.
    struct FromPageView {
        ZLiveMap* livemap = nullptr;
        uint64_t epoch = 0;
        MAddress topAtStart = 0;
        uint64_t birthSequence = 0;
        uint8_t owner = 1;
        uint8_t largeMarked = 0;
        RegionLifeId lifeId = 0;
    };

    static size_t nentries(size_t objectCountUpperBound);
    static uint32_t nentries(const ZPage* page);
    static ZForwarding* alloc(ZForwardingAllocator* allocator, ZPage* page, PageAge to_age);

    static ZForwarding* alloc(size_t liveObjects, MAddress start, MAddress heapBase, size_t regionSize,
                              ZPage* page, RegionLifeId pageLifeId = 0,
                              ForwardingAllocator* arena = nullptr);

    // Standalone storage for focused forwarding tests. Product objects use the set arena.
    static ZForwarding* Create(size_t liveObjects, MAddress start, MAddress heapBase, size_t regionSize = 0)
    {
        return alloc(liveObjects, start, heapBase, regionSize, nullptr);
    }

    void Destroy()
    {
        this->~ZForwarding();
        AttachedArray::free(this);
    }

    MAddress start() const;
    size_t size() const;
    size_t regionSize() const { return _size; }
    size_t object_alignment_shift() const { return _object_alignment_shift; }
    PageAge from_age() const { return _from_age; }
    PageAge to_age() const { return _to_age; }
    bool is_promotion() const { return _from_age != PageAge::old && _to_age == PageAge::old; }
    ZPage* page() const;
    RegionLifeId page_life_id() const { return _page_life_id; }
    bool page_life_current() const;
    void verify() const;
    size_t length() const { return _entries.length(); }

    void publish_from_page_view(ZLiveMap* livemap, uint64_t epoch, MAddress topAtStart,
                                uint64_t birthSequence,
                                uint8_t owner, uint8_t largeMarked, RegionLifeId lifeId)
    {
        // lifeId is the publication word. Readers either reject the zero word
        // or acquire the complete immutable replacement.
        __atomic_store_n(&_from_page.lifeId, static_cast<RegionLifeId>(0), __ATOMIC_RELEASE);
        _from_page.livemap = livemap;
        _from_page.epoch = epoch;
        _from_page.topAtStart = topAtStart;
        _from_page.birthSequence = birthSequence;
        _from_page.owner = owner;
        _from_page.largeMarked = largeMarked;
        __atomic_store_n(&_from_page.lifeId, lifeId, __ATOMIC_RELEASE);
    }

    const FromPageView* from_page_view(RegionLifeId currentLife) const
    {
        const RegionLifeId life = __atomic_load_n(&_from_page.lifeId, __ATOMIC_ACQUIRE);
        return life != 0 && life == currentLife ? &_from_page : nullptr;
    }

    const FromPageView* from_page_snapshot() const
    {
        return __atomic_load_n(&_from_page.lifeId, __ATOMIC_ACQUIRE) == 0 ? nullptr : &_from_page;
    }

    // zPage.inline.hpp:176-185 seqnum bounds livemap/forwarding to one page life.
    // Record the to-region start+regionLifeSeq at insert; consume rejects when
    // InitZPage has bumped that seq (ZPage.h:InitZPage).
    static bool DestUsable(MAddress to);


    bool covers(MAddress addr) const { return _size != 0 && addr >= _start && addr < _start + _size; }

    uintptr_t index(MAddress from) const;

    std::atomic<uint64_t>* entries() const;

    ForwardingEntry at(ForwardingCursor* cursor) const;

    ForwardingEntry first(uintptr_t fromIndex, ForwardingCursor* cursor) const;

    ForwardingEntry next(ForwardingCursor* cursor) const;

    // zForwarding.inline.hpp:230-245 plus a bound: our nentries estimate can undersize
    // (REPORT-fwdentries). A miss is a state every caller already handles.
    ForwardingEntry find(uintptr_t fromIndex, ForwardingCursor* cursor) const;

    // In-place relocation reuses one page for both layouts.  ClassifyCompactedMiss
    // must therefore be able to establish that an address below the new top is a
    // published destination before consulting the old liveness face
    // (ZGC zForwarding.cpp:55-64; zHeap.cpp:202-208).
    bool find_from_by_to(MAddress to, MAddress* fromOut) const
    {
        auto* words = entries();
        for (size_t i = 0; i < _entries.length(); ++i) {
            const ForwardingEntry entry = ForwardingEntry::FromRaw(words[i].load(std::memory_order_acquire));
            if (!entry.populated()) {
                continue;
            }
            if (_heapBase + static_cast<MAddress>(entry.to_offset()) == to) {
                if (fromOut != nullptr) {
                    *fromOut = _start + (static_cast<MAddress>(entry.from_index()) << kAlignShift);
                }
                return true;
            }
        }
        return false;
    }

    // zForwarding.inline.hpp:248-252 — miss is null, never geometry.
    MAddress find(MAddress from) const;

    template<typename Fn>
    void for_each_from(Fn&& fn) const
    {
        auto* words = entries();
        for (size_t i = 0; i < _entries.length(); ++i) {
            const ForwardingEntry entry = ForwardingEntry::FromRaw(words[i].load(std::memory_order_acquire));
            if (entry.populated()) {
                fn(_start + (static_cast<MAddress>(entry.from_index()) << kAlignShift));
            }
        }

    }

    size_t insert(uintptr_t fromIndex, size_t toOffset, ForwardingCursor* cursor, bool* installed = nullptr);

    Receipt insert_receipt(MAddress from, MAddress to, const std::function<void()>& beforeFirstCas = {})
    {
        // zForwarding.inline.hpp:267-300: one attached array and one CAS winner.
        ForwardingCursor cursor = 0;
        const uintptr_t fromIndex = index(from);
        if (fromIndex > ForwardingEntry::kMaxFromIndex) {
            return Receipt{ 0, false, Receipt::Status::EXISTING };
        }
        const size_t toOffset = static_cast<size_t>(to - _heapBase);
        const ForwardingEntry existing = find(fromIndex, &cursor);
        if (existing.populated()) {
            return Receipt{ _heapBase + static_cast<MAddress>(existing.to_offset()), false,
                            Receipt::Status::EXISTING };
        }
        if (beforeFirstCas) beforeFirstCas();
        bool installed = false;
        const size_t finalOff = insert(fromIndex, toOffset, &cursor, &installed);
        return Receipt{ _heapBase + static_cast<MAddress>(finalOff), installed,
                        installed ? Receipt::Status::INSTALLED : Receipt::Status::EXISTING };
    }

    MAddress insert(MAddress from, MAddress to);

    enum class ZPublishState : int8_t {
        none,
        published,
        reject,
        accept,
    };

    static uint32_t young_seqnum();

    static bool young_marking()
    {
        return ZGeneration::young() != nullptr && ZGeneration::young()->is_phase_mark();
    }

    void relocated_remembered_fields_register(MAddress field);

    bool relocated_remembered_fields_is_concurrently_scanned() const;

    // zForwarding.cpp:relocated_remembered_fields_published_contains.
    // Verification is observational; unlike apply_to_published it never clears.
    bool relocated_remembered_fields_published_contains(MAddress field);

    void relocated_remembered_fields_after_relocate();

    void relocated_remembered_fields_publish();

    void relocated_remembered_fields_notify_concurrent_scan_of();

    template<typename Function>
    void relocated_remembered_fields_apply_to_published(Function function);

    // zForwarding.cpp:51-53 / :86-194. Source-page ownership only.
    bool claim();
    bool is_claimed() const;
    bool in_place() const;
    void set_in_place();
    bool retain_page(ZRelocateQueue* queue);
    void release_page();
    ZPage* detach_page();
    void mark_done();
    bool is_done() const;
    void in_place_relocation_claim_page();
    void in_place_relocation_start(MAddress relocated_watermark);
    void in_place_relocation_finish();
    bool in_place_relocation_is_below_top_at_start(MAddress offset) const;

    std::atomic<int32_t>& ref_count() { return _ref_count; }
    std::atomic<bool>& claimed() { return _claimed; }
    std::atomic<bool>& done() { return _done; }
    std::mutex& ref_lock() const { return _ref_lock; }

private:
    // zForwarding.inline.hpp:59-76
    ZForwarding(ZPage* page, MAddress start, MAddress heapBase, size_t regionSize, size_t nentries,
                RegionLifeId pageLifeId, PageAge from_age, PageAge to_age, size_t object_alignment_shift);

    const MAddress _start;
    const size_t _size;
    const MAddress _heapBase;
    const size_t _object_alignment_shift;
    const AttachedArray _entries;
    ZPage* const _page;
    const PageAge _from_age;
    const PageAge _to_age;
    const RegionLifeId _page_life_id;
    // Monotonic per-region-span generation. Written before the table pointer is
    // published, then immutable for the table's lifetime.
    std::atomic<bool> _claimed;
    std::atomic<bool> _in_place;
    MAddress _in_place_top_at_start;
    std::atomic<std::thread::id> _in_place_thread;
    mutable std::mutex _ref_lock;
    std::condition_variable _ref_changed;
    std::atomic<int32_t> _ref_count;
    std::atomic<bool> _done;
    FromPageView _from_page;
    std::atomic<ZPublishState> _relocated_remembered_fields_state;
    std::vector<MAddress> _relocated_remembered_fields_array;
    uint32_t _relocated_remembered_fields_publish_young_seqnum;
    mutable std::mutex _relocated_fields_lock;
};

// Attached-entry spelling used by existing consumers.
using ForwardingEntries = ZForwarding;

} // namespace MapleRuntime

#include "Heap/z/zForwarding.inline.hpp"

#endif // MRT_Z_FORWARDING_H
