// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#pragma once

#include "Heap/z/zAddress.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <set>
#include <vector>

#include "Base/Macros.h"
#include "Heap/Collector/GcRequest.h"
#include "Heap/Collector/GcStats.h"

#include "Heap/z/zGeneration.hpp"
namespace MapleRuntime {
// loadfc (zBarrier.inline.hpp:327-343): best-effort detection verdict for slow/runtime hand-outs.
// The single relaxed header read classifies the observed word but establishes no lifetime
// guarantee. ZGC structurally cannot hand a from-address back after a slow-path miss
// (zGeneration.inline.hpp:131-140 has no "lookup miss ⇒ return from" exit); detected shapes are:
//   Forwarded   header stateCode=3, a to-version exists and must be found
//   ZeroHeader  payload cleared by reclamation (ClearUnits reuse) -- nothing to resolve
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
        uint8_t gcPhase{ GC_PHASE_UNDEF };
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
          unavailableGcPhase(GC_PHASE_UNDEF)
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
}
