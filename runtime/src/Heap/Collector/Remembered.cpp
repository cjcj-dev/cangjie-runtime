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

// F3 true-dead arm (FixOldTaggedRefField soft-null). Always-on classified counters
// (f3arm / F3_KEEP_NO_NULL_DEAD_ARM §6 BEFORE_A). Default product still CAS-null.
// Categories partition the soft-null write — sum == total.
std::atomic<size_t> g_f3DeadarmTotal{ 0 };
std::atomic<size_t> g_f3DeadarmLatestNull{ 0 };
std::atomic<size_t> g_f3DeadarmRegionGarbage{ 0 };
std::atomic<size_t> g_f3DeadarmRegionNull{ 0 };
std::atomic<size_t> g_f3DeadarmRegionFree{ 0 };
std::atomic<size_t> g_f3DeadarmLatestNotHeap{ 0 };
std::atomic<size_t> g_f3DeadarmValidButNotLive{ 0 };
std::atomic<size_t> g_f3DeadarmInvalidObject{ 0 }; // active-region bad-tip early-return arm
std::atomic<size_t> g_f3DeadarmUnknown{ 0 };
// Orthogonal overlay (f3weak): dead-arm hits whose holder is IsWeakRef — not a
// partition class. Reason classes still sum to total; weak_holder ⊆ total.
std::atomic<size_t> g_f3DeadarmWeakHolder{ 0 };
std::atomic<bool> g_f3DeadarmAtexit{ false };

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

void ReportF3DeadarmCounts(const char* point)
{
    const size_t total = g_f3DeadarmTotal.load(std::memory_order_relaxed);
    const size_t latestNull = g_f3DeadarmLatestNull.load(std::memory_order_relaxed);
    const size_t regionGarbage = g_f3DeadarmRegionGarbage.load(std::memory_order_relaxed);
    const size_t regionNull = g_f3DeadarmRegionNull.load(std::memory_order_relaxed);
    const size_t regionFree = g_f3DeadarmRegionFree.load(std::memory_order_relaxed);
    const size_t latestNotHeap = g_f3DeadarmLatestNotHeap.load(std::memory_order_relaxed);
    const size_t validButNotLive = g_f3DeadarmValidButNotLive.load(std::memory_order_relaxed);
    const size_t invalidObject = g_f3DeadarmInvalidObject.load(std::memory_order_relaxed);
    const size_t unknown = g_f3DeadarmUnknown.load(std::memory_order_relaxed);
    const size_t weakHolder = g_f3DeadarmWeakHolder.load(std::memory_order_relaxed);
    // soft-null classes + invalid_object_active_region (bad-tip early-return) partition total.
    // weak_holder is orthogonal (holder type overlay), not part of classSum.
    const size_t softNullParts =
        latestNull + regionGarbage + regionNull + regionFree + latestNotHeap + validButNotLive + unknown;
    const size_t classSum = softNullParts + invalidObject;
    // fprintf+fflush: crash/assert paths must leave a greppable line even if VLOG is late.
    std::fprintf(stderr,
                 "[GCV2][f3-deadarm] point=%s total=%zu soft_null=%zu "
                 "latest_null=%zu region_garbage=%zu region_null=%zu region_free=%zu "
                 "region_null_or_free=%zu latest_not_heap=%zu valid_but_not_live=%zu "
                 "invalid_object_active_region=%zu unknown=%zu weak_holder=%zu "
                 "class_sum_ok=%d env_assert=%d\n",
                 point != nullptr ? point : "?", total, softNullParts, latestNull, regionGarbage, regionNull,
                 regionFree, regionNull + regionFree, latestNotHeap, validButNotLive, invalidObject, unknown,
                 weakHolder, classSum == total ? 1 : 0, 0);
    std::fflush(stderr);
    VLOG(REPORT,
         "[GCV2][f3-deadarm] point=%s total=%zu soft_null=%zu "
         "latest_null=%zu region_garbage=%zu region_null=%zu region_free=%zu "
         "region_null_or_free=%zu latest_not_heap=%zu valid_but_not_live=%zu "
         "invalid_object_active_region=%zu unknown=%zu weak_holder=%zu "
         "class_sum_ok=%d env_assert=%d",
         point != nullptr ? point : "?", total, softNullParts, latestNull, regionGarbage, regionNull, regionFree,
         regionNull + regionFree, latestNotHeap, validButNotLive, invalidObject, unknown, weakHolder,
         classSum == total ? 1 : 0, 0);
}

void EnsureF3DeadarmAtexit()
{
    bool expected = false;
    if (g_f3DeadarmAtexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { ReportF3DeadarmCounts("atexit"); });
    }
}

// Classify + count one hit on the !latestLive residue path. Returns reason for assert/log.
// invalid_object_active_region is counted on the bad-tip arm (no soft-null).
// holder: optional; when IsWeakRef, increments weak_holder overlay (f3weak).
const char* NoteF3DeadarmHit(const char* reason, BaseObject* holder)
{
    EnsureF3DeadarmAtexit();
    g_f3DeadarmTotal.fetch_add(1, std::memory_order_relaxed);
    if (holder != nullptr && holder->IsWeakRef()) {
        g_f3DeadarmWeakHolder.fetch_add(1, std::memory_order_relaxed);
    }
    if (reason == nullptr) {
        g_f3DeadarmUnknown.fetch_add(1, std::memory_order_relaxed);
        return "unknown";
    }
    if (std::strcmp(reason, "latest_null") == 0) {
        g_f3DeadarmLatestNull.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "region_garbage") == 0) {
        g_f3DeadarmRegionGarbage.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "region_null") == 0) {
        g_f3DeadarmRegionNull.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "region_free") == 0) {
        g_f3DeadarmRegionFree.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "latest_not_heap") == 0) {
        g_f3DeadarmLatestNotHeap.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "valid_but_not_live") == 0) {
        g_f3DeadarmValidButNotLive.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "invalid_object") == 0) {
        // Active-region bad tip: counted under invalid_object_active_region; not soft-null.
        g_f3DeadarmInvalidObject.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_f3DeadarmUnknown.fetch_add(1, std::memory_order_relaxed);
    }
    return reason;
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
void WCollector::FixOldTaggedRefField(BaseObject* holder, RefField<>& field, const ScopedStopTheWorld& stw)
{
    (void)holder;
    (void)stw;
    RefField<> observed(field);
    BaseObject* from = to_object(observed.GetTargetObject());
    if (from == nullptr) {
        return;
    }
    // ZGC remaps selected roots and live object fields; dead holders are not
    // consumers and therefore do not need a manufactured replacement value.
    if (!HolderObjectIsLive(holder)) {
        return;
    }
    if (!Heap::IsHeapAddress(from)) {
        RefField<> plain = RootSlotWriteback(from, field);
        if (observed.GetFieldValue() != plain.GetFieldValue()) {
            (void)HealSlot(field, observed.GetFieldValue(), plain.GetFieldValue(),
                           HealSite::WCollectorFixOldTaggedNonHeap);
        }
        return;
    }

    // zBarrier.inline.hpp:294-343: resolve through forwarding, completing
    // relocation on this thread when necessary, before self-healing the slot.
    // No route miss, invalid tip, or stale from-address is an installable value.
    const ForwardingProvenance provenance{ ForwardingHolderKind::Remset, holder, &field };
    BaseObject* resolved = ResolveStoreValue(from, provenance);
    CHECK_DETAIL(resolved != nullptr && Heap::IsHeapAddress(resolved),
                 "old-tag heal requires a resolved heap address from=%p", from);
    CHECK_DETAIL(Collector::JudgeHandOutTarget(resolved) == HandVerdict::Usable,
                 "old-tag heal requires a usable target from=%p resolved=%p", from, resolved);
    CHECK_DETAIL(!IsStaleStoreValue(resolved),
                 "old-tag heal must not install a relocation-set address from=%p resolved=%p",
                 from, resolved);

    RefField<> desired = ColourStoreGood(from_object(resolved));
    if (observed.GetFieldValue() != desired.GetFieldValue()) {
        (void)HealSlot(field, observed.GetFieldValue(), desired.GetFieldValue(),
                       HealSite::WCollectorFixOldTaggedLive);
    }
}

void WCollector::InvalidateOldTaggedRefsBeforeDispel()
{
    static const bool preflipWalk = []() {
        const char* value = std::getenv("MRT_GCV2_PREFLIP_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (!preflipWalk) {
        return;
    }
    InvalidateOldTaggedRefs(true);
}

void WCollector::InvalidateOldTaggedRefs(bool requireSurvivedMark)
{
    static const bool preflipVerify = []() {
        const char* value = std::getenv("MRT_GCV2_PREFLIP_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    const bool account = requireSurvivedMark && preflipVerify;
    const bool trackFixed = requireSurvivedMark && preflipVerify;
    MRT_PHASE_TIMER(requireSurvivedMark ? "InvalidateOldTaggedRefs.preflip" : "InvalidateOldTaggedRefs.postflip");
    ScopedStopTheWorld stw(requireSurvivedMark ? "invalidate old tagged refs before dispel"
                                               : "invalidate old tagged refs after flip");

    // CHUNK = 256 units × UNIT_SIZE (16MiB @ 64KB unit). Spec §六 T1.
    constexpr size_t kChunkUnits = 256;
    const size_t chunkBytes = kChunkUnits * RegionInfo::UNIT_SIZE;

    struct RootAccount {
        size_t rootSlots = 0;
        size_t oldTaggedRootSlots = 0;
        size_t fixedRootSlots = 0;
    };
    struct HeapAccount {
        size_t processedRegions = 0;
        size_t processedObjects = 0;
        size_t invalidObjects = 0;
        size_t filteredObjects = 0;
        size_t refHolders = 0;
        size_t fields = 0;
        size_t oldTaggedSlots = 0;
        size_t fixedSlots = 0;
        size_t youngTargetSlots = 0;
        size_t fromLiveObjects = 0;
        size_t fromLiveFields = 0;
        size_t rebuilt = 0;
        size_t chunksTaken = 0;
    };

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& regionManager = space.GetRegionManager();
    RememberedSet* rebuildRemset = requireSurvivedMark ? nullptr : &Heap::GetHeap().GetRememberedSet();
    const uintptr_t heapStart = regionManager.GetRegionHeapStart();
    const uintptr_t inactiveZone = regionManager.GetInactiveZone();

    auto makeRootVisitor = [this, trackFixed](RootAccount* acc) -> RootVisitor {
        return [this, trackFixed, acc](ObjectRef& root) {
#if defined(MRT_GC_UNIT_TESTS)
            NoteLargeArrayInitRootVisit(LargeArrayRootVisitSite::REMEMBERED,
                                        to_object(safe(root.LoadPlain(std::memory_order_acquire))));
#endif
            uintptr_t oldValue = raw(root.LoadPlain());
            HeapSlot<> observedBits(to_zpointer(oldValue));
            bool oldTagged = trackFixed && IsOldPointer(observedBits);
            if (trackFixed && acc != nullptr) {
                ++acc->rootSlots;
                if (oldTagged) {
                    ++acc->oldTaggedRootSlots;
                }
            }
            ForwardUpdateRawRef(root);
            if (trackFixed && acc != nullptr && oldTagged && raw(root.LoadPlain()) != oldValue) {
                ++acc->fixedRootSlots;
            }
        };
    };

    auto processObject = [this, requireSurvivedMark, rebuildRemset, account, trackFixed,
                          &stw](BaseObject* obj, HeapAccount& acc) {
        RegionInfo* accountRegion = nullptr;
        if (account) {
            ++acc.processedObjects;
            if (obj != nullptr) {
                accountRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
            }
            // H2: region count is per-worker local; each region is owned by exactly one
            // worker (region-head ownership), so summing processedRegions is exact.
        }
        if (obj == nullptr || !obj->IsValidObject()) {
            if (account) {
                ++acc.invalidObjects;
            }
            return;
        }
        if (requireSurvivedMark) {
            if (!IsSurvivedObject<Generation::Old>(obj)) {
                if (account) {
                    ++acc.filteredObjects;
                }
                return;
            }
            if (account && accountRegion != nullptr && accountRegion->IsFromRegion()) {
                ++acc.fromLiveObjects;
            }
        }
        if (!obj->HasRefField()) {
            return;
        }
        if (account) {
            ++acc.refHolders;
        }
        bool recordCrossGen = false;
        if (rebuildRemset != nullptr) {
            RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
            recordCrossGen = holderRegion != nullptr && !holderRegion->IsYoungRegion() &&
                             !holderRegion->IsGarbageRegion() && !holderRegion->IsFreeRegion();
        }
        bool forwardHolder = account && requireSurvivedMark && accountRegion != nullptr &&
                             accountRegion->IsFromRegion();
        obj->ForEachRefField([this, obj, recordCrossGen, rebuildRemset, forwardHolder, account, trackFixed,
                              &acc, &stw](RefField<>& field) {
            uintptr_t oldValue = raw(field.GetFieldValue());
            bool oldTagged = trackFixed && IsOldPointer(field);
            if (trackFixed) {
                ++acc.fields;
                if (forwardHolder) {
                    ++acc.fromLiveFields;
                }
                if (oldTagged) {
                    ++acc.oldTaggedSlots;
                }
            }
            FixOldTaggedRefField(obj, field, stw);
            if (oldTagged && raw(field.GetFieldValue()) != oldValue) {
                ++acc.fixedSlots;
            }
            if (!recordCrossGen) {
                return;
            }
            BaseObject* target = to_object(field.GetTargetObject());
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                if (account) {
                    ++acc.youngTargetSlots;
                }
                rebuildRemset->Record(reinterpret_cast<MAddress>(&field));
                ++acc.rebuilt;
            }
        });
    };

    // Region-head-ownership walk over [rangeStart, rangeEnd). H6: carry transient-extent
    // guard verbatim. Spec §六 T1: first-step correction if region head is before rangeStart.
    auto walkRange = [&processObject, requireSurvivedMark, account](uintptr_t rangeStart, uintptr_t rangeEnd,
                                                                    uintptr_t inactive, HeapAccount& acc) {
        if (rangeStart >= rangeEnd || rangeStart >= inactive) {
            return;
        }
        uintptr_t limit = std::min(rangeEnd, inactive);
        uintptr_t addr = rangeStart;
        // First-step correction: if the unit at rangeStart is mid-region, skip to that
        // region's end — the region belongs to the worker that owns its head.
        {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
            uintptr_t regionStart = region->GetRegionStart();
            uintptr_t nextAddr = region->GetRegionEnd();
            if (nextAddr <= addr || nextAddr > inactive) {
                // Transient illegal extent: step one unit, do not visit (H6).
                addr += RegionInfo::UNIT_SIZE;
            } else if (regionStart < rangeStart) {
                addr = nextAddr;
            }
        }
        while (addr < limit) {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
            uintptr_t nextAddr = region->GetRegionEnd();
            // H6 transient-extent guard — character-identical to ForEachObjUnsafe.
            if (nextAddr <= addr || nextAddr > inactive) {
                addr += RegionInfo::UNIT_SIZE;
                continue;
            }
            // Region-head ownership: only visit if the region's head is in this chunk.
            // Regions that spill past limit still belong entirely to this worker.
            if (addr >= rangeStart && addr < limit) {
                if (region->IsValidRegion() && !region->IsFreeRegion() && !region->IsGarbageRegion() &&
                    !(requireSurvivedMark &&
                      region->IsKnownEmpty(region->GetMarkView<Generation::Old>()))) {
                    region->VisitAllObjects([&processObject, &acc](BaseObject* object) {
                        processObject(object, acc);
                    });
                }
            }
            addr = nextAddr;
        }
    };

    auto heapWorkerBody = [&](std::atomic<uintptr_t>& cursor, HeapAccount& acc) {
        for (;;) {
            uintptr_t chunkStart = cursor.fetch_add(chunkBytes, std::memory_order_relaxed);
            if (chunkStart >= inactiveZone) {
                break;
            }
            ++acc.chunksTaken;
            uintptr_t chunkEnd = chunkStart + chunkBytes;
            walkRange(chunkStart, chunkEnd, inactiveZone, acc);
        }
    };

    GCThreadPool* threadPool = GetThreadPool();
    // Positive control for silent serial degradation (spec §六 T3 ②).
    // Force serial via MRT_GCV2_STWPAR_FORCE_SERIAL=1 for bidirectional proof.
    static const bool forceSerialEnv = []() {
        const char* value = std::getenv("MRT_GCV2_STWPAR_FORCE_SERIAL");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    const bool forceSerial = forceSerialEnv;
    const bool useParallel = threadPool != nullptr && !forceSerial;

    RootAccount rootTotals{};
    HeapAccount heapTotals{};
    std::vector<size_t> chunksPerWorker;
    size_t workersScheduled = 0;

    if (!useParallel) {
        // Keep the "fallback=serial pool_unavailable" literal for the case it actually names:
        // REMSET_OPTION1_SPEC_0805 §positive-control greps that exact string.
        VLOG(REPORT, "[F3][parallel] fallback=serial %s",
             threadPool == nullptr ? "pool_unavailable" : "force_serial");
        // Six root families serial (same order as before).
        {
            RootAccount acc;
            RootVisitor fixRoot = makeRootVisitor(&acc);
            MutatorManager::Instance().VisitAllMutators(
                [&fixRoot](Mutator& mutator) { mutator.VisitMutatorRoots(fixRoot); });
            Heap::GetHeap().VisitStaticRoots(fixRoot);
            Runtime::Current().GetConcurrencyModel().VisitGCRoots(&fixRoot);
            collectorResources.GetFinalizerProcessor().VisitGCRoots(fixRoot);
            collectorResources.GetFinalizerProcessor().VisitFinalizers(fixRoot);
            Heap::GetHeap().VisitAllExportRoots(fixRoot);
            rootTotals.rootSlots += acc.rootSlots;
            rootTotals.oldTaggedRootSlots += acc.oldTaggedRootSlots;
            rootTotals.fixedRootSlots += acc.fixedRootSlots;
        }
        {
            HeapAccount acc;
            // Single-threaded full range — equivalent to ForEachObjUnsafe.
            walkRange(heapStart, inactiveZone, inactiveZone, acc);
            // Count as one logical chunk for the diagnostic line.
            if (heapStart < inactiveZone) {
                acc.chunksTaken = 1;
            }
            heapTotals = acc;
            chunksPerWorker.push_back(acc.chunksTaken);
            workersScheduled = 1;
        }
    } else {
        // Root-side: 6 family-level tasks (static family must not be split — mutex+dedup set).
        // Heap-side: N cursor tasks. Same pool, same batch as Preforward.
        // Cap via MRT_GCV2_STWPAR_WORKERS for scale curve (1/2/4/8/16); never expand pool.
        const int32_t helperNum = threadPool->GetMaxThreadNum();
        // Caller's GC thread also drains via WaitFinish → effective capacity = helpers + 1.
        const int32_t poolCap = helperNum + 1;
        int32_t heapWorkers = poolCap;
        {
            const char* wEnv = std::getenv("MRT_GCV2_STWPAR_WORKERS");
            if (wEnv != nullptr && wEnv[0] != '\0') {
                int32_t want = static_cast<int32_t>(std::strtol(wEnv, nullptr, 10));
                if (want >= 1 && want < heapWorkers) {
                    heapWorkers = want;
                }
            }
        }
        std::vector<RootAccount> rootAcc(6);
        std::vector<HeapAccount> heapAcc(static_cast<size_t>(heapWorkers));
        std::atomic<uintptr_t> cursor{ heapStart };

        // Roots first into queue, then heap workers. Start after all AddWork so helpers
        // see the full batch (same shape as Preforward: AddWork×N then Start then WaitFinish).
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[0]);
            MutatorManager::Instance().VisitAllMutators(
                [&fixRoot](Mutator& mutator) { mutator.VisitMutatorRoots(fixRoot); });
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[1]);
            Heap::GetHeap().VisitStaticRoots(fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[2]);
            Runtime::Current().GetConcurrencyModel().VisitGCRoots(&fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[3]);
            collectorResources.GetFinalizerProcessor().VisitGCRoots(fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[4]);
            collectorResources.GetFinalizerProcessor().VisitFinalizers(fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[5]);
            Heap::GetHeap().VisitAllExportRoots(fixRoot);
        }));

        for (int32_t i = 0; i < heapWorkers; ++i) {
            HeapAccount* acc = &heapAcc[static_cast<size_t>(i)];
            threadPool->AddWork(new (std::nothrow) LambdaWork(
                [heapWorkerBody, &cursor, acc](size_t) { heapWorkerBody(cursor, *acc); }));
        }

        threadPool->Start();
        threadPool->WaitFinish();

        for (const auto& a : rootAcc) {
            rootTotals.rootSlots += a.rootSlots;
            rootTotals.oldTaggedRootSlots += a.oldTaggedRootSlots;
            rootTotals.fixedRootSlots += a.fixedRootSlots;
        }
        chunksPerWorker.reserve(heapAcc.size());
        for (const auto& a : heapAcc) {
            heapTotals.processedRegions += a.processedRegions;
            heapTotals.processedObjects += a.processedObjects;
            heapTotals.invalidObjects += a.invalidObjects;
            heapTotals.filteredObjects += a.filteredObjects;
            heapTotals.refHolders += a.refHolders;
            heapTotals.fields += a.fields;
            heapTotals.oldTaggedSlots += a.oldTaggedSlots;
            heapTotals.fixedSlots += a.fixedSlots;
            heapTotals.youngTargetSlots += a.youngTargetSlots;
            heapTotals.fromLiveObjects += a.fromLiveObjects;
            heapTotals.fromLiveFields += a.fromLiveFields;
            heapTotals.rebuilt += a.rebuilt;
            heapTotals.chunksTaken += a.chunksTaken;
            chunksPerWorker.push_back(a.chunksTaken);
        }
        workersScheduled = static_cast<size_t>(heapWorkers);
    }

    // Parallel-liveness positive control: at least 2 workers with chunks_taken>0 when heap
    // spans >2×CHUNK (spec §六 T3 ①). Always print so silence ≠ "never fired".
    {
        size_t active = 0;
        std::string chunksStr;
        for (size_t i = 0; i < chunksPerWorker.size(); ++i) {
            if (chunksPerWorker[i] != 0) {
                ++active;
            }
            if (i != 0) {
                chunksStr += ',';
            }
            chunksStr += std::to_string(chunksPerWorker[i]);
        }
        VLOG(REPORT, "[F3][parallel] phase=%s workers_active=%zu workers_scheduled=%zu chunks=[%s] parallel=%d",
             requireSurvivedMark ? "preflip" : "postflip", active, workersScheduled, chunksStr.c_str(),
             useParallel ? 1 : 0);
    }

    if (heapTotals.rebuilt != 0) {
        VLOG(REPORT, "[GCV2][remset] rebuilt after full GC recorded=%zu", heapTotals.rebuilt);
    }
    GarbRegionDiag::Report(requireSurvivedMark ? "preflip" : "postflip");
    if (requireSurvivedMark && preflipVerify) {
        static const bool preflipVerifyFatal = []() {
            const char* value = std::getenv("MRT_GCV2_PREFLIP_VERIFY_FATAL");
            return value != nullptr && std::strcmp(value, "1") == 0;
        }();
        const size_t fixedTotal = heapTotals.fixedSlots + rootTotals.fixedRootSlots;
        VLOG(REPORT,
             "[GCV2][preflip-verify] fixed=%zu fixedRoots=%zu fixedTotal=%zu oldTagged=%zu oldTaggedRoots=%zu "
             "fields=%zu rootSlots=%zu fatal=%d env=MRT_GCV2_PREFLIP_VERIFY=1",
             heapTotals.fixedSlots, rootTotals.fixedRootSlots, fixedTotal, heapTotals.oldTaggedSlots,
             rootTotals.oldTaggedRootSlots, heapTotals.fields, rootTotals.rootSlots,
             static_cast<int>(preflipVerifyFatal));
        if (fixedTotal > 0) {
            LOG(RTLOG_ERROR,
                "[GCV2][preflip-verify] PREFLIP_RESIDUE fixed=%zu fixedRoots=%zu fixedTotal=%zu "
                "oldTagged=%zu oldTaggedRoots=%zu (production skips preflip; residue means insurance needed)",
                heapTotals.fixedSlots, rootTotals.fixedRootSlots, fixedTotal, heapTotals.oldTaggedSlots,
                rootTotals.oldTaggedRootSlots);
            if (preflipVerifyFatal) {
                CHECK_DETAIL(false,
                             "MRT_GCV2_PREFLIP_VERIFY_FATAL: preflip residue fixedTotal=%zu "
                             "(fixed=%zu fixedRoots=%zu)",
                             fixedTotal, heapTotals.fixedSlots, rootTotals.fixedRootSlots);
            }
        }
    }
}
void WCollector::RescanRememberedSet(WorkStack& workStack, const MinorSlotSet& rememberedSlots,
                                     const MinorSlotSet& reachableSlots, const MinorSlotSet& weakSlots,
                                     const MinorObjectSet& currentMinorRoots, bool fullYoungScan,
                                     MinorSlotSet* consumedOut, RemsetScanStats* statsOut,
                                     MinorInteriorBaseMap* interiorBasesOut, const ScopedStopTheWorld* stw)
{
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
    auto plannedTo = [this, stw](BaseObject* from) -> BaseObject* {
        if (stw != nullptr) {
            return PlanRouteUnderStw(from, *stw).dest;
        }
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
    static std::atomic<size_t> g_remsetLifeClearLogged{ 0 };
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
    constexpr bool retainedProbe = false;
    // remsetlife: uncapped classification of the bad_target arm. The existing sample log
    // caps at 16 per process, so the 386-edge population was only ever seen through 84
    // samples. Counters answer "where does the target sit" for every dropped edge.
    // Default off; observation only, no control-flow change.
    constexpr bool remsetLifeProbe = false;
    size_t btNoRegion = 0;
    size_t btBeyondAlloc = 0;
    size_t btInAlloc = 0;
    size_t btWord0Zero = 0;
    size_t btNeverExamined = 0;
    size_t btOriginFound = 0;
    size_t btHolderInvalid = 0;
    size_t btTargetYoung = 0;
    size_t btRawTagged = 0;
    size_t btLoadBad = 0;
    size_t btOldPtr = 0;
    size_t btCurrentPtr = 0;
    size_t btClearHit = 0;
    size_t btRegionType[16] = { 0 };
    // On, unconditionally, because OpenJDK does it unconditionally: ZRemembered::scan_field
    // (zRemembered.cpp:578-589) re-arms every scanned slot whose healed target is still young, and
    // ZBarrier::remember (zBarrier.inline.hpp:729-733) conditions only on the slot being old.  The
    // pair is the whole design: record cheaply at the barrier without loading the target, prune at
    // scan time.  Drop either half and a long-lived old holder written once loses its record after
    // the first minor drains it.
    //
    // This was previously off behind an A/B switch whose stated reason was that a proposed
    // acceptance criterion -- "remembered= collapses across minors" -- did not reproduce.  That is a
    // side-effect criterion, not a correctness one: ZGC's argument never claims the count collapses,
    // and the count is free to grow when the old->young edge population genuinely grows.  Turning it
    // on is also the conservative direction; it can only add entries, and a superfluous remset entry
    // costs a scan while a missing one loses an object.
    constexpr bool remsetReRemember = true;
    size_t reRemembered = 0;
    size_t originFound = 0;
    size_t originBoundsValid = 0;
    size_t retainedNever = 0;
    size_t retainedValid = 0;
    size_t retainedEmpty = 0;
    size_t retainedStale = 0;
    size_t retainedKeep = 0;
    size_t retainedDrop = 0;
    size_t safeEmptyDrop = 0;
    size_t directDeadDrop = 0;
    size_t filterCorrect = 0;
    size_t filterIncorrect = 0;
    size_t filterOverKeep = 0;   // filter kept, closure says dead — floating garbage (safe)
    size_t filterOverDrop = 0;   // filter dropped, closure reached it — a live edge (unsafe)
    // holderlive (F2): never=100% has three candidate producers and the state word cannot
    // tell them apart. RegionInfo keeps a per-region-life history (RegionInfo.h:107-124);
    // read it here so the answer is a count, not a reading of the code.
    size_t neverNoPreserve = 0;      // Preserve* never ran on this region in its current life
    size_t neverPreserveSaidNever = 0; // Preserve* ran, had no live info, wrote NEVER itself
    size_t neverCleared = 0;         // Preserve* stored a snapshot, a clear path wiped it
    size_t neverLastOp[RegionInfo::RETAINED_OP_COUNT] = { 0 };
    size_t neverLiveInfoNow = 0;     // holder region has a live (current-cycle) LiveInfo now
    size_t neverHolderYoung = 0;
    size_t neverRegionType[16] = { 0 };
    // holderlive (F2) ③: bad_target population crossed with holder liveness (FYS oracle).
    size_t deadHolderTargetOk = 0;
    size_t deadHolderTargetBad = 0;
    size_t liveHolderTargetOk = 0;
    size_t liveHolderTargetBad = 0;
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
        if (retainedProbe || (retainedState != RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED &&
                             region->IsRetainedSnapshotValid())) {
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
                if (retainedProbe) {
                    ++originFound;
                    MAddress holderAddress = reinterpret_cast<MAddress>(holder);
                    size_t holderSize = RegionSpace::GetAllocSize(*holder);
                    if (slot >= holderAddress && slot < holderAddress + holderSize) {
                        ++originBoundsValid;
                    }
                }
                RegionInfo::RetainedLiveInfoState retainedState = holderRegion->GetRetainedLiveInfoState();
                if (retainedState == RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED) {
                    if (retainedProbe) {
                        ++retainedNever;
                        uint32_t preserveCnt = holderRegion->GetRetainedPreserveCount();
                        uint32_t clearCnt = holderRegion->GetRetainedClearCount();
                        if (preserveCnt == 0) {
                            ++neverNoPreserve;
                        } else if (clearCnt != 0) {
                            ++neverCleared;
                        } else {
                            ++neverPreserveSaidNever;
                        }
                        neverLastOp[holderRegion->GetRetainedLastOp() % RegionInfo::RETAINED_OP_COUNT]++;
                        if (holderRegion->GetLiveInfo() != nullptr) {
                            ++neverLiveInfoNow;
                        }
                        if (holderRegion->IsYoungRegion()) {
                            ++neverHolderYoung;
                        }
                        neverRegionType[static_cast<unsigned>(holderRegion->GetRegionType()) & 0xFU]++;
                    }
                } else if (!holderRegion->IsRetainedSnapshotValid()) {
                    if (retainedProbe) {
                        ++retainedStale;
                    }
                } else {
                    MAddress coveredUpTo = holderRegion->GetRetainedLiveInfoCoveredUpTo();
                    CHECK(coveredUpTo >= holderRegion->GetRegionStart() &&
                          coveredUpTo <= holderRegion->GetRegionAllocPtr());
                    MAddress holderAddress = reinterpret_cast<MAddress>(holder);
                    if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
                        if (retainedProbe) {
                            ++retainedEmpty;
                        }
                    } else {
                        if (retainedProbe) {
                            ++retainedValid;
                        }
                    }
                    if (holderAddress < coveredUpTo) {
                        if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
                            keepByRetainedSnapshot = false;
                            if (retainedProbe) {
                                ++safeEmptyDrop;
                            }
                        } else if (holderRegion->IsLargeRegion()) {
                            LiveInfo* retainedLiveInfo = holderRegion->GetRetainedLiveInfo();
                            MarkView<Generation::Old> retainedView =
                                holderRegion->GetMarkView<Generation::Old>();
                            keepByRetainedSnapshot = retainedLiveInfo != nullptr
                                ? holderRegion->IsSurvivedObject(retainedView, retainedLiveInfo, 0)
                                : holderRegion->IsSurvivedObject(retainedView, 0);
                            if (retainedProbe && !keepByRetainedSnapshot) {
                                ++directDeadDrop;
                            }
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
                            if (retainedProbe && !keepByRetainedSnapshot) {
                                ++directDeadDrop;
                            }
                        }
                    }
                }
            }
        }
        if (retainedProbe) {
            if (keepByRetainedSnapshot) {
                ++retainedKeep;
            } else {
                ++retainedDrop;
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
            if (remsetLifeProbe) {
                MAddress tAddr = reinterpret_cast<MAddress>(target);
                RegionInfo* tRegion = RegionInfo::TryGetRegionInfoAt(tAddr);
                if (tRegion == nullptr) {
                    ++btNoRegion;
                } else {
                    btRegionType[static_cast<unsigned>(tRegion->GetRegionType()) & 0xFU]++;
                    if (tRegion->IsYoungRegion()) {
                        ++btTargetYoung;
                    }
                    bool noDecisionFace = false;
                    if (tRegion->IsYoungRegion()) {
                        noDecisionFace = tRegion->GetMarkBitmap(
                            tRegion->GetMarkView<Generation::Young>()) == nullptr;
                    } else {
                        noDecisionFace = tRegion->GetMarkBitmap(
                            tRegion->GetMarkView<Generation::Old>()) == nullptr;
                    }
                    if (noDecisionFace &&
                        tRegion->GetRegionAllocPtr() > tRegion->GetRegionStart()) {
                        ++btNeverExamined;
                    }
                    // Is the address inside the region's allocated prefix at all? A target at or
                    // past allocPtr was never handed out by this region's current life.
                    if (tAddr >= tRegion->GetRegionAllocPtr()) {
                        ++btBeyondAlloc;
                    } else {
                        ++btInAlloc;
                    }
                }
                uint64_t word0 = 0;
                std::memcpy(&word0, target, sizeof(word0));
                if (word0 == 0) {
                    ++btWord0Zero;
                }
                if ((rawSlot & TAGGED_BITS_MASK) != 0) {
                    ++btRawTagged;
                }
                // Which read-path predicate saw this value? peek is the pre-resolve snapshot.
                if (IsLoadBad(peek)) {
                    ++btLoadBad;
                }
                if (IsOldPointer(peek)) {
                    ++btOldPtr;
                }
                if (IsCurrentPointer(peek)) {
                    ++btCurrentPtr;
                }
                auto btOrigin = rememberedOrigins.find(slot);
                if (btOrigin != rememberedOrigins.end() && btOrigin->second != nullptr) {
                    ++btOriginFound;
                    if (!btOrigin->second->IsValidObject()) {
                        ++btHolderInvalid;
                    }
                }
                // Was the target's memory zeroed under us by a region recycle
                // (TakeRegion → ClearUnits, RegionManager.cpp:1262) or a compact tail?
                // Reuses the existing gcfwdfix ring; needs MRT_GCV2_TRACE_CLEAR=1.
                if (TraceClear::Enabled()) {
                    char clearDetail[256];
                    if (TraceClear::Lookup(tAddr, clearDetail, sizeof(clearDetail))) {
                        ++btClearHit;
                        size_t c = g_remsetLifeClearLogged.fetch_add(1, std::memory_order_relaxed);
                        if (c < 8) {
                            VLOG(REPORT, "[GCV2][remsetlife][cleared] slot=%#zx target=%p detail=%s",
                                 static_cast<size_t>(slot), target, clearDetail);
                        }
                    }
                }
            }
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
        if (remsetReRemember) {
            RegionInfo* keepRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (keepRegion != nullptr && keepRegion->IsYoungRegion()) {
                Heap::GetHeap().GetRememberedSet().Record(slot);
                ++reRemembered;
            } else {
                noteRemsetOutcome(slot, 10, reinterpret_cast<MAddress>(target));
            }
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
    if (remsetLifeProbe && scrubbedBadTarget != 0) {
        VLOG(REPORT,
             "[GCV2][remsetlife] badTarget=%zu noRegion=%zu beyondAlloc=%zu inAlloc=%zu word0Zero=%zu "
             "neverExamined=%zu targetYoung=%zu rawTagged=%zu loadBad=%zu oldPtr=%zu currentPtr=%zu "
             "originFound=%zu holderInvalid=%zu clearHit=%zu "
             "rtype=[%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu] "
             "originsBuilt=%zu remembered=%zu env=MRT_GCV2_REMSETLIFE=1",
             scrubbedBadTarget, btNoRegion, btBeyondAlloc, btInAlloc, btWord0Zero, btNeverExamined, btTargetYoung,
             btRawTagged, btLoadBad, btOldPtr, btCurrentPtr, btOriginFound, btHolderInvalid, btClearHit,
             btRegionType[0], btRegionType[1], btRegionType[2],
             btRegionType[3], btRegionType[4], btRegionType[5], btRegionType[6], btRegionType[7], btRegionType[8],
             btRegionType[9], btRegionType[10], btRegionType[11], btRegionType[12], btRegionType[13],
             btRegionType[14], btRegionType[15], rememberedOrigins.size(), rememberedSlots.size());
    }
    if (retainedProbe) {
        VLOG(REPORT,
             "[RETLIVE][summary] slots=%zu originFound=%zu originBoundsValid=%zu never=%zu valid=%zu empty=%zu "
             "stale=%zu keep=%zu drop=%zu safeEmpty=%zu directDead=%zu oracleCorrect=%zu oracleIncorrect=%zu "
             "fullYoungScan=%u",
             rememberedSlots.size(), originFound, originBoundsValid, retainedNever, retainedValid, retainedEmpty,
             retainedStale, retainedKeep, retainedDrop, safeEmptyDrop, directDeadDrop, filterCorrect,
             filterIncorrect, static_cast<unsigned>(fullYoungScan));
        VLOG(REPORT,
             "[RETLIVE][why-never] never=%zu noPreserve=%zu preserveSaidNever=%zu cleared=%zu "
             "liveInfoNow=%zu holderYoung=%zu lastOp=[none=%zu,pVALID=%zu,pEMPTY=%zu,pNEVER=%zu,"
             "clrChecked=%zu,clrAll=%zu,clrRange=%zu]",
             retainedNever, neverNoPreserve, neverPreserveSaidNever, neverCleared, neverLiveInfoNow,
             neverHolderYoung, neverLastOp[RegionInfo::RETAINED_OP_NONE],
             neverLastOp[RegionInfo::RETAINED_OP_PRESERVE_VALID],
             neverLastOp[RegionInfo::RETAINED_OP_PRESERVE_EMPTY],
             neverLastOp[RegionInfo::RETAINED_OP_PRESERVE_NEVER],
             neverLastOp[RegionInfo::RETAINED_OP_CLEAR_CHECKED],
             neverLastOp[RegionInfo::RETAINED_OP_CLEAR_ALL],
             neverLastOp[RegionInfo::RETAINED_OP_CLEAR_RANGE]);
        VLOG(REPORT,
             "[RETLIVE][why-never-rtype] originRegions=%zu rtype=[%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,"
             "%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu]",
             originRegions.size(), neverRegionType[0], neverRegionType[1], neverRegionType[2],
             neverRegionType[3], neverRegionType[4], neverRegionType[5], neverRegionType[6],
             neverRegionType[7], neverRegionType[8], neverRegionType[9], neverRegionType[10],
             neverRegionType[11], neverRegionType[12], neverRegionType[13], neverRegionType[14],
             neverRegionType[15]);
        VLOG(REPORT,
             "[RETLIVE][deadalive] deadHolderTargetOk=%zu deadHolderTargetBad=%zu "
             "liveHolderTargetOk=%zu liveHolderTargetBad=%zu (FYS oracle; liveHolderTargetBad>0 "
             "= a live holder whose remset target does not resolve)",
             deadHolderTargetOk, deadHolderTargetBad, liveHolderTargetOk, liveHolderTargetBad);
        VLOG(REPORT,
             "[RETLIVE][verdict] overKeep=%zu overDrop=%zu correct=%zu (overDrop>0 = the filter "
             "would have dropped an edge the young closure reached)",
             filterOverKeep, filterOverDrop, filterCorrect);
    }
}
} // namespace MapleRuntime
