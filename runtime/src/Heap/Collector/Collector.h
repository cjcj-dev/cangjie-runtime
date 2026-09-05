// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_H
#define MRT_COLLECTOR_H

#include "Common/ColourMask.h"
#include "Common/ColourPredicates.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <set>
#include <vector>

#include "Base/Macros.h"
#include "GcRequest.h"
#include "GcStats.h"

namespace MapleRuntime {
// GCPhase describes phases for stw/concurrent gc.
enum GCPhase : uint8_t {
    GC_PHASE_UNDEF = 0,
    GC_PHASE_IDLE = 1,
    GC_PHASE_FINISH = 2,
    GC_PHASE_RECLAIM_SATB_NODE = 3,
    GC_PHASE_INIT = 8,

    // only gc phase after GC_PHASE_INIT ( enum value > GC_PHASE_INIT) needs barrier.
    GC_PHASE_ENUM = 9,
    GC_PHASE_TRACE = 10,
    GC_PHASE_CLEAR_SATB_BUFFER = 11,
    GC_PHASE_POST_TRACE = 12,
    GC_PHASE_PREFORWARD = 13,
    GC_PHASE_FORWARD = 14,
};

enum CollectorType {
    NO_COLLECTOR = 0, // No Collector
    PROXY_COLLECTOR,  // Proxy of Collector
    COPY_COLLECTOR,   // Regional-Copying GC
    SMOOTH_COLLECTOR, // wgc
    COLLECTOR_TYPE_COUNT,
};

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
        bool fromRegionInfoNullValid{ false };
        bool fromRegionInfoNull{ false };
        const char* lookupAnswer{ "not_queried" };
        bool lookupSnapshotValid{ false };
        const char* lookupCause{ "n/a" };
        bool lookupActiveCandidate{ false };
        const char* lookupActiveAnswer{ "n/a" };
        const char* lookupRetiredAnswer{ "n/a" };
        bool lookupPublicationClosed{ false };
        bool routeStateValid{ false };
        uint8_t routeState{ 0 };
        uintptr_t from{ 0 };
        uintptr_t fromRegion{ 0 };
        bool regionSnapshotValid{ false };
        uint8_t regionType{ 0 };
        uint8_t generation{ 0 };
        bool inCurrentRelocationSet{ false };
        uintptr_t tableId{ 0 };
        uint64_t publicationGeneration{ 0 };
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
    bool unavailable_from_region_info_null_valid() const { return unavailableFromRegionInfoNullValid; }
    bool unavailable_from_region_info_null() const { return unavailableFromRegionInfoNull; }
    const char* unavailable_lookup_answer() const { return unavailableLookupAnswer; }
    bool unavailable_lookup_snapshot_valid() const { return unavailableLookupSnapshotValid; }
    const char* unavailable_lookup_cause() const { return unavailableLookupCause; }
    bool unavailable_lookup_active_candidate() const { return unavailableLookupActiveCandidate; }
    const char* unavailable_lookup_active_answer() const { return unavailableLookupActiveAnswer; }
    const char* unavailable_lookup_retired_answer() const { return unavailableLookupRetiredAnswer; }
    bool unavailable_lookup_publication_closed() const { return unavailableLookupPublicationClosed; }
    bool unavailable_route_state_valid() const { return unavailableRouteStateValid; }
    uint8_t unavailable_route_state() const { return unavailableRouteState; }
    uintptr_t unavailable_from() const { return unavailableFrom; }
    uintptr_t unavailable_from_region() const { return unavailableFromRegion; }
    bool unavailable_region_snapshot_valid() const { return unavailableRegionSnapshotValid; }
    uint8_t unavailable_region_type() const { return unavailableRegionType; }
    uint8_t unavailable_generation() const { return unavailableGeneration; }
    bool unavailable_in_current_relocation_set() const { return unavailableInCurrentRelocationSet; }
    uintptr_t unavailable_table_id() const { return unavailableTableId; }
    uint64_t unavailable_publication_generation() const { return unavailablePublicationGeneration; }
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
        const char* fromRegionInfoNull = unavailableFromRegionInfoNullValid
            ? (unavailableFromRegionInfoNull ? "1" : "0") : "n/a";
        const char* lookup = unavailableLookupSnapshotValid ? unavailableLookupAnswer : "n/a";
        const char* lookupCause = unavailableLookupSnapshotValid ? unavailableLookupCause : "n/a";
        const char* activeCandidate = unavailableLookupSnapshotValid
            ? (unavailableLookupActiveCandidate ? "1" : "0") : "n/a";
        const char* activeLookup = unavailableLookupSnapshotValid ? unavailableLookupActiveAnswer : "n/a";
        const char* retiredLookup = unavailableLookupSnapshotValid ? unavailableLookupRetiredAnswer : "n/a";
        const char* publicationClosed = unavailableLookupSnapshotValid
            ? (unavailableLookupPublicationClosed ? "1" : "0") : "n/a";
        const char* routeState = unavailableRouteStateValid
            ? (unavailableRouteState == 0 ? "0" :
               unavailableRouteState == 1 ? "1" :
               unavailableRouteState == 2 ? "2" :
               unavailableRouteState == 3 ? "3" :
               unavailableRouteState == 4 ? "4" :
               unavailableRouteState == 5 ? "5" : "invalid")
            : "n/a";
        const char* regionType = unavailableRegionSnapshotValid ? "present" : "n/a";
        CHECK_DETAIL(lookupState != State::Unavailable,
                     "[FINDTO][fail-closed] consumer=%s forwarding carrier unavailable "
                     "holder_kind=%s holder=%p slot=%p stage=%s writer_kind=%s "
                     "incoming_source_kind=%s source_slot=%p working_copy_slot=%p "
                     "field_type=%s field_offset=%zu from=%p from_region=%p "
                     "region_type=%s(%u) generation=%u in_current_relocation_set=%u table_id=%#zx "
                     "publication_generation=%llu from_page_epoch=%llu lifeId=%llu "
                     "lookup_state=%s route=%s forwarded=%s fromRegionInfo_null=%s lookup=%s "
                     "lookup_snapshot_valid=%u cause=%s active_candidate=%s active_lookup=%s "
                     "retired_lookup=%s publication_closed=%s route_state=%s never_installed_event=%llu gc_phase=%u",
                     consumer == nullptr ? "unknown" : consumer,
                     ForwardingProvenance::KindName(provenance.kind), provenance.holder, provenance.slot,
                     ForwardingProvenance::StageName(provenance.stage),
                     ForwardingProvenance::WriterName(provenance.writerKind),
                     ForwardingProvenance::SourceName(provenance.incomingSourceKind), provenance.sourceSlot,
                     provenance.workingCopySlot, ForwardingProvenance::FieldName(provenance.fieldKind),
                     provenance.fieldOffset,
                     reinterpret_cast<void*>(unavailableFrom), reinterpret_cast<void*>(unavailableFromRegion),
                     regionType, static_cast<unsigned>(unavailableRegionType),
                     static_cast<unsigned>(unavailableGeneration),
                     unavailableInCurrentRelocationSet ? 1u : 0u,
                     static_cast<size_t>(unavailableTableId),
                     static_cast<unsigned long long>(unavailablePublicationGeneration),
                     static_cast<unsigned long long>(unavailableFromPageEpoch),
                     static_cast<unsigned long long>(unavailableFromPageLifeId),
                     lookup,
                     unavailable_route_name(),
                     forwarded, fromRegionInfoNull, lookup,
                     static_cast<unsigned>(unavailableLookupSnapshotValid), lookupCause,
                     activeCandidate, activeLookup, retiredLookup, publicationClosed, routeState,
                     static_cast<unsigned long long>(unavailableNeverInstalledEvent),
                     static_cast<unsigned>(unavailableGcPhase));
        return found();
    }

private:
    FindToVersionResult(State state, BaseObject* object)
        : lookupState(state), object(object), unavailableRoute(UnavailableRoute::Unknown),
          unavailableForwardedValid(false), unavailableForwarded(false),
          unavailableFromRegionInfoNullValid(false), unavailableFromRegionInfoNull(false),
          unavailableLookupAnswer("not_queried"), unavailableLookupSnapshotValid(false),
          unavailableLookupCause("n/a"), unavailableLookupActiveCandidate(false),
          unavailableLookupActiveAnswer("n/a"), unavailableLookupRetiredAnswer("n/a"),
          unavailableLookupPublicationClosed(false), unavailableRouteStateValid(false),
          unavailableRouteState(0), unavailableFrom(0), unavailableFromRegion(0),
          unavailableRegionSnapshotValid(false), unavailableRegionType(0), unavailableGeneration(0),
          unavailableInCurrentRelocationSet(false), unavailableTableId(0),
          unavailablePublicationGeneration(0), unavailableFromPageEpoch(0), unavailableFromPageLifeId(0),
          unavailableForwardingSnapshotValid(false), unavailableNeverInstalledEvent(0),
          unavailableGcPhase(GC_PHASE_UNDEF)
    {
    }

    FindToVersionResult(UnavailableRoute route, const UnavailableWitness& witness)
        : lookupState(State::Unavailable), object(nullptr), unavailableRoute(route),
          unavailableForwardedValid(witness.forwardedValid), unavailableForwarded(witness.forwarded),
          unavailableFromRegionInfoNullValid(witness.fromRegionInfoNullValid),
          unavailableFromRegionInfoNull(witness.fromRegionInfoNull),
          unavailableLookupAnswer(witness.lookupAnswer == nullptr ? "unknown" : witness.lookupAnswer),
          unavailableLookupSnapshotValid(witness.lookupSnapshotValid),
          unavailableLookupCause(witness.lookupCause == nullptr ? "unknown" : witness.lookupCause),
          unavailableLookupActiveCandidate(witness.lookupActiveCandidate),
          unavailableLookupActiveAnswer(witness.lookupActiveAnswer == nullptr ? "unknown"
                                                                              : witness.lookupActiveAnswer),
          unavailableLookupRetiredAnswer(witness.lookupRetiredAnswer == nullptr ? "unknown"
                                                                                : witness.lookupRetiredAnswer),
          unavailableLookupPublicationClosed(witness.lookupPublicationClosed),
          unavailableRouteStateValid(witness.routeStateValid),
          unavailableRouteState(witness.routeState), unavailableFrom(witness.from),
          unavailableFromRegion(witness.fromRegion),
          unavailableRegionSnapshotValid(witness.regionSnapshotValid),
          unavailableRegionType(witness.regionType), unavailableGeneration(witness.generation),
          unavailableInCurrentRelocationSet(witness.inCurrentRelocationSet),
          unavailableTableId(witness.tableId),
          unavailablePublicationGeneration(witness.publicationGeneration),
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
    bool unavailableFromRegionInfoNullValid;
    bool unavailableFromRegionInfoNull;
    const char* unavailableLookupAnswer;
    bool unavailableLookupSnapshotValid;
    const char* unavailableLookupCause;
    bool unavailableLookupActiveCandidate;
    const char* unavailableLookupActiveAnswer;
    const char* unavailableLookupRetiredAnswer;
    bool unavailableLookupPublicationClosed;
    bool unavailableRouteStateValid;
    uint8_t unavailableRouteState;
    uintptr_t unavailableFrom;
    uintptr_t unavailableFromRegion;
    bool unavailableRegionSnapshotValid;
    uint8_t unavailableRegionType;
    uint8_t unavailableGeneration;
    bool unavailableInCurrentRelocationSet;
    uintptr_t unavailableTableId;
    uint64_t unavailablePublicationGeneration;
    uint64_t unavailableFromPageEpoch;
    uint64_t unavailableFromPageLifeId;
    bool unavailableForwardingSnapshotValid;
    uint64_t unavailableNeverInstalledEvent;
    uint8_t unavailableGcPhase;
};

// c4unify MASKEQUIV: dual-run the published bad masks against a verbatim copy of the literal
// expressions WCollector::set_good_masks carried before the formula was lifted into
// ColourMask.h::ComputeBadMasks, and count divergences. Same three-piece shape as GATEEQUIV
// (Collector.cpp:109-146), deliberately: MRT_GCV2_MASKEQUIV=1 arms it, and
// MRT_GCV2_MASKEQUIV_INJECT=1 forces exactly one synthetic divergence so that "the probe never
// ran" is distinguishable from "there was no divergence". Default off; the entry point returns
// immediately when unarmed, so the product path publishes the same words it always did
// (storegood2 adds StoreGood next to StoreBad; the check covers that pair too).
//
// What this covers that the compile-time table cannot: which value each flip actually publishes,
// and in what order. What neither covers: a flip that forgets to call set_good_masks at all.
bool MaskEquivOn();
bool MaskEquivInjectOn();
void MaskEquivCheck(const EpochColours& e, const BadMasks& m);
void MaskEquivAtexitReport();

// Central garbage identification algorithm.
class Collector {
public:
    Collector();
    virtual ~Collector() = default;

    static const char* GetGCPhaseName(GCPhase phase);

    // Initializer and finalizer.
    virtual void Init() = 0;
    virtual void Fini() {}
    const char* GetCollectorName() const;

    // This pure virtual function implements the trigger of GC.
    // reason: Reason for GC.
    // async:  Trigger from unsafe context, e.g., holding a lock, in the middle of an allocation.
    //         In order to prevent deadlocks, async trigger only add one async gc task and will not block.
    void RequestGC(GCReason reason, bool async);

    virtual GCPhase GetGCPhase() const { return gcPhase.load(std::memory_order_acquire); }

    virtual void SetGCPhase(const GCPhase phase) { gcPhase.store(phase, std::memory_order_release); }

    // determine how we treat new object during gc.
    virtual void MarkNewObject(BaseObject*) {}

    virtual void FixObject(BaseObject&) const {}

    virtual void RunGarbageCollection(uint64_t, GCReason) = 0;

    // Named aborts: virtual defaults for collectors that do not implement this method.
    // Bodies live in Collector.cpp so headers stay free of FormatLog / string literals.
    [[noreturn]] static void AbortUnimplemented(const char* method);

    // loadfc: shared hand-out verdict (header word only, one relaxed load). Out-of-line because
    // Heap::IsHeapAddress lives behind Barrier.h which must not be included from here.
    static HandVerdict JudgeHandOutTarget(BaseObject* target);
    // Emit the gated NeverInstalled identity record from one relaxed raw-header
    // read and one ownership-protected carrier snapshot. Returns its event id.
    static uint64_t EmitNeverInstalledDiagnostic(BaseObject* target, uintptr_t rawSlotBits,
                                                 MAddress witnessStart, uint64_t witnessEpoch,
                                                 uint64_t witnessLife, bool witnessValid);
    // loadfc: the loud failure for "resolution failed and the from-address is not Usable"
    // (0825 用户令: no silent fold-back to the original address).
    [[noreturn]] static void FailClosedLoad(const char* site, BaseObject* target, uintptr_t slotBits,
                                            const ForwardingProvenance& provenance);

    virtual GCStats& GetGCStats() { AbortUnimplemented("Collector::GetGCStats"); }

    virtual BaseObject* ForwardObject(BaseObject*) { AbortUnimplemented("Collector::ForwardObject"); }

    virtual bool ShouldIgnoreRequest(GCRequest& quest) = 0;
    virtual bool IsFromObject(BaseObject*) const { AbortUnimplemented("Collector::IsFromObject"); }
    virtual bool IsGhostFromObject(BaseObject*) const { AbortUnimplemented("Collector::IsGhostFromObject"); }
    virtual bool IsUnmovableFromObject(BaseObject*) const
    {
        AbortUnimplemented("Collector::IsUnmovableFromObject");
    }
    // Every miss has a public state. Consumers may treat NotManaged and
    // NotForwarded as their existing soft misses; Unavailable must never fall
    // through to object-field access.
    virtual FindToVersionResult FindToVersion(BaseObject* obj) const = 0;

    // OpenJDK zBarrier.inline.hpp:695-716 store_barrier / color_store_good:
    // a stored reference must already be the current version (remap included).
    // Default identity so gc_unit Collector stubs do not abort.
    virtual BaseObject* ResolveStoreValue(BaseObject* ref,
                                          const ForwardingProvenance& provenance) const
    {
        (void)provenance;
        return ref;
    }

    virtual bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const
    {
        AbortUnimplemented("Collector::TryUpdateRefField");
    }
    virtual bool TryUpdateRefFieldWithProvenance(BaseObject* obj, RefField<>& field, BaseObject*& to,
                                                 const ForwardingProvenance&) const
    {
        return TryUpdateRefField(obj, field, to);
    }
    virtual bool TryForwardRefField(BaseObject*, RefField<>&, BaseObject*&) const
    {
        AbortUnimplemented("Collector::TryForwardRefField");
    }
    virtual bool TryUntagRefField(BaseObject*, RefField<>&, BaseObject*&) const
    {
        AbortUnimplemented("Collector::TryUntagRefField");
    }
    virtual bool TryTagRefField(BaseObject*, RefField<>&, BaseObject*) const
    {
        AbortUnimplemented("Collector::TryTagRefField");
    }
    virtual Uptr CurrentRemapColourForProbe() const { return 0; }

    virtual bool IsMarkedObjectForProbe(BaseObject*) const { return false; }

    // healfp: lossy fingerprint of "this slot address was healed by the mark walk this process".
    // Volume alone cannot answer whether a *particular* stale slot was ever visited -- TraceRefField
    // heals >=524k slots per run, so a big number proves breadth, not coverage of the one slot that
    // went stale.  A bit that is *clear* is strong evidence the slot was never healed; a bit that is
    // set is weak (hash collisions), which is why the same query is also run on ordinary hand-outs
    // to get the collision baseline.
    static constexpr size_t kHealFpBits = 1u << 22; // 4 Mbit = 512 KiB
    static uint64_t* HealFpTable()
    {
        static uint64_t table[kHealFpBits / 64] = {};
        return table;
    }
    static size_t HealFpIndex(uintptr_t slot)
    {
        // slots are 8-byte aligned; mix the high bits down so regions do not alias wholesale
        const uintptr_t h = (slot >> 3) ^ (slot >> 23) ^ (slot >> 41);
        return static_cast<size_t>(h) & (kHealFpBits - 1);
    }
    static void HealFpMark(uintptr_t slot)
    {
        const size_t i = HealFpIndex(slot);
        __atomic_fetch_or(&HealFpTable()[i / 64], uint64_t(1) << (i % 64), __ATOMIC_RELAXED);
    }
    static bool HealFpTest(uintptr_t slot)
    {
        const size_t i = HealFpIndex(slot);
        return (__atomic_load_n(&HealFpTable()[i / 64], __ATOMIC_RELAXED) & (uint64_t(1) << (i % 64))) != 0;
    }

    virtual RefField<> GetAndTryTagRefField(BaseObject*) const
    {
        AbortUnimplemented("Collector::GetAndTryTagRefField");
    }
    virtual RefField<> GetAndTryTagRefFieldWithProvenance(BaseObject* obj,
                                                          const ForwardingProvenance&) const
    {
        return GetAndTryTagRefField(obj);
    }

    // "Does this reference need the barrier before use?" -- the question every consumer of the
    // two predicates below is actually asking. Today a reference carries no colour unless it is
    // being evacuated, so the answer was a pointer tag bit; phase C of the colouring work
    // (ops/design/G1_WRITE_BARRIER_DESIGN.md §3.6) makes it a mask test. Non-virtual and phase
    // independent: the encoding is a property of RefField, not of the collector's phase.
    // Phase C: the value now says whether it may be stale. A reference is good when it carries
    // the colour the collector is currently handing out and is not mid-evacuation; anything else
    // -- an older colour, or a tagged reference -- has to go through the barrier. One AND, matching
    // what the compiler emits (CJBarrierLowering.cpp:653) and what ZGC does
    // (jdk zBarrier.inline.hpp:626-628).
    //
    // A zero field passes, as it does in ZGC: null carries no colour, and every stored reference
    // is coloured on the way in, so the only uncoloured values are the ones that were never
    // written (jdk zAddress.inline.hpp:635-643 makes the same trade deliberately).
    bool IsLoadBad(RefField<>& ref) const
    {
        // 凭什么 raw: 掩码测的是槽位位型，不是解引用。
        return (raw(ref.GetFieldValue()) & ::g_cjLoadBadMask) != 0;
    }

    virtual bool is_young_load_good(RefField<>&) const { AbortUnimplemented("Collector::is_young_load_good"); }
    virtual bool is_old_load_good(RefField<>&) const { AbortUnimplemented("Collector::is_old_load_good"); }

    // ZPointer::is_load_good (zAddress.inline.hpp:631-633). Keep the product
    // predicate in the collector domain; diagnostic mode selection must not own it.
    bool is_load_good(RefField<>& ref) const
    {
        return ColourPredicates::is_load_good(static_cast<uintptr_t>(raw(ref.GetFieldValue())),
                                              static_cast<uintptr_t>(::g_cjLoadBadMask));
    }

    virtual ZGenerationId remap_generation(RefField<>&) const
    {
        AbortUnimplemented("Collector::remap_generation");
    }
    virtual BaseObject* relocate_or_remap_object(BaseObject*, ZGenerationId) const
    {
        AbortUnimplemented("Collector::relocate_or_remap_object");
    }
    virtual BaseObject* relocate_or_remap_object(
        BaseObject* object, ZGenerationId generation, const ForwardingProvenance&) const
    {
        return relocate_or_remap_object(object, generation);
    }

    // make_load_good: 带色槽 → 可解引用对象。内部仍返 BaseObject* 以兼容现有调用面；
    // 新代码应经 to_object(zaddress) 出口。
    // tipnull barriernull: live non-null ref must never become nullptr for mutator
    // (cjpm+0x31061a test [rax+0xc] after CJ_MCC_ReadRefField with rax=0).
    //
    // loadfc scope note (0826 measured, kkk2 gate nwdet e_256MB.txt): make_load_good is NOT
    // exclusively a mutator hand-out funnel -- the GC mark walk calls it too (Mark.cpp) and
    // already owns a tolerance policy for cleared from-addresses ([MARKSTALE]; dozens per cycle
    // are routine). Failing loudly here killed a green nwdet wave from inside mark, at no
    // hand-out. Slow/runtime exits close with Barrier::FinalizeLoadForMutator; the finalizer
    // consumer now uses the public ReadStaticRef runtime path instead of this helper. Compiler
    // colour-good fast paths retain ZGC's direct-uncolour form and depend on the producer-side
    // colour/lifetime invariant.
    BaseObject* make_load_good(RefField<>& ref, const ForwardingProvenance& provenance) const
    {
        // 凭什么 to_object: GetTargetObject 已剥色；null 或 load-good 可直接用。
        BaseObject* target = to_object(ref.GetTargetObject());
        if (target == nullptr || is_load_good(ref)) {
            return target;
        }

        // ZGC's relocate_or_remap_object has no "return the from oop" exit
        // (zRelocate.cpp:382-416).  A failed lookup is therefore represented
        // as null for internal walkers; callers that hand values to mutators
        // must use the load-barrier slow path, which fails closed rather than
        // laundering the unresolved address into a load-good value.
        return relocate_or_remap_object(target, remap_generation(ref), provenance);
    }

    // OpenJDK ZPointer::is_mark_good (zAddress.inline.hpp:658-664): mark-good includes load-good,
    // the current young mark epoch, and the current old mark epoch; raw null is not mark-good.
    //
    // Encoding completeness is enforced at HeapSlot publication. As in ZGC,
    // this phase predicate is only the single not-bad-mask test.
    bool is_mark_good(RefField<>& ref) const
    {
        return ColourPredicates::is_mark_good(static_cast<uintptr_t>(raw(ref.GetFieldValue())),
                                              static_cast<uintptr_t>(::g_cjLoadBadMask),
                                              static_cast<uintptr_t>(::g_cjMarkBadMask));
    }

    // OpenJDK ZPointer::is_store_good (zAddress.inline.hpp:679-684): store-good includes
    // mark-good plus the current Remembered epoch bit. Fast path for write barrier.
    bool is_store_good(RefField<>& ref) const
    {
        return ColourPredicates::is_store_good(static_cast<uintptr_t>(raw(ref.GetFieldValue())),
                                               static_cast<uintptr_t>(::g_cjLoadBadMask),
                                               static_cast<uintptr_t>(::g_cjStoreBadMask));
    }

    bool is_store_bad(RefField<>& ref) const
    {
        return (raw(ref.GetFieldValue()) & ::g_cjStoreBadMask) != 0;
    }

    // zc7fix: is_mark_good admits plain (uncoloured) non-null; those may be non-heap.
    // Gate before IsValidObject/IsMarkedObject. Count rejects under MRT_GCV2_MARKGOOD_HEAP_GATE=1.
    static bool MarkGoodHeapGate(const char* site, BaseObject* target);
    static void ReportMarkGoodHeapGateCounts();

    // markfloor: reject heap interiors (e.g. RawArray+8 / &length) whose first word is a
    // small integer or non-TypeInfo, before GetSize/HasRefField dereference the tip.
    // sizeguard: also reject addresses in FREE/GARBAGE regions (stale payload may still
    // look like a TypeInfo tip and trip INVALID_OBJECT_SIZE at MarkObject).
    // tailslot: reject when obj+GetSize crosses regionEnd on any live region
    // (zMarkStackEntry.hpp:81 object_address bits 63-5; zPage.inline.hpp:188 is_in).
    // Count rejects under MRT_GCV2_MARKFLOOR_OBJ_GATE=1.
    static bool PlausibleManagedObjectGate(const char* site, BaseObject* obj);
    static void ReportPlausibleManagedObjectGateCounts();
    // introot: if obj is a heap interior (RawArray+8/...), return host object base; else nullptr.
    // writeback2: when knownBase is non-null (derived channel already paired base↔derived),
    // trust it over ClassifyInteriorOffset heuristics.
    static BaseObject* TryRecoverInteriorBase(BaseObject* obj, BaseObject* knownBase = nullptr);

    virtual bool IsOldPointer(RefField<>&) const { AbortUnimplemented("Collector::IsOldPointer"); }
    virtual bool IsCurrentPointer(RefField<>&) const { AbortUnimplemented("Collector::IsCurrentPointer"); }
    virtual void AddRawPointerObject(BaseObject*) { AbortUnimplemented("Collector::AddRawPointerObject"); }
    // Pin for callers that hand the pinned payload out (MCC_AcquireRawData): the pin may
    // resolve a movable from-copy to its to-version first, and the caller MUST adopt the
    // returned pointer — Inc lands on the resolved object's region, so releasing through
    // the original from payload would Dec a region that was never Inc'd (underflow) and
    // hand C a payload the collector is about to relocate.
    virtual BaseObject* PinRawPointerObject(BaseObject* obj)
    {
        AddRawPointerObject(obj);
        return obj;
    }
    virtual void RemoveRawPointerObject(BaseObject*)
    {
        AbortUnimplemented("Collector::RemoveRawPointerObject");
    }
    virtual void ResolveCycleRef() { AbortUnimplemented("Collector::ResolveCycleRef"); }

    // F5: to==nullptr must not silently return a dead/zeroed from (REPORT-tagaba F5).
    // Implementation in Collector.cpp — needs complete BaseObject + CHECK_DETAIL.
    // Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94.
    BaseObject* FindLatestVersion(BaseObject* obj, const ForwardingProvenance& provenance) const;

protected:
    virtual void RequestGCInternal(GCReason, bool) { AbortUnimplemented("Collector::RequestGCInternal"); }

    CollectorType collectorType = CollectorType::NO_COLLECTOR;
    std::atomic<GCPhase> gcPhase = { GCPhase::GC_PHASE_IDLE };
};
} // namespace MapleRuntime

#endif // MRT_COLLECTOR_H
