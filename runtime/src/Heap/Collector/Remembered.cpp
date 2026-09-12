// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/WCollector/WCollector.h"
#include "Heap/WCollector/RememberedHolderPolicy.h"
#include "Heap/Verify/ProbeReadRouteDiag.h"

#include <array>
#include <atomic>
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include <csignal>
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Base/SysCall.h"
#endif
#include "Concurrency/Concurrency.h"
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Collector/GcTriggerFlags.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/Collector/TenuringThreshold.h"
#include "Heap/GcThreadPool.h"
#include "Heap/HeapWork.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Heap/WCollector/UntagRefFieldBreadcrumb.h"
#endif
#include "Heap/Verify/VerifyHeap.h"
#include "Heap/Verify/MarkCompleteVerify.h"
#include "Heap/Verify/VerifyOption.h"
#include "Heap/Verify/VerifyRememberedSet.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/VerifyRoots.h"
#include "Heap/Verify/Zap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/NwDropAudit.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/Stw2CurrentAudit.h"
#include "Heap/Verify/SurvNodeDiag.h"
#include "Heap/Collector/PromotedRegionDomain.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Collector/ZForwarding.h"
#include "Heap/Verify/CsetEmptyWho.h"
#include "Common/ColourPredicates.h"
#include "Heap/WCollector/RemapYoungRoots.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Verify/VerifyRegions.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "securec.h"
#endif
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
namespace {
struct RemsetFilterReceiptState {
    std::atomic<uint64_t> seen { 0 };
    std::atomic<uint64_t> consumed { 0 };
    std::atomic<uint64_t> stale { 0 };
    std::atomic<uint64_t> deadHolder { 0 };
    std::atomic<uint64_t> noOrigin { 0 };
    std::atomic<uint64_t> badTarget { 0 };
    std::atomic<MAddress> lastConsumedSlot { 0 };
    std::atomic<MAddress> lastStaleSlot { 0 };
    std::atomic<MAddress> lastDeadHolderSlot { 0 };
    std::atomic<MAddress> lastNoOriginSlot { 0 };
    std::atomic<MAddress> lastBadTargetSlot { 0 };
};
RemsetFilterReceiptState g_remsetFilterReceipt;
}

void ResetRemsetFilterTestReceipt()
{
    g_remsetFilterReceipt.seen.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.consumed.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.stale.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.deadHolder.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.noOrigin.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.badTarget.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastConsumedSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastStaleSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastDeadHolderSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastNoOriginSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastBadTargetSlot.store(0, std::memory_order_relaxed);
}

RemsetFilterTestReceipt ReadRemsetFilterTestReceipt()
{
    return { g_remsetFilterReceipt.seen.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.consumed.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.stale.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.deadHolder.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.noOrigin.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.badTarget.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastConsumedSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastStaleSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastDeadHolderSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastNoOriginSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastBadTargetSlot.load(std::memory_order_relaxed) };
}

void NoteRemsetFilterTestReceipt(MAddress slot, RemsetFilterReceiptReason reason, bool consumed)
{
    if (slot == 0) {
        return;
    }
    g_remsetFilterReceipt.seen.fetch_add(1, std::memory_order_relaxed);
    if (consumed) {
        g_remsetFilterReceipt.consumed.fetch_add(1, std::memory_order_relaxed);
        g_remsetFilterReceipt.lastConsumedSlot.store(slot, std::memory_order_relaxed);
    }
    switch (reason) {
        case RemsetFilterReceiptReason::kStale:
            g_remsetFilterReceipt.stale.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastStaleSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kDeadHolder:
            g_remsetFilterReceipt.deadHolder.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastDeadHolderSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kNoOrigin:
            g_remsetFilterReceipt.noOrigin.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastNoOriginSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kBadTarget:
            g_remsetFilterReceipt.badTarget.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastBadTargetSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kNone:
            break;
    }
}
#endif
// nullslot: count product paths that CAS-install nullptr into a ref field.
// MRT_GCV2_NULLSLOT=1 → LOG each write (cap 64/path) + totals; default off.
// rootdrop: same gate also arms path=resolve_root_null (RootSlot HealRoot null).
#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif
namespace WCollectorInternal {
bool NullslotProbeEnabled()
{
    static const bool on = []() {
        return DiagGate::LegacyOrToken("MRT_GCV2_NULLSLOT", "nullslot");
    }();
    return on;
}

std::atomic<size_t> g_nullslotF3{ 0 };
std::atomic<size_t> g_nullslotResolve{ 0 };
std::atomic<size_t> g_nullslotRemset{ 0 };
std::atomic<size_t> g_nullslotResolveRoot{ 0 };
// rootdrop entry accounting (always-on atomics; LOG only when MRT_GCV2_NULLSLOT=1).
std::atomic<size_t> g_resolveRootEntry{ 0 };
std::atomic<size_t> g_resolveRootOld{ 0 };
std::atomic<size_t> g_resolveRootHealNull{ 0 };
std::atomic<size_t> g_fixMinorRootSlotsCalls{ 0 };
std::atomic<size_t> g_findtoPostLifecycleSoft{ 0 };

// ZGC zPage.inline.hpp:254-256: is_object_live = is_allocating || livemap.
// zBarrier.inline.hpp:73-78: never heal a non-null slot with null.
// 4fcf746a used IsMarkedObject<Old> only — post-flip to-space and young
// holders have no Old face, so F3 / Resolve / Scrub planted null into live
// Array slots (nwreclaim: pc_off=0x29589 mov 0x8(%rcx) rcx=0).
bool RegionIsAllocatingPage(const RegionInfo* region)
{
    if (region == nullptr) {
        return false;
    }
    const RegionInfo::RegionType type = region->GetRegionType();
    return region->IsToRegion() || region->IsThreadLocalRegion() ||
        type == RegionInfo::RegionType::RECENT_FULL_REGION ||
        type == RegionInfo::RegionType::RECENT_LARGE_REGION ||
        type == RegionInfo::RegionType::TL_RAW_POINTER_REGION ||
        type == RegionInfo::RegionType::TL_LARGE_RAW_POINTER_REGION ||
        region->IsPinnedRegion() || region->HasMarkStartAllocGap();
}

bool HolderObjectIsLive(BaseObject* holder)
{
    if (holder == nullptr || !Heap::IsHeapAddress(holder) || !holder->IsValidObject()) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return false;
    }
    if (RegionIsAllocatingPage(region)) {
        return true;
    }
    // ZGC answers this from the page livemap until relocation completes
    // (zPage.inline.hpp:239-240). Our current LiveInfo face can already have
    // been unbound here, so use the mark-time retained copy while it covers
    // this holder and still belongs to the current old-generation epoch.
    MAddress holderAddress = reinterpret_cast<MAddress>(holder);
    RegionInfo::RetainedLiveInfoState retainedState = region->GetRetainedLiveInfoState();
    if (retainedState != RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED &&
        region->IsRetainedSnapshotValid() &&
        holderAddress < region->GetRetainedLiveInfoCoveredUpTo()) {
        size_t holderOffset = region->GetAddressOffset(holderAddress);
        return retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID &&
            region->HasRetainedMarkWords() && region->RetainedMarkWordsSay(holderOffset);
    }
    if (region->IsYoungRegion()) {
        return RegionSpace::IsMarkedObject<Generation::Young>(holder);
    }
    return RegionSpace::IsMarkedObject<Generation::Old>(holder);
}

bool SlotHeldByLiveObject(const void* slot)
{
    if (slot == nullptr || !Heap::IsHeapAddress(slot)) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(slot));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return false;
    }
    if (RegionIsAllocatingPage(region)) {
        return true;
    }
    BaseObject* holder = Collector::TryRecoverInteriorBase(
        reinterpret_cast<BaseObject*>(const_cast<void*>(slot)));
    return HolderObjectIsLive(holder);
}

void NoteNullslotWrite(const char* path, BaseObject* holder, void* field, BaseObject* from, BaseObject* latest,
                       std::atomic<size_t>* pathCount)
{
    size_t n = pathCount->fetch_add(1, std::memory_order_relaxed);
    if (!NullslotProbeEnabled() || n >= 64) {
        return;
    }
    GCPhase phase = Heap::GetHeap().GetGCPhase();
    LOG(RTLOG_ERROR,
        "[GCV2][nullslot] path=%s n=%zu holder=%p field=%p from=%p latest=%p phase=%s(%u) "
        "holderValid=%d fromHeap=%u latestHeap=%u",
        path, n, holder, field, from, latest, Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase),
        holder != nullptr && Heap::IsHeapAddress(holder) ? static_cast<int>(holder->IsValidObject()) : -1,
        static_cast<unsigned>(from != nullptr && Heap::IsHeapAddress(from)),
        static_cast<unsigned>(latest != nullptr && Heap::IsHeapAddress(latest)));
}

// Classify why ResolveMinorReference(RootSlot) live-predicates rejected to/from.
// Gate: NullslotProbeEnabled (MRT_GCV2_NULLSLOT=1); default off — never on hot path alone.
// Never touch object headers when region is free/garbage (madvise / recycled).
const char* ClassifyRootLiveFail(BaseObject* obj, RegionInfo* region)
{
    if (obj == nullptr) {
        return "obj_null";
    }
    if (!Heap::IsHeapAddress(obj)) {
        return "not_heap";
    }
    if (region == nullptr) {
        return "no_region";
    }
    if (region->IsFreeRegion()) {
        return "free";
    }
    if (region->IsGarbageRegion()) {
        return "garbage";
    }
    // Only touch header when region still claims to own live units.
    if (!obj->IsValidObject()) {
        return "invalid_object";
    }
    return "live_ok";
}

void NoteResolveRootNull(void* rootSlot, BaseObject* from, BaseObject* to, RegionInfo* fromRegion,
                         RegionInfo* toRegion, const char* toWhy, const char* fromWhy)
{
    size_t n = g_nullslotResolveRoot.fetch_add(1, std::memory_order_relaxed);
    if (!NullslotProbeEnabled() || n >= 64) {
        return;
    }
    GCPhase phase = Heap::GetHeap().GetGCPhase();
    unsigned fromRtype = fromRegion != nullptr ? static_cast<unsigned>(fromRegion->GetRegionType()) : 0xffu;
    unsigned toRtype = toRegion != nullptr ? static_cast<unsigned>(toRegion->GetRegionType()) : 0xffu;
    unsigned fromRoute = fromRegion != nullptr ? static_cast<unsigned>(fromRegion->GetRouteState()) : 0xffu;
    unsigned toRoute = toRegion != nullptr ? static_cast<unsigned>(toRegion->GetRouteState()) : 0xffu;
    unsigned fromYoung = fromRegion != nullptr ? static_cast<unsigned>(fromRegion->IsYoungRegion()) : 0xffu;
    unsigned toYoung = toRegion != nullptr ? static_cast<unsigned>(toRegion->IsYoungRegion()) : 0xffu;
    int fromMarked = -1;
    int toMarked = -1;
    // Skip mark/valid probes on free/garbage — header may be unmapped.
    const bool fromSafe = from != nullptr && fromRegion != nullptr && Heap::IsHeapAddress(from) &&
                          !fromRegion->IsFreeRegion() && !fromRegion->IsGarbageRegion();
    const bool toSafe = to != nullptr && toRegion != nullptr && Heap::IsHeapAddress(to) &&
                        !toRegion->IsFreeRegion() && !toRegion->IsGarbageRegion();
    if (fromSafe) {
        if (fromRegion->IsYoungRegion()) {
            auto view = fromRegion->GetMarkView<Generation::Young>();
            fromMarked = static_cast<int>(fromRegion->IsMarkedObject(view, from));
        } else {
            auto view = fromRegion->GetMarkView<Generation::Old>();
            fromMarked = static_cast<int>(fromRegion->IsMarkedObject(view, from));
        }
    }
    if (toSafe) {
        if (toRegion->IsYoungRegion()) {
            auto view = toRegion->GetMarkView<Generation::Young>();
            toMarked = static_cast<int>(toRegion->IsMarkedObject(view, to));
        } else {
            auto view = toRegion->GetMarkView<Generation::Old>();
            toMarked = static_cast<int>(toRegion->IsMarkedObject(view, to));
        }
    }
    int fromValid = fromSafe ? static_cast<int>(from->IsValidObject()) : -1;
    int toValid = toSafe ? static_cast<int>(to->IsValidObject()) : -1;
    // fprintf+fflush: Mode A often dies in the same concurrent window; LOG may not flush.
    std::fprintf(stderr,
                 "[GCV2][nullslot] path=resolve_root_null n=%zu root=%p from=%p to=%p phase=%s(%u) "
                 "fromRtype=%u fromRoute=%u fromYoung=%u fromMarked=%d fromValid=%d fromWhy=%s "
                 "toRtype=%u toRoute=%u toYoung=%u toMarked=%d toValid=%d toWhy=%s\n",
                 n, rootSlot, from, to, Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase), fromRtype,
                 fromRoute, fromYoung, fromMarked, fromValid, fromWhy, toRtype, toRoute, toYoung, toMarked, toValid,
                 toWhy);
    std::fflush(stderr);
}
} // namespace WCollectorInternal
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif
void WCollector::ScanRelocatedRememberedFields(MinorSlotSet& rememberedSlots)
{
    std::unordered_set<ZForwarding*> forwardings;
    for (MAddress slot : rememberedSlots) {
        if (ZForwarding* forwarding = ForwardingTable::GetCovering(slot)) {
            forwardings.insert(forwarding);
        }
    }
    for (ZForwarding* forwarding : forwardings) {
        if (forwarding->retain_page()) {
            forwarding->relocated_remembered_fields_notify_concurrent_scan_of();
            forwarding->release_page();
        } else {
            forwarding->relocated_remembered_fields_apply_to_published([&](MAddress field) {
                rememberedSlots.insert(field);
            });
        }
    }
}

void WCollector::RescanRememberedSet(WorkStack& workStack, const MinorSlotSet& rememberedSlots,
                                     const MinorSlotSet& reachableSlots, const MinorSlotSet& weakSlots,
                                     const MinorObjectSet& currentMinorRoots, bool fullYoungScan,
                                      MinorSlotSet* consumedOut, RemsetScanStats* statsOut,
                                      MinorInteriorBaseMap* interiorBasesOut, const ScopedStopTheWorld* stw)
{
    (void)stw;
    auto noteRemsetOutcome = [](MAddress slot, uint8_t outcome, MAddress target) {
        if (!ProbeReadRouteDiag::RootTrackingEnabled() || slot == 0) {
            return;
        }
        const size_t start = ProbeReadRouteDiag::EdgeStoreLedger::Hash(slot);
        for (size_t n = 0; n < 8; ++n) {
            auto& record = ProbeReadRouteDiag::EdgeStoreLedger::Records()[
                (start + n) & ProbeReadRouteDiag::EdgeStoreLedger::kMask];
            if (record.slot.load(std::memory_order_acquire) != slot) {
                continue;
            }
            record.remsetEpoch.store(
                ProbeReadRouteDiag::RemsetEpoch().load(std::memory_order_relaxed), std::memory_order_relaxed);
            record.remsetTarget.store(target, std::memory_order_relaxed);
            record.remsetFace.store(0xff, std::memory_order_relaxed);
            record.remsetEvent.store(static_cast<uint8_t>(64 + outcome), std::memory_order_release);
            return;
        }
    };
    auto plannedTo = [this](BaseObject* from) -> BaseObject* {
        FindToVersionResult resolved = FindToVersion(from);
        if (resolved.is_unavailable()) {
            // The remembered-set scrub is the third non-dereference consumer:
            // an unavailable carrier drops this scan item and never installs a
            // null/old value into the slot.  Product barriers retain the
            // fail-closed abort; this scrub's single strategy is deferral.
            g_findtoPostLifecycleSoft.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        const ForwardingProvenance provenance{ ForwardingHolderKind::Remset, this, &from };
        return resolved.GetOrFailClosed("WCollector::RescanRememberedSet", provenance);
    };

    // HotSpot G1RemSet scrub analogue. ORDER matters (STEER2 / defect⑤):
    //   1) region-level holder_dead (free/garbage region only — not object liveness)
    //   2) pre-check target safety BEFORE ResolveMinorReference
    //      (old-tag with no to-version + invalid from must not reach FindLatestVersion/F5)
    //   3) ResolveMinorReference (soft-resolve; never calls FindLatestVersion)
    //   4) post-resolve null / bad_target drops
    // Does not relax IsValidObject / FindLatestVersion CHECK_DETAIL.
    static std::atomic<size_t> g_remsetScrubLogged{ 0 };
    size_t scrubbedStale = 0;
    size_t scrubbedDeadHolder = 0;
    size_t scrubbedNoTargetOrigin = 0;
    size_t recoveredTargetInterior = 0;
    size_t targetOriginSlowLookups = 0;
    size_t targetOriginIndexedRegions = 0;
    size_t targetOriginVisitedObjects = 0;
    size_t scrubbedBadTarget = 0;
    size_t scrubbedStaleOldTag = 0;
    size_t retainedDeadDropped = 0;
    size_t rootedRetainedKept = 0;
    size_t rootedStalePreserved = 0;
    size_t reRemembered = 0;
    // The precise bitmap intentionally stores only field-slot identity. Recover an
    // object origin only for regions whose retained snapshot is consumable (or when
    // the default-off probe requests visibility), and keep that adapter local to this
    // minor collection rather than adding a second persistent remset index.
    std::unordered_map<MAddress, BaseObject*> rememberedOrigins;
    std::unordered_set<RegionInfo*> originRegions;
    for (MAddress slot : rememberedSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            noteRemsetOutcome(slot, 1, 0);
            continue;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(slot);
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
            continue;
        }
        RegionInfo::RetainedLiveInfoState retainedState = region->GetRetainedLiveInfoState();
        if (retainedState != RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED &&
            region->IsRetainedSnapshotValid()) {
            originRegions.insert(region);
        }
    }
    for (RegionInfo* region : originRegions) {
        region->VisitAllObjects([&rememberedSlots, &rememberedOrigins](BaseObject* holder) {
            if (holder == nullptr || !holder->HasRefField()) {
                return;
            }
            holder->ForEachRefField([holder, &rememberedSlots, &rememberedOrigins](RefField<>& field) {
                MAddress slot = reinterpret_cast<MAddress>(&field);
                // rememberedSlots is the remset drain set (not the mark ledger); leave untimed.
                if (rememberedSlots.count(slot) != 0) {
                    rememberedOrigins[slot] = holder;
                }
            });
        });
    }
    std::unordered_set<RegionInfo*> indexedTargetRegions;
    std::unordered_map<RegionInfo*, std::vector<MAddress>> targetStarts;
    std::unordered_map<TypeInfo*, bool> knownTypeInfos;
    auto isKnownTypeInfo = [&knownTypeInfos](TypeInfo* tip) {
        auto cached = knownTypeInfos.find(tip);
        if (cached != knownTypeInfos.end()) {
            return cached->second;
        }
        bool known = TypeInfoManager::GetTypeInfoManager().ContainsTypeInfo(tip);
        knownTypeInfos.emplace(tip, known);
        return known;
    };
    auto hasKnownTypeInfo = [&isKnownTypeInfo](const char* site, BaseObject* object) {
        return Collector::PlausibleManagedObjectGate(site, object) && isKnownTypeInfo(object->GetTypeInfo());
    };
    auto recoverYoungTargetBase = [&indexedTargetRegions, &targetStarts, &targetOriginSlowLookups,
                                   &targetOriginIndexedRegions, &targetOriginVisitedObjects,
                                   &hasKnownTypeInfo](BaseObject* target) {
        MAddress address = reinterpret_cast<MAddress>(target);
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(address);
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion() || !region->IsYoungRegion()) {
            return target;
        }
        bool targetKnown = hasKnownTypeInfo("RescanRememberedSet.target", target);
        unsigned interiorCandidateCount = 0;
        for (unsigned offset : { 8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u }) {
            if (address < offset) {
                continue;
            }
            MAddress candidateAddress = address - offset;
            if (!Heap::IsHeapAddress(candidateAddress)) {
                continue;
            }
            RegionInfo* candidateRegion = RegionInfo::TryGetRegionInfoAt(candidateAddress);
            if (candidateRegion != region) {
                continue;
            }
            auto* candidate = reinterpret_cast<BaseObject*>(candidateAddress);
            if (hasKnownTypeInfo("RescanRememberedSet.targetCandidate", candidate) &&
                offset < RegionSpace::GetAllocSize(*candidate)) {
                ++interiorCandidateCount;
            }
        }
        // With no preceding header candidate, only an exact registered TypeInfo can
        // preserve the normal target path. Any candidate count (including ambiguity)
        // must be decided by the exact-start table below; plausibility alone is not
        // an object-start proof.
        if (interiorCandidateCount == 0) {
            return targetKnown ? target : static_cast<BaseObject*>(nullptr);
        }

        ++targetOriginSlowLookups;
        if (indexedTargetRegions.insert(region).second) {
            ++targetOriginIndexedRegions;
            region->VisitAllObjects([region, &targetStarts, &targetOriginVisitedObjects](BaseObject* object) {
                targetStarts[region].push_back(reinterpret_cast<MAddress>(object));
                ++targetOriginVisitedObjects;
            });
        }
        const auto& starts = targetStarts[region];
        if (std::binary_search(starts.begin(), starts.end(), address)) {
            return target;
        }

        BaseObject* recovered = nullptr;
        for (unsigned offset : { 8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u }) {
            if (address < offset) {
                continue;
            }
            MAddress candidateAddress = address - offset;
            if (!Heap::IsHeapAddress(candidateAddress)) {
                continue;
            }
            RegionInfo* candidateRegion = RegionInfo::TryGetRegionInfoAt(candidateAddress);
            if (candidateRegion != region) {
                continue;
            }
            auto* candidate = reinterpret_cast<BaseObject*>(candidateAddress);
            if (!std::binary_search(starts.begin(), starts.end(), candidateAddress)) {
                continue;
            }
            if (offset >= RegionSpace::GetAllocSize(*candidate) || recovered != nullptr) {
                return static_cast<BaseObject*>(nullptr);
            }
            recovered = candidate;
        }
        return recovered;
    };
    NwDropAudit::EnsureAtexit();
    for (MAddress slot : rememberedSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            if (statsOut != nullptr) {
                ++statsOut->skippedNotHeap;
            }
            NwDropAudit::Note(NwDropAudit::kNotHeap);
            continue;
        }
        if (LedgerCount(weakSlots, slot) != 0) {
            noteRemsetOutcome(slot, 2, 0);
            if (statsOut != nullptr) {
                ++statsOut->skippedWeak;
            }
            NwDropAudit::Note(NwDropAudit::kWeak);
            continue;
        }
        RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(slot);
        if (holderRegion == nullptr || holderRegion->IsFreeRegion() || holderRegion->IsGarbageRegion()) {
#if defined(MRT_TESTABLE_INTERNALS)
            NoteRemsetFilterTestReceipt(slot, RemsetFilterReceiptReason::kDeadHolder, false);
#endif
            noteRemsetOutcome(slot, 3, 0);
            ++scrubbedDeadHolder;
            NwDropAudit::Note(NwDropAudit::kDeadHolder);
            size_t n = g_remsetScrubLogged.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
                VLOG(REPORT,
                     "[GCV2][remset-filter] drop slot=%#zx reason=holder_dead region=%p free=%u garbage=%u",
                     static_cast<size_t>(slot), holderRegion,
                     holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->IsFreeRegion()),
                     holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->IsGarbageRegion()));
            }
            continue;
        }

        bool keepByRetainedSnapshot = true;
        BaseObject* retainedHolder = nullptr;
        auto originIt = rememberedOrigins.find(slot);
        if (originIt != rememberedOrigins.end() && originIt->second != nullptr &&
            Heap::IsHeapAddress(originIt->second)) {
            BaseObject* holder = originIt->second;
            retainedHolder = holder;
            RegionInfo* originRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (originRegion == holderRegion) {
                RegionInfo::RetainedLiveInfoState retainedState = holderRegion->GetRetainedLiveInfoState();
                if (retainedState == RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED) {
                } else if (!holderRegion->IsRetainedSnapshotValid()) {
                } else {
                    MAddress coveredUpTo = holderRegion->GetRetainedLiveInfoCoveredUpTo();
                    CHECK(coveredUpTo >= holderRegion->GetRegionStart() &&
                          coveredUpTo <= holderRegion->GetRegionAllocPtr());
                    MAddress holderAddress = reinterpret_cast<MAddress>(holder);
                    if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
                    } else {
                    }
                    if (holderAddress < coveredUpTo) {
                        if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
                            keepByRetainedSnapshot = false;
                        } else if (holderRegion->IsLargeRegion()) {
                            LiveInfo* retainedLiveInfo = holderRegion->GetRetainedLiveInfo();
                            MarkView<Generation::Old> retainedView =
                                holderRegion->GetMarkView<Generation::Old>();
                            keepByRetainedSnapshot = retainedLiveInfo != nullptr
                                ? holderRegion->IsSurvivedObject(retainedView, retainedLiveInfo, 0)
                                : holderRegion->IsSurvivedObject(retainedView, 0);
                        } else {
                            size_t holderOffset = holderRegion->GetAddressOffset(holderAddress);
                            // holderlive (F2): prefer the region's own copy of the mark bits.
                            // GetRetainedLiveInfo() is a borrowed pointer into the per-tag
                            // LiveInfo arena and is nulled by UnbindPreviousLiveInfo
                            // (DoGarbageCollection, WCollector.cpp:6122 at 7924d28f) at the end of every
                            // major, which is why this
                            // arm was unreachable — the state word read NEVER_EXAMINED before
                            // control ever got here.
                            if (holderRegion->HasRetainedMarkWords()) {
                                keepByRetainedSnapshot = holderRegion->RetainedMarkWordsSay(holderOffset);
                            } else {
                                LiveInfo* retainedLiveInfo = holderRegion->GetRetainedLiveInfo();
                                CHECK(retainedLiveInfo != nullptr);
                                MarkView<Generation::Old> retainedView =
                                    holderRegion->GetMarkView<Generation::Old>();
                                keepByRetainedSnapshot = holderRegion->IsSurvivedObject(
                                    retainedView, retainedLiveInfo, holderOffset);
                            }
                        }
                    }
                }
            }
        }
        // A young collection has no authority over old-generation liveness, so it may not
        // prune a remembered field because the old mark's retained snapshot does not claim
        // its holder.  ZRemembered::scan_field consults no holder liveness at all: it runs
        // the young-good barrier on the field, marks whatever young object it finds, and
        // re-arms the entry if the healed value is still young (zRemembered.cpp:578-589).
        // The old page's own liveness is settled by the *old* mark, which for a page promoted
        // by the previous young cycle has not run yet -- the retained snapshot cannot contain
        // it, so it reads "dead" for every such holder.
        //
        // Measured on NW256/256MB, three shots, at the first young cycle after a promoting
        // one: 550,016 of 550,025 remembered slots were dropped here and 9 admitted, the
        // young closure collapsed to ~1.9K objects against a 124 MB collection set, and all
        // 1,896 of its pages were then freed with no live map at all -- including the page a
        // live stack-rooted object still pointed into three cycles later.  The pruning is
        // deleted rather than gated: keeping it behind a switch would leave the aligned path
        // untested (0825).  keepByRetainedSnapshot / keepByCurrentRoot stay as observations.
        bool keepByCurrentRoot =
            retainedHolder != nullptr && currentMinorRoots.count(retainedHolder) != 0;
        if (!KeepRememberedHolder(keepByRetainedSnapshot, keepByCurrentRoot)) {
            ++scrubbedDeadHolder;
            ++retainedDeadDropped;
            NwDropAudit::Note(NwDropAudit::kRetained);
        }
        if (!keepByRetainedSnapshot && keepByCurrentRoot) {
            ++rootedRetainedKept;
        }

        HeapSlot<>* field = &HeapSlotAt<>(slot);
        uint64_t rawSlot = 0;
        std::memcpy(&rawSlot, field, sizeof(rawSlot));
        RefField<> peek(*field);
        BaseObject* rawTarget = to_object(peek.GetTargetObject());
        // Pre-check (before resolve): one-gen-stale old-tag whose from has no to-version
        // and is not a live object — drop without FindLatestVersion (F5 fail-closed stays).
        if (IsOldPointer(peek)) {
            BaseObject* to = plannedTo(rawTarget);
            bool fromLive = false;
            if (to == nullptr && Heap::IsHeapAddress(rawTarget)) {
                RegionInfo* fromRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(rawTarget));
                fromLive = fromRegion != nullptr && !fromRegion->IsFreeRegion() && !fromRegion->IsGarbageRegion() &&
                           rawTarget->IsValidObject();
            }
            // Non-heap target: FindToVersion null + fromLive false is expected (not dead).
            // Do not CAS-null — slot may be RO; drop remset edge only via fall-through scrub.
            if (to == nullptr && !fromLive &&
                (rawTarget == nullptr || Heap::IsHeapAddress(rawTarget))) {
                bool holderLiveBySnapshot = SlotHeldByLiveObject(field);
                if (KeepRememberedHolder(holderLiveBySnapshot, keepByCurrentRoot)) {
                    if (!holderLiveBySnapshot && keepByCurrentRoot) {
                        ++rootedStalePreserved;
                    }
                    noteRemsetOutcome(slot, 5, reinterpret_cast<MAddress>(rawTarget));
                    continue;
                }
                noteRemsetOutcome(slot, 6, reinterpret_cast<MAddress>(rawTarget));
                ++scrubbedStaleOldTag;
#if defined(MRT_TESTABLE_INTERNALS)
                NoteRemsetFilterTestReceipt(slot, RemsetFilterReceiptReason::kStale, false);
#endif
                NwDropAudit::Note(NwDropAudit::kStaleOldTag);
                // zBarrier.inline.hpp:294-343 heals only a successfully resolved
                // load-good value. This dead-holder remset cleanup therefore
                // removes the remembered-set record without manufacturing a
                // replacement field value.
                size_t n = g_remsetScrubLogged.fetch_add(1, std::memory_order_relaxed);
                if (n < 16) {
                    VLOG(REPORT,
                         "[GCV2][remset-filter] drop slot=%#zx raw=%#llx target=%p reason=stale_oldtag "
                         "(no to-version; from invalid/reclaimed — pre-resolve)",
                         static_cast<size_t>(slot), static_cast<unsigned long long>(rawSlot), rawTarget);
                }
                continue;
            }
        }

        bool preservedByCurrentRoot = false;
        BaseObject* target =
            ResolveMinorReference(*field, nullptr, keepByCurrentRoot, &preservedByCurrentRoot);
        if (preservedByCurrentRoot) {
            ++rootedStalePreserved;
        }
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            noteRemsetOutcome(slot, 7, reinterpret_cast<MAddress>(target));
            ++scrubbedStale;
            if (rawTarget != nullptr && Heap::IsHeapAddress(rawTarget) && plannedTo(rawTarget) == nullptr) {
                NwDropAudit::Note(NwDropAudit::kFindToMiss);
            } else {
                NwDropAudit::Note(NwDropAudit::kResolveNull);
            }
#if defined(MRT_TESTABLE_INTERNALS)
            NoteRemsetFilterTestReceipt(slot, RemsetFilterReceiptReason::kStale, false);
#endif
            continue;
        }
        BaseObject* targetBase = recoverYoungTargetBase(target);
        if (targetBase == nullptr) {
#if defined(MRT_TESTABLE_INTERNALS)
            NoteRemsetFilterTestReceipt(slot, RemsetFilterReceiptReason::kNoOrigin, false);
#endif
            noteRemsetOutcome(slot, 8, reinterpret_cast<MAddress>(target));
            ++scrubbedNoTargetOrigin;
            NwDropAudit::Note(NwDropAudit::kNoOrigin);
            continue;
        }
        if (targetBase != target) {
            if (interiorBasesOut != nullptr) {
                (*interiorBasesOut)[slot] = targetBase;
            }
            target = targetBase;
            ++recoveredTargetInterior;
        }
        if (!target->IsValidObject()) {
#if defined(MRT_TESTABLE_INTERNALS)
            NoteRemsetFilterTestReceipt(slot, RemsetFilterReceiptReason::kBadTarget, false);
#endif
            noteRemsetOutcome(slot, 9, reinterpret_cast<MAddress>(target));
            ++scrubbedBadTarget;
            NwDropAudit::Note(NwDropAudit::kBadTarget);
            size_t n = g_remsetScrubLogged.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
                RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                VLOG(REPORT,
                     "[GCV2][remset-filter] drop slot=%#zx raw=%#llx target=%p reason=bad_target "
                     "holderYoung=%u holderFree=%u targetYoung=%u targetFree=%u targetGarbage=%u "
                     "targetNeverExamined=%u (H1: stale remset after reclaim)",
                     static_cast<size_t>(slot), static_cast<unsigned long long>(rawSlot), target,
                     static_cast<unsigned>(holderRegion->IsYoungRegion()),
                     static_cast<unsigned>(holderRegion->IsFreeRegion()),
                     targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->IsYoungRegion()),
                     targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->IsFreeRegion()),
                     targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->IsGarbageRegion()),
                     targetRegion == nullptr
                         ? 0u
                         : static_cast<unsigned>((targetRegion->IsYoungRegion()
                               ? targetRegion->GetMarkBitmap(targetRegion->GetMarkView<Generation::Young>())
                               : targetRegion->GetMarkBitmap(targetRegion->GetMarkView<Generation::Old>())) ==
                              nullptr &&
                                                 targetRegion->GetRegionAllocPtr() > targetRegion->GetRegionStart()));
            }
            continue;
        }

        PushYoungObject(target, workStack, "remset");
        NwDropAudit::Note(NwDropAudit::kAdmit);
        if (consumedOut != nullptr) {
            consumedOut->insert(slot);
        }
        if (statsOut != nullptr) {
            ++statsOut->consumed;
        }
#if defined(MRT_TESTABLE_INTERNALS)
        NoteRemsetFilterTestReceipt(slot, RemsetFilterReceiptReason::kNone, true);
#endif
        // S1 (fysminor): re-remember on consumption, like ZGC zRemembered.cpp:578-588
        // (scan_field re-arms the entry via remember(p) whenever the healed value is
        // still young). DrainForMinor emptied the scan buffer, and the three rebuild
        // sites only cover *promoted* holders (RegionManager.cpp:258 / :328 and
        // WCollector.cpp:4688 walk reachableVec's to-versions), so a long-lived old
        // holder whose field is written once and never again loses its record after
        // one minor. Record() targets the active (next-cycle) buffer and is idempotent.
        // If the target is promoted out of young by this collection, the next Rescan
        // simply will not re-arm it, so the entry self-drains.
        RegionInfo* keepRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (keepRegion != nullptr && keepRegion->IsYoungRegion()) {
            Heap::GetHeap().GetRememberedSet().Record(slot);
            ++reRemembered;
        } else {
            noteRemsetOutcome(slot, 10, reinterpret_cast<MAddress>(target));
        }
    }
    if (scrubbedStale != 0 || scrubbedDeadHolder != 0 || scrubbedNoTargetOrigin != 0 ||
        recoveredTargetInterior != 0 || targetOriginSlowLookups != 0 || scrubbedBadTarget != 0 ||
        scrubbedStaleOldTag != 0) {
        auto typeInfoIndexShape = TypeInfoManager::GetTypeInfoManager().GetTypeInfoIndexShape();
        VLOG(REPORT,
             "[GCV2][remset-filter] summary staleTarget=%zu deadHolderRegion=%zu noTargetOrigin=%zu "
             "targetInteriorRecovered=%zu targetOriginSlowLookups=%zu targetOriginIndexedRegions=%zu "
             "targetOriginVisitedObjects=%zu typeInfoIndexEntries=%zu typeInfoIndexBuckets=%zu "
             "badTarget=%zu staleOldTag=%zu recorded=%zu "
             "(DEAD_HOLDER_DROPPED≈deadHolderRegion+staleOldTag; region-level holder_dead ≠ object-dead)",
             scrubbedStale, scrubbedDeadHolder, scrubbedNoTargetOrigin, recoveredTargetInterior,
             targetOriginSlowLookups, targetOriginIndexedRegions, targetOriginVisitedObjects,
             typeInfoIndexShape.first, typeInfoIndexShape.second, scrubbedBadTarget, scrubbedStaleOldTag,
             rememberedSlots.size());
    }
    // Printed every minor, including the zero: "re-arm never fired" and "re-arm is compiled out"
    // read identically otherwise, and this campaign has already spent a turn on that confusion.
    VLOG(REPORT, "[GCV2][remset-rearm] reRemembered=%zu scanned=%zu", reRemembered, rememberedSlots.size());
    VLOG(REPORT,
         "[GCV2][remset-holder-policy] rootedRetainedKept=%zu retainedDeadDropped=%zu "
         "rootedStalePreserved=%zu currentRoots=%zu",
         rootedRetainedKept, retainedDeadDropped, rootedStalePreserved, currentMinorRoots.size());
    VLOG(REPORT, "[GCV2][findto-postlifecycle] soft=%zu",
         g_findtoPostLifecycleSoft.load(std::memory_order_relaxed));
    NwDropAudit::Report("rescan");
}
} // namespace MapleRuntime
