// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Collector/Collector.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Collector/GcStats.h"
#include "Common/BaseObject.h"
#include "Common/ColourPredicates.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/ManagedObjectGate.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {
const char* const COLLECTOR_NAME[] = { "No Collector", "Proxy Collector", "Regional-Copying Collector",
                                       "Smooth Collector" };

// zc7fix: is_mark_good fast path may admit plain non-heap slots (g_cjMarkBadMask all-zero on
// uncoloured non-null). Count rejects before IsValidObject/IsMarkedObject.
std::atomic<size_t> g_markGoodHeapGateReject{ 0 };

std::atomic<size_t> g_geomCrossEndReject{ 0 };
std::atomic<bool> g_geomAtexit{ false };
std::atomic<uint64_t> g_neverInstalledEvent{ 0 };

HandVerdict ClassifyRawHeader(uint64_t header)
{
    if (((header >> 48) & 0x3u) == 3u) {
        return HandVerdict::Forwarded;
    }
    if ((header & 0xffffffffffffull) == 0) {
        return HandVerdict::ZeroHeader;
    }
    return HandVerdict::Usable;
}

const char* HandVerdictName(HandVerdict verdict)
{
    switch (verdict) {
        case HandVerdict::Forwarded: return "Forwarded";
        case HandVerdict::ZeroHeader: return "ZeroHeader";
        case HandVerdict::Usable: return "Usable";
    }
    return "Unknown";
}

const char* CarrierStateName(ForwardingTable::CarrierState state)
{
    switch (state) {
        case ForwardingTable::CarrierState::ActiveUnpublished: return "active_unpublished";
        case ForwardingTable::CarrierState::ActiveOpen: return "active_open";
        case ForwardingTable::CarrierState::ActiveClosed: return "active_closed";
        case ForwardingTable::CarrierState::Retired: return "retired";
    }
    return "unknown";
}

const char* ToAnswerName(ForwardingTable::ToAnswer answer)
{
    switch (answer) {
        case ForwardingTable::ToAnswer::ArmedHit: return "armed_hit";
        case ForwardingTable::ToAnswer::ArmedMiss: return "armed_miss";
        case ForwardingTable::ToAnswer::Unavailable: return "unavailable";
        case ForwardingTable::ToAnswer::Unarmed: return "unarmed";
    }
    return "unknown";
}

class BoundedDiagnosticBuffer {
public:
    BoundedDiagnosticBuffer(char* storage, size_t capacity) : data(storage), cap(capacity)
    {
        if (cap != 0) {
            data[0] = '\0';
        }
    }

    void Append(const char* format, ...)
    {
        if (truncated || used >= cap) {
            truncated = true;
            return;
        }
        va_list args;
        va_start(args, format);
        const int n = std::vsnprintf(data + used, cap - used, format, args);
        va_end(args);
        if (n < 0 || static_cast<size_t>(n) >= cap - used) {
            used = cap == 0 ? 0 : cap - 1;
            truncated = true;
            return;
        }
        used += static_cast<size_t>(n);
    }

    bool Truncated() const { return truncated; }

private:
    char* data;
    size_t cap;
    size_t used{ 0 };
    bool truncated{ false };
};

void EnsureGeomAtexit()
{
    bool expected = false;
    if (g_geomAtexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() {
            std::fprintf(stderr, "[GCV2][tailslot] geom_cross_end=%zu\n",
                         g_geomCrossEndReject.load(std::memory_order_relaxed));
            std::fflush(stderr);
        });
    }
}

// Smallest plausible TypeInfo / binary address without touching tip payload.
//
// markfloor caught RawArray+8 with small MArray::length (e.g. 0x200) via 64KiB.
// fys0segv: same interior shape with **large** length (observed tip=0x1fda868 /
// 0x2793ea8 under e75 ALOT FYS=0 → GetSize SEGV at tip+8, clear_satb young mark).
// Length is a size count; PIE TypeInfo / TIM mmap live well above 4GiB under ASLR.
// Raising the floor rejects large-length interiors so TryRecoverInteriorBase can
// re-host them — does NOT relax the gate (stricter reject set only).
constexpr uintptr_t kMinPlausibleTypeInfoAddr = 0x100000000ULL;

// fysfloor3: TypeInfo never lives at N*4GiB. TIM mmap(nullptr) 1MB arenas and
// PIE/static modules always have a non-zero page offset. Windows ImageBase
// 0x140000000 has low32=0x40000000 ≠ 0. Observed FYS=0 GetSize MAPERR family
// (compile+24GB N=20): tip=0x{3,5,6,7,8,9,b,d,19}00000000 = 15/20.
// Rejecting low32==0 is stricter-only — does not relax the gate.
inline bool TipLow32IsZero(uintptr_t tipAddr)
{
    return (tipAddr & 0xffffffffULL) == 0;
}

bool ObjectFitsInRegion(BaseObject* obj, RegionInfo* region)
{
    if (obj == nullptr || region == nullptr) {
        return false;
    }
    if (region->IsLargeRegion()) {
        return true;
    }
    MAddress objAddr = reinterpret_cast<MAddress>(obj);
    MAddress regionEnd = region->GetRegionEnd();
    if (objAddr >= regionEnd) {
        return false;
    }
    size_t objSize = obj->GetSize();
    if (objSize == 0 || (objSize % 8) != 0) {
        return false;
    }
    return objSize <= (regionEnd - objAddr);
}

// interiorsrc2: classify tip word without calling IsVaildType (may SEGV on bad tip).
bool TipWordLooksLikeTypeInfo(uintptr_t tipAddr)
{
    if (tipAddr == 0 || tipAddr < kMinPlausibleTypeInfoAddr) {
        return false;
    }
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return false;
    }
    if (TipLow32IsZero(tipAddr)) {
        return false;
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    if (!TypeInfoManager::GetTypeInfoManager().IsResidentTypeInfoAddress(tipAddr)) {
        return false;
    }
    return true;
}

// If obj is interior into a managed object, return offset (8..64) else 0.
// Only peeks tip at obj-k; never walks payload. n7 GetSize crash was +40.
unsigned ClassifyInteriorOffset(BaseObject* obj)
{
    auto base = reinterpret_cast<uintptr_t>(obj);
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(base);
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion() ||
        region->GetRegionType() == RegionInfo::RegionType::FREE_REGION) {
        return 0;
    }
    unsigned offset = 0;
    for (unsigned k : { 8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u }) {
        if (base < k) {
            continue;
        }
        auto* cand = reinterpret_cast<BaseObject*>(base - k);
        if (!Heap::IsHeapAddress(cand)) {
            continue;
        }
        RegionInfo* candidateRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(cand));
        if (candidateRegion == nullptr || candidateRegion != region || candidateRegion->IsFreeRegion() ||
            candidateRegion->IsGarbageRegion() ||
            candidateRegion->GetRegionType() == RegionInfo::RegionType::FREE_REGION) {
            continue;
        }
        // Safe: tip is first word; heap address already checked.
        uintptr_t tipAddr = reinterpret_cast<uintptr_t>(cand->GetTypeInfo());
        if (TipWordLooksLikeTypeInfo(tipAddr)) {
            if (offset != 0) {
                return 0;
            }
            offset = k;
        }
    }
    return offset;
}

// introot: host object for a heap interior (RawArray+8/...). nullptr if not interior.
// writeback2: knownBase from derived pairing wins over 8/16/24/32 tip scan.
BaseObject* RecoverInteriorBaseImpl(BaseObject* obj, BaseObject* knownBase)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return nullptr;
    }
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(obj->GetTypeInfo());
    if (TipWordLooksLikeTypeInfo(tipAddr)) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
        if (region != nullptr && ObjectFitsInRegion(obj, region)) {
            return nullptr;
        }
    }
    if (knownBase != nullptr && Heap::IsHeapAddress(knownBase) && knownBase != obj) {
        uintptr_t hostTip = reinterpret_cast<uintptr_t>(knownBase->GetTypeInfo());
        if (TipWordLooksLikeTypeInfo(hostTip)) {
            uintptr_t o = reinterpret_cast<uintptr_t>(obj);
            uintptr_t b = reinterpret_cast<uintptr_t>(knownBase);
            if (o > b && (o - b) <= 4096u) {
                return knownBase;
            }
        }
    }
    unsigned off = ClassifyInteriorOffset(obj);
    if (off == 0) {
        return nullptr;
    }
    return reinterpret_cast<BaseObject*>(reinterpret_cast<uintptr_t>(obj) - off);
}
} // namespace

void MaskEquivAtexitReport() {}

bool MaskEquivOn()
{
    return false;
}

bool MaskEquivInjectOn()
{
    return false;
}

void MaskEquivCheck(const EpochColours& e, const BadMasks& m)
{
    (void)e;
    (void)m;
}

bool Collector::MarkGoodHeapGate(const char* site, BaseObject* target)
{
    (void)site;
    if (Heap::IsHeapAddress(target)) {
        return true;
    }
    g_markGoodHeapGateReject.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Collector::ReportMarkGoodHeapGateCounts() {}

bool PlausibleManagedObjectGate(const char* site, BaseObject* obj)
{
    if (obj == nullptr) {
        return false;
    }
    if (!Heap::IsHeapAddress(obj)) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion() ||
        region->GetRegionType() == RegionInfo::RegionType::FREE_REGION) {
        return false;
    }
    TypeInfo* tip = obj->GetTypeInfo();
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if (tipAddr == 0 || tipAddr < kMinPlausibleTypeInfoAddr ||
        (tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0 || TipLow32IsZero(tipAddr) ||
        Heap::IsHeapAddress(tipAddr) ||
        !TypeInfoManager::GetTypeInfoManager().IsResidentTypeInfoAddress(tipAddr)) {
        return false;
    }
    if (!ObjectFitsInRegion(obj, region)) {
        EnsureGeomAtexit();
        size_t gn = g_geomCrossEndReject.fetch_add(1, std::memory_order_relaxed) + 1;
        if (gn <= 16 || (gn & 0x3ffU) == 0) {
            MAddress objAddr = reinterpret_cast<MAddress>(obj);
            MAddress rEnd = region->GetRegionEnd();
            LOG(RTLOG_ERROR,
                "[GCV2][tailslot] REJECT site=%s obj=%p tip=%p objSize=%zu regionEnd=%#zx remain=%zu n=%zu",
                site, obj, tip, obj->GetSize(), static_cast<size_t>(rEnd),
                objAddr < rEnd ? static_cast<size_t>(rEnd - objAddr) : 0, gn);
        }
        return false;
    }
    return true;
}

bool Collector::PlausibleManagedObjectGate(const char* site, BaseObject* obj)
{
    return MapleRuntime::PlausibleManagedObjectGate(site, obj);
}

BaseObject* Collector::TryRecoverInteriorBase(BaseObject* obj, BaseObject* knownBase)
{
    return RecoverInteriorBaseImpl(obj, knownBase);
}

void Collector::ReportPlausibleManagedObjectGateCounts()
{
    std::fprintf(stderr, "[GCV2][tailslot] geom_cross_end=%zu\n",
                 g_geomCrossEndReject.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

// F5: when FindToVersion returns null, never silently hand back a dead/zeroed from.
// Legal null (high-live / raw-pin survivor still at from, ghost=0) keeps returning obj.
// Illegal null (D: old tag + ghost already dispelled + from cleared) fails loudly here.
// See reports/REPORT-nullenum.md LEGAL_NULL_SET; reports/REPORT-tagaba.md F5.
// Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94.
BaseObject* Collector::FindLatestVersion(BaseObject* obj, const ForwardingProvenance& provenance) const
{
    if (obj == nullptr) {
        return nullptr;
    }

    BaseObject* to = FindToVersion(obj).GetOrFailClosed("Collector::FindLatestVersion", provenance);
    if (to != nullptr) {
        if (to != obj && Heap::IsHeapAddress(to) && !to->IsValidObject()) {
            CHECK_DETAIL(obj->IsValidObject(),
                         "FindLatestVersion: route dest %p has no tip and from %p is not valid",
                         to, obj);
            return obj;
        }
        return to;
    }
    CHECK_DETAIL(obj->IsValidObject(),
                 "FindLatestVersion: no to-version for invalid from-object %p "
                 "(stale old-tag after ghost dispel; do not fall back to from)",
                 obj);
    return obj;
}

// The positional table this replaced still carried names from an older phase
// enum, so indices 12, 13 and 14 printed "forward phase", "enum fix phase" and
// "trace fix phase" for POST_TRACE, PREFORWARD and FORWARD. Every crash report
// naming a phase past CLEAR_SATB_BUFFER therefore named the wrong one, and a
// reader comparing two reports could not tell. Switching on the enum keeps the
// name attached to the value, so adding a phase is a compile error here rather
// than a silent relabelling of the phases after it.
const char* Collector::GetGCPhaseName(GCPhase phase)
{
    switch (phase) {
        case GC_PHASE_UNDEF: return "undefined phase";
        case GC_PHASE_IDLE: return "idle phase";
        case GC_PHASE_FINISH: return "finish phase";
        case GC_PHASE_RECLAIM_SATB_NODE: return "reclaim satb phase";
        case GC_PHASE_INIT: return "init phase";
        case GC_PHASE_ENUM: return "enum phase";
        case GC_PHASE_TRACE: return "trace phase";
        case GC_PHASE_CLEAR_SATB_BUFFER: return "clear satb phase";
        case GC_PHASE_POST_TRACE: return "post trace phase";
        case GC_PHASE_PREFORWARD: return "preforward phase";
        case GC_PHASE_FORWARD: return "forward phase";
    }
    return "unknown phase";
}

Collector::Collector() {}

const char* Collector::GetCollectorName() const { return COLLECTOR_NAME[collectorType]; }

void Collector::RequestGC(GCReason reason, bool async)
{
    RequestGCInternal(reason, async);
}

// loadfc: best-effort detection verdict. Same header-word shape as Barrier.cpp's former staleguard
// judge (StateWord.h:215-228: bits 0-47 TypeInfo, bits 48-49 stateCode; FORWARDED=3). This one
// relaxed read classifies the observed word; it does not establish object lifetime or happens-before.
HandVerdict Collector::JudgeHandOutTarget(BaseObject* target)
{
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return HandVerdict::Usable;
    }
    const uint64_t hdr = __atomic_load_n(reinterpret_cast<const uint64_t*>(target), __ATOMIC_RELAXED);
    return ClassifyRawHeader(hdr);
}

uint64_t Collector::EmitNeverInstalledDiagnostic(BaseObject* target, uintptr_t rawSlotBits,
                                                 MAddress witnessStart, uint64_t witnessEpoch,
                                                 uint64_t witnessLife, bool witnessValid)
{
    const uint64_t event = g_neverInstalledEvent.fetch_add(1, std::memory_order_relaxed) + 1;
    const MAddress address = target == nullptr ? 0 : reinterpret_cast<MAddress>(target);
    const uint64_t rawHeader = (target != nullptr && Heap::IsHeapAddress(target))
        ? __atomic_load_n(reinterpret_cast<const uint64_t*>(target), __ATOMIC_RELAXED)
        : 0;
    const HandVerdict verdict = ClassifyRawHeader(rawHeader);
    const ForwardingTable::NeverInstalledSnapshot snapshot =
        ForwardingTable::CaptureNeverInstalledSnapshot(address);

    RegionInfo* region = (target != nullptr && Heap::IsHeapAddress(target))
        ? RegionInfo::TryGetRegionInfoAt(address)
        : nullptr;
    const MAddress regionStart = region == nullptr ? 0 : region->GetRegionStart();
    const unsigned regionType = region == nullptr ? 0xffu : static_cast<unsigned>(region->GetRegionType());
    const unsigned generation = region == nullptr ? 0xffu : static_cast<unsigned>(region->generation_id());
    const uint64_t currentEpoch = region == nullptr ? 0 : region->GetSnapshotEpoch();
    const RegionLifeId currentLife = region == nullptr ? 0 : region->GetRegionLifeId();
    const bool sameWitnessIncarnation = witnessValid && region != nullptr && witnessStart == regionStart &&
        witnessLife != 0 && witnessLife == currentLife;
    char witnessEpochDelta[48];
    if (sameWitnessIncarnation) {
        (void)std::snprintf(witnessEpochDelta, sizeof(witnessEpochDelta), "%lld",
                            static_cast<long long>(static_cast<int64_t>(currentEpoch) -
                                                   static_cast<int64_t>(witnessEpoch)));
    } else {
        (void)std::snprintf(witnessEpochDelta, sizeof(witnessEpochDelta), "%s",
                            witnessValid && region != nullptr && witnessLife != 0
                                ? "n/a(reused)" : "n/a(no-incarnation)");
    }
    bool stateMachineViolation = false;
    for (size_t i = 0; i < snapshot.carrierCount; ++i) {
        const ForwardingTable::CarrierState state = snapshot.carriers[i].state;
        if (state == ForwardingTable::CarrierState::ActiveUnpublished ||
            state == ForwardingTable::CarrierState::ActiveOpen ||
            state == ForwardingTable::CarrierState::ActiveClosed) {
            stateMachineViolation = true;
        }
    }

    char carriers[6144];
    BoundedDiagnosticBuffer carrierText(carriers, sizeof(carriers));
    carrierText.Append("[");
    for (size_t i = 0; i < snapshot.carrierCount; ++i) {
        const ForwardingTable::CarrierIdentity& carrier = snapshot.carriers[i];
        const bool sameIncarnation = region != nullptr && carrier.start == regionStart &&
            carrier.fromPageLifeId != 0 && carrier.fromPageLifeId == currentLife;
        carrierText.Append(
            "%s{table_id=%#zx,start=%#zx,size=%zu,table_generation=%u,publication_generation=%llu,"
            "from_page_epoch=%llu,lifeId=%llu,state=%s,answer=%s,pending_destroy=%u,epoch_delta=",
            i == 0 ? "" : ",", static_cast<size_t>(carrier.tableId),
            static_cast<size_t>(carrier.start), carrier.size,
            static_cast<unsigned>(carrier.tableGeneration),
            static_cast<unsigned long long>(carrier.publicationGeneration),
            static_cast<unsigned long long>(carrier.fromPageEpoch),
            static_cast<unsigned long long>(carrier.fromPageLifeId),
            CarrierStateName(carrier.state), ToAnswerName(carrier.answer), carrier.pendingDestroy ? 1u : 0u);
        if (sameIncarnation) {
            const int64_t delta = static_cast<int64_t>(currentEpoch) -
                static_cast<int64_t>(carrier.fromPageEpoch);
            carrierText.Append("%lld}", static_cast<long long>(delta));
        } else if (region != nullptr && carrier.fromPageLifeId != 0) {
            carrierText.Append("n/a(reused)}");
        } else {
            carrierText.Append("n/a(no-incarnation)}");
        }
    }
    carrierText.Append("]");

    char receipts[2048];
    BoundedDiagnosticBuffer receiptText(receipts, sizeof(receipts));
    receiptText.Append("[");
    for (size_t i = 0; i < snapshot.reverseCount; ++i) {
        const ForwardingTable::ReverseReceiptIdentity& receipt = snapshot.reverseReceipts[i];
        receiptText.Append("%s{table_id=%#zx,publication_generation=%llu,from=%#zx}",
                           i == 0 ? "" : ",", static_cast<size_t>(receipt.tableId),
                           static_cast<unsigned long long>(receipt.publicationGeneration),
                           static_cast<size_t>(receipt.from));
    }
    receiptText.Append("]");

    std::fprintf(
        stderr,
        "[FINDTO][never-installed] never_installed_event=%llu target=%p raw_slot_bits=%#zx raw_target_header=%#llx "
        "hand_verdict=%s current_region_start=%#zx current_region_type=%u current_generation=%u "
        "current_page_epoch=%llu current_lifeId=%llu witness_start=%#zx witness_from_page_epoch=%llu "
        "witness_lifeId=%llu witness_epoch_delta="
        "%s covering_total=%zu covering_emitted=%zu "
        "carrier_overflow=%u carriers=%s reverse_total=%zu reverse_emitted=%zu reverse_overflow=%u "
        "reverse_receipts=%s scan_overflow=%u format_overflow=%u historical_writer=unknown "
        "historical_slot_colour=unknown current_writer_role=consumer state_machine_violation=%u\n",
        static_cast<unsigned long long>(event), static_cast<void*>(target),
        static_cast<size_t>(rawSlotBits), static_cast<unsigned long long>(rawHeader),
        HandVerdictName(verdict), static_cast<size_t>(regionStart), regionType, generation,
        static_cast<unsigned long long>(currentEpoch), static_cast<unsigned long long>(currentLife),
        static_cast<size_t>(witnessStart), static_cast<unsigned long long>(witnessEpoch),
        static_cast<unsigned long long>(witnessLife),
        witnessEpochDelta,
        snapshot.carrierTotal, snapshot.carrierCount, snapshot.carrierOverflow ? 1u : 0u, carriers,
        snapshot.reverseTotal, snapshot.reverseCount, snapshot.reverseOverflow ? 1u : 0u, receipts,
        snapshot.scanOverflow ? 1u : 0u,
        (carrierText.Truncated() || receiptText.Truncated()) ? 1u : 0u,
        stateMachineViolation ? 1u : 0u);
    (void)std::fflush(stderr);
    CHECK_DETAIL(!stateMachineViolation,
                 "[FINDTO][never-installed-state] event=%llu active carrier survived publication close",
                 static_cast<unsigned long long>(event));
    return event;
}

// loadfc (zBarrier.inline.hpp:327-343): the slow path must produce a verified current version or
// stop the mutator in a controlled, attributable place -- never hand back a structurally dead
// from-address. The [LOADFC] tag is the population-accounting signature.
[[noreturn]] void Collector::FailClosedLoad(const char* site, BaseObject* target, uintptr_t slotBits,
                                            const ForwardingProvenance& provenance)
{
    const HandVerdict verdict = JudgeHandOutTarget(target);
    const MAddress from = target != nullptr ? reinterpret_cast<MAddress>(target) : 0;
    RegionInfo* region = (from != 0 && Heap::IsHeapAddress(target) && verdict != HandVerdict::ZeroHeader)
        ? RegionInfo::TryGetRegionInfoAt(from)
        : nullptr;
    const bool canLookup = from != 0 && Heap::IsHeapAddress(target) && verdict != HandVerdict::ZeroHeader;
    const ForwardingTable::LookupResult lookup = canLookup
        ? ForwardingTable::LookupTo(from)
        : ForwardingTable::LookupResult{ 0, ForwardingTable::ToAnswer::Unarmed,
                                         ForwardingTable::ToUnavailableCause::None, false, false,
                                         ForwardingTable::ToAnswer::Unarmed,
                                         ForwardingTable::ToAnswer::Unarmed, false, false, 0 };
    // This is the last-chance diagnostic (zBarrier.inline.hpp:327-343). Pre-init callers, including
    // gc_unit other-vm children, have CollectorResources but no CollectorProxy target to query.
    const unsigned gcPhase = Heap::GetHeap().GetCollectorResources().IsGcStarted()
        ? static_cast<unsigned>(Heap::GetHeap().GetGCPhase())
        : 0xffu;
    std::fprintf(stderr,
                 "[LOADFC][fail-closed] site=%s target=%p verdict=%u slotBits=%#zx "
                 "consumer=%s holder_kind=%s holder=%p slot=%p stage=%s writer_kind=%s "
                 "incoming_source_kind=%s source_slot=%p working_copy_slot=%p "
                 "field_type=%s field_offset=%zu from=%p from_region=%p "
                 "region_type=%u generation=%u in_current_relocation_set=%u "
                 "table_id=%#zx publication_generation=%llu from_page_epoch=%llu lifeId=%llu "
                 "lookup_state=%u lookup_cause=%u retired_lookup=%u gc_phase=%u "
                 "unresolved non-Usable from-address must not be handed out\n",
                 site != nullptr ? site : "?", static_cast<void*>(target),
                 static_cast<unsigned>(verdict), slotBits,
                 site != nullptr ? site : "unknown",
                 ForwardingProvenance::KindName(provenance.kind),
                 provenance.holder, provenance.slot,
                 ForwardingProvenance::StageName(provenance.stage),
                 ForwardingProvenance::WriterName(provenance.writerKind),
                 ForwardingProvenance::SourceName(provenance.incomingSourceKind), provenance.sourceSlot,
                 provenance.workingCopySlot, ForwardingProvenance::FieldName(provenance.fieldKind),
                 provenance.fieldOffset, static_cast<void*>(target),
                 static_cast<void*>(region),
                 region != nullptr ? static_cast<unsigned>(region->GetRegionType()) : 0xffu,
                 region != nullptr ? static_cast<unsigned>(region->generation_id()) : 0xffu,
                 lookup.currentMembership ? 1u : 0u,
                 static_cast<size_t>(lookup.tableId),
                 static_cast<unsigned long long>(lookup.publicationGeneration),
                 static_cast<unsigned long long>(lookup.fromPageEpoch),
                 static_cast<unsigned long long>(lookup.fromPageLifeId),
                 static_cast<unsigned>(lookup.answer),
                 static_cast<unsigned>(lookup.unavailableCause),
                 static_cast<unsigned>(lookup.retiredAnswer),
                 gcPhase);
    (void)fflush(stderr);
    (void)fflush(stdout);
    std::abort();
}

// Virtual default: this collector type does not implement the method. Always abort;
// body is out-of-line so Collector.h stays free of FormatLog / string payloads.
[[noreturn]] void Collector::AbortUnimplemented(const char* method)
{
    Logger::GetLogger().FormatLog(RTLOG_FATAL, true,
                                  "unimplemented virtual %s on this Collector "
                                  "(base default must not be reached)",
                                  method != nullptr ? method : "?");
    std::abort();
}
} // namespace MapleRuntime.
