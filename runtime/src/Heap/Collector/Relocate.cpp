// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/WCollector/WCollector.h"
#include "Heap/WCollector/RememberedHolderPolicy.h"

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
#include "Heap/Verify/M0ExitDiagnostics.h"
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
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
struct CopierRouteMint {
    static CopierRouteToken Make() { return CopierRouteToken(); }
};
#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::atomic<uintptr_t> g_remapYoungRootsTargetSlot{ 0 };
std::atomic<uintptr_t> g_remapYoungRootsBefore{ 0 };
std::atomic<uintptr_t> g_remapYoungRootsAfter{ 0 };
std::atomic<uintptr_t> g_remapYoungRootsResolvedAddress{ 0 };
std::atomic<uint64_t> g_remapYoungRootsVisits{ 0 };
std::atomic<uint64_t> g_remapYoungRootsHeals{ 0 };
std::atomic<bool> g_remapYoungRootsStoreGoodAfter{ false };
} // namespace

void ResetRemapYoungRootsTestReceipt(uintptr_t targetSlot)
{
    g_remapYoungRootsTargetSlot.store(targetSlot, std::memory_order_relaxed);
    g_remapYoungRootsBefore.store(0, std::memory_order_relaxed);
    g_remapYoungRootsAfter.store(0, std::memory_order_relaxed);
    g_remapYoungRootsResolvedAddress.store(0, std::memory_order_relaxed);
    g_remapYoungRootsVisits.store(0, std::memory_order_relaxed);
    g_remapYoungRootsHeals.store(0, std::memory_order_relaxed);
    g_remapYoungRootsStoreGoodAfter.store(false, std::memory_order_relaxed);
}

RemapYoungRootsTestReceipt ReadRemapYoungRootsTestReceipt()
{
    return { g_remapYoungRootsTargetSlot.load(std::memory_order_relaxed),
             g_remapYoungRootsBefore.load(std::memory_order_relaxed),
             g_remapYoungRootsAfter.load(std::memory_order_relaxed),
             g_remapYoungRootsResolvedAddress.load(std::memory_order_relaxed),
             g_remapYoungRootsVisits.load(std::memory_order_relaxed),
             g_remapYoungRootsHeals.load(std::memory_order_relaxed),
             g_remapYoungRootsStoreGoodAfter.load(std::memory_order_relaxed) };
}

static void NoteRemapYoungRootsTestReceipt(RefField<>& field, uintptr_t before, bool healed,
                                           bool storeGoodAfter)
{
    const uintptr_t slot = reinterpret_cast<uintptr_t>(&field);
    if (slot != g_remapYoungRootsTargetSlot.load(std::memory_order_relaxed)) {
        return;
    }
    g_remapYoungRootsBefore.store(before, std::memory_order_relaxed);
    g_remapYoungRootsAfter.store(raw(field.GetFieldValue()), std::memory_order_relaxed);
    g_remapYoungRootsResolvedAddress.store(
        reinterpret_cast<uintptr_t>(to_object(field.GetTargetObject())), std::memory_order_relaxed);
    g_remapYoungRootsVisits.fetch_add(1, std::memory_order_relaxed);
    if (healed) {
        g_remapYoungRootsHeals.fetch_add(1, std::memory_order_relaxed);
    }
    g_remapYoungRootsStoreGoodAfter.store(storeGoodAfter, std::memory_order_relaxed);
}
#endif
#if defined(MRT_GC_UNIT_TESTS)
static thread_local WCollector::RouteLookupTestResult* g_routeLookupTestContext = nullptr;
#endif
#if defined(MRT_TESTABLE_INTERNALS)
// Scheduling only: install a barrier after flip, at wait entry, and before
// post-copy cleanup. The callback never supplies a forwarding answer.
using RemapWindowTestHook = void (*)(unsigned, RegionInfo*, BaseObject*);
static std::atomic<RemapWindowTestHook> g_remapWindowTestHook{ nullptr };
extern "C" MRT_EXPORT void MRT_SetRemapWindowTestHook(RemapWindowTestHook hook)
{
    g_remapWindowTestHook.store(hook, std::memory_order_release);
}
void RunRemapWindowTestHook(unsigned point, RegionInfo* region, BaseObject* object)
{
    auto hook = g_remapWindowTestHook.load(std::memory_order_acquire);
    if (hook != nullptr) {
        hook(point, region, object);
    }
}
#endif
namespace WCollectorInternal {
} // namespace WCollectorInternal
// installdomain: positive control — how often Resolve/Fix would install a ghost-from that is
// outside GetRoute's liveInfo0 survivor domain. Grant paints that bit before route geometry.
// Report with MRT_GCV2_INSTALLDOMAIN_ACCOUNT=1 (also always VLOG once per minor if >0).
std::atomic<size_t> g_installDomainGrant{ 0 };
std::atomic<size_t> g_installDomainAlready{ 0 };
std::atomic<size_t> g_installDomainTooLate{ 0 };
std::atomic<size_t> g_installDomainSkip{ 0 };
bool WCollector::IsUnmovableFromObject(BaseObject* obj) const
{
    // filter const string object.
    if (!Heap::IsHeapAddress(obj)) {
        return false;
    }

    // zRelocate.cpp:385-390: a lookup miss after the membership probe is a legal
    // concurrent outcome (ghost dispel), so re-resolve from authoritative state
    // instead of using a pointer that this race can leave null. GetRegionInfoAt
    // itself CHECKs (early-stop) on a genuine no-owner invariant break.
    RegionInfo* regionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<uintptr_t>(obj));
    if (regionInfo == nullptr) {
        regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    }
    return regionInfo->IsUnmovableFromRegion();
}

void WCollector::CheckStoreGoodTarget(const char* consumer, BaseObject* target,
                                      const ForwardingProvenance& provenance) const
{
    const HandVerdict verdict = Collector::JudgeHandOutTarget(target);
    const bool stale = IsStaleStoreValue(target);
    if (verdict == HandVerdict::Usable && !stale) {
        return;
    }
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(
        reinterpret_cast<MAddress>(target));
    CHECK_DETAIL(false,
                 "%s consumer=%s target=%p holder_kind=%s holder=%p slot=%p stage=%s "
                 "writer_kind=%s incoming_source_kind=%s source_slot=%p working_copy_slot=%p "
                 "field_type=%s field_offset=%zu table_id=%#zx publication_generation=%llu "
                 "from_page_epoch=%llu lifeId=%llu lookup_state=%u lookup_cause=%u "
                 "publication_closed=%u",
                 verdict != HandVerdict::Usable
                     ? "store-good requires a usable resolved address"
                     : "store-good must not colour a relocation-set address",
                 consumer == nullptr ? "unknown" : consumer, target,
                 ForwardingProvenance::KindName(provenance.kind), provenance.holder, provenance.slot,
                 ForwardingProvenance::StageName(provenance.stage),
                 ForwardingProvenance::WriterName(provenance.writerKind),
                 ForwardingProvenance::SourceName(provenance.incomingSourceKind), provenance.sourceSlot,
                 provenance.workingCopySlot, ForwardingProvenance::FieldName(provenance.fieldKind),
                 provenance.fieldOffset, static_cast<size_t>(lookup.tableId),
                 static_cast<unsigned long long>(lookup.publicationGeneration),
                 static_cast<unsigned long long>(lookup.fromPageEpoch),
                 static_cast<unsigned long long>(lookup.fromPageLifeId),
                 static_cast<unsigned>(lookup.answer), static_cast<unsigned>(lookup.unavailableCause),
                 lookup.publicationClosed ? 1u : 0u);
}

template<bool forward>
bool WCollector::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                       BaseObject*& toObj, const ForwardingProvenance& provenance) const
{
    RefField<> oldRef(field);
    if (IsLoadBad(oldRef)) {
        fromObj = to_object(oldRef.GetTargetObject());
        if (forward) {
            toObj = const_cast<WCollector*>(this)->TryForwardObject(fromObj);
        } else {
            toObj = FindToVersion(fromObj).GetOrFailClosed(
                "WCollector::TryUpdateRefFieldImpl", provenance);
        }
        if (toObj == nullptr) {
            return false;
        }
        // R7：写回必须经规范色单产地，禁 plain RefField<>(toObj)。
        // expected 仍是 observed-raw（oldRef.GetFieldValue()）；模板 = GetAndTryTagRefField。
        RefField<> tmpField = GetAndTryTagRefField(toObj);
        if (HealSlot(field, oldRef.GetFieldValue(), tmpField.GetFieldValue(),
                     HealSite::WCollectorTryUpdateRefField)) {
            if (obj != nullptr) {
                DLOG(TRACE, "update obj %p<%p>(%zu)+%zu ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(),
                     obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field, raw(oldRef.GetFieldValue()),
                     raw(tmpField.GetFieldValue()));
            } else {
                DLOG(TRACE, "update ref@%p: 0x%zx -> %p", &field, raw(oldRef.GetFieldValue()), toObj);
            }
            return true;
        } else {
            if (obj != nullptr) {
                DLOG(TRACE,
                     "update obj %p<%p>(%zu)+%zu but cas failed ref-field@%p: %#zx(%#zx) -> %#zx but cas failed ", obj,
                     obj->GetTypeInfo(), obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field,
                     raw(oldRef.GetFieldValue()), raw(field.GetFieldValue()), raw(tmpField.GetFieldValue()));
            } else {
                DLOG(TRACE, "update but cas failed ref@%p: 0x%zx(%zx) -> %p", &field, raw(oldRef.GetFieldValue()),
                     field.GetFieldValue(), toObj);
            }
            return true;
        }
    }

    return false;
}
bool WCollector::TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, obj, &field };
    return TryUpdateRefFieldImpl<false>(obj, field, oldRef, newRef, provenance);
}

bool WCollector::TryUpdateRefFieldWithProvenance(BaseObject* obj, RefField<>& field, BaseObject*& newRef,
                                                  const ForwardingProvenance& provenance) const
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<false>(obj, field, oldRef, newRef, provenance);
}

bool WCollector::TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, obj, &field };
    return TryUpdateRefFieldImpl<true>(obj, field, oldRef, newRef, provenance);
}
// this api untags current pointer as well as old pointer, caller should take care of this.
bool WCollector::TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const
{
    for (;;) {
        RefField<> oldRef(field);
        if (!IsLoadBad(oldRef)) {
            return false;
        }
        target = to_object(oldRef.GetTargetObject());
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
        untagRefFieldBreadcrumb.active = 0;
        untagRefFieldBreadcrumb.holder = obj;
        untagRefFieldBreadcrumb.field = &field;
        untagRefFieldBreadcrumb.target = target;
        untagRefFieldBreadcrumb.caller = __builtin_return_address(0);
        untagRefFieldBreadcrumb.fieldOffset =
            obj == nullptr ? static_cast<size_t>(-1) : BaseObject::FieldOffset(obj, &field);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        untagRefFieldBreadcrumb.active = 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
        const bool isValidTarget = target->IsValidObject();
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
        if (LIKELY(isValidTarget)) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            untagRefFieldBreadcrumb.active = 0;
        }
#endif
        // Anchor main 2f1bc8355e92dbf01c063050b5c9a2947c711d64
        CHECK_DETAIL(isValidTarget, "TryUntagRefField encounters invalid tagged target %p at field %p", target,
                     &field);
        // TRUST_STATE_KILL_PLAN Phase 1: API retained, but HeapSlot write-back is current colour
        // (not plain). Read path no longer calls this; residual callers must not re-install trust.
        RefField<> newRef = GetAndTryTagRefField(target);
        if (HealSlot(field, oldRef.GetFieldValue(), newRef.GetFieldValue(),
                     HealSite::WCollectorTryUntagRefField)) {
            if (obj != nullptr) {
                DLOG(FIX, "untag obj %p<%p>(%zu) ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(), obj->GetSize(),
                     &field, raw(oldRef.GetFieldValue()), raw(newRef.GetFieldValue()));
            } else {
                DLOG(FIX, "untag ref@%p: %#zx -> %#zx", &field, raw(oldRef.GetFieldValue()), raw(newRef.GetFieldValue()));
            }
            return true;
        }
    }

    return false;
}

BaseObject* WCollector::ForwardUpdateRawRef(ObjectRef& root)
{
    zaddress_unsafe observed = root.LoadPlain();
    HeapSlot<> observedBits(to_zpointer(raw(observed)));
    BaseObject* oldObj = to_object(observedBits.GetTargetObject());
    DLOG(FIX, "visit raw-ref @%p: %p", &root, oldObj);
    // Static / RO slots (e.g. .data.rel.ro under GNU_RELRO) hold non-heap objects that
    // are never evacuated. Keep their existing plain value and skip write-back.
    // Same heap gate as IsGhostFromObject / FindToVersion / FixMinorEvacuatedSlot resolve.
    if (oldObj == nullptr || !Heap::IsHeapAddress(oldObj)) {
        return oldObj;
    }
    // arrayinit2 / markfloor Q2 / introot: stackmap may label RawArray+8 (&length) as a root.
    // Colouring that interior makes the mutator load a non-canonical address (si_code=128).
    // Relocate via host object; write plain interior (toHost+offset) back.
    if (!Collector::PlausibleManagedObjectGate("ForwardUpdateRawRef", oldObj)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(oldObj);
        RegionInfo* hostRegion = host == nullptr ? nullptr :
            RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(host));
        const bool hostHasForwardingFace = hostRegion != nullptr && hostRegion->GetLiveInfo0ForProbe() != nullptr;
        if (hostHasForwardingFace && IsGhostFromObject(host) && !IsUnmovableFromObject(host)) {
            BaseObject* toHost = TryForwardObject(host);
            if (toHost == nullptr) {
                Collector::FailClosedLoad(
                    "WCollector::ForwardUpdateRawRef.interior-unresolved", host,
                    reinterpret_cast<uintptr_t>(&root),
                    ForwardingProvenance{ ForwardingHolderKind::StackSlot, this, &root });
            }
            BaseObject* toInterior = reinterpret_cast<BaseObject*>(
                reinterpret_cast<uintptr_t>(toHost) +
                (reinterpret_cast<uintptr_t>(oldObj) - reinterpret_cast<uintptr_t>(host)));
            HealRootWriteback(root, toInterior, HealSite::WCollectorForwardRawInterior);
            return toInterior;
        }
        HealRootWriteback(root, oldObj, HealSite::WCollectorPreserveRawInterior);
        return oldObj;
    }
    if (IsGhostFromObject(oldObj)) {
        const MAddress mappedAddr = ForwardingTable::FindTo(reinterpret_cast<MAddress>(oldObj));
        if (mappedAddr != 0) {
            BaseObject* mapped = reinterpret_cast<BaseObject*>(mappedAddr);
            HealRootWriteback(root, mapped, HealSite::WCollectorForwardRawGhost);
            DLOG(FIX, "fix raw-ref @%p: %p -> %p", &root, oldObj, mapped);
            return mapped;
        }
        const GCPhase phase = GetGCPhase();
        if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
            Collector::FailClosedLoad(
                "WCollector::ForwardUpdateRawRef.unresolved", oldObj,
                reinterpret_cast<uintptr_t>(&root),
                ForwardingProvenance{ ForwardingHolderKind::StackSlot, this, &root });
        }
        BaseObject* toVersion = TryForwardObject(oldObj);
        if (toVersion == nullptr) {
            Collector::FailClosedLoad(
                "WCollector::ForwardUpdateRawRef.unresolved", oldObj,
                reinterpret_cast<uintptr_t>(&root),
                ForwardingProvenance{ ForwardingHolderKind::StackSlot, this, &root });
        }
        HealRootWriteback(root, toVersion, HealSite::WCollectorForwardRawGhost);
        DLOG(FIX, "fix raw-ref @%p: %p -> %p", &root, oldObj, toVersion);
        return toVersion;
    } else {
        HealRootWriteback(root, oldObj, HealSite::WCollectorNormalizeRawRoot);
    }

    return oldObj;
}
void WCollector::PreforwardAllExportFromRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    Heap::GetHeap().VisitAllExportRoots(visitor);
}
void WCollector::PreforwardStaticRoots()
{
    RootSlotVisitor visitor = [this](RootSlot& root) { ForwardUpdateRawRef(root); };
    Heap::GetHeap().VisitStaticRoots(visitor);
}

void WCollector::RemapYoungRoots()
{
    if (!RemapYoungRootsLogic::kEnableRemapYoungRoots) {
        return;
    }
    MRT_PHASE_TIMER("RemapYoungRoots");
    const uintptr_t youngMask = ZPointerRemappedYoungMask;
    const uintptr_t oldMask = ZPointerRemappedOldMask;
    size_t remsetSeen = 0;
    size_t remsetColoured = 0;
    size_t remsetRemapped = 0;
    size_t remsetDoubleBad = 0;
    size_t staticSeen = 0;
    size_t staticColoured = 0;
    size_t staticRemapped = 0;
    size_t staticDoubleBad = 0;
    size_t stackSeen = 0;
    size_t stackColoured = 0;
    size_t stackRemapped = 0;
    size_t otherSeen = 0;
    size_t otherColoured = 0;
    size_t otherRemapped = 0;
    size_t otherDoubleBad = 0;

    auto remapField = [&](RefField<>& field, size_t& seen, size_t& coloured, size_t& remapped,
                          size_t& doubleBad) {
        ++seen;
        RefField<> oldField(field);
        const uintptr_t rawVal = raw(oldField.GetFieldValue());
        const auto kind = RemapYoungRootsLogic::Classify(rawVal, youngMask, oldMask);
        if (!RemapYoungRootsLogic::NeedsForwardingLookup(kind)) {
            return;
        }
        ++coloured;
        if (kind == RemapYoungRootsLogic::Kind::DoubleBad) {
            ++doubleBad;
        }
        BaseObject* observed = to_object(oldField.GetTargetObject());
        if (!Heap::IsHeapAddress(observed)) {
            return;
        }
        // ZRemapOopClosure calls load_barrier_on_oop_field for every coloured
        // root (zGeneration.cpp:1408-1418,1483-1499).  Do the address-level
        // forwarding lookup even when the four-value colour has wrapped back
        // to load-good; only an absent forwarding may preserve `observed`.
        const ForwardingProvenance provenance{ ForwardingHolderKind::Remset, nullptr, &field };
        BaseObject* latest = ResolveStoreValue(observed, provenance);
        if (!Heap::IsHeapAddress(latest)) {
            return;
        }
        if (!Collector::PlausibleManagedObjectGate("RemapYoungRoots", latest)) {
            return;
        }
        // ZGC resolves a field word exactly once: ZBarrier::barrier runs one make_load_good
        // (zBarrier.inline.hpp:294-343) and then rebuilds the word with ZPointer::uncolor /
        // ZAddress::store_good, which never consult a forwarding table
        // (zAddress.inline.hpp:609-624,806-811).  GetAndTryTagRefField resolves a second time,
        // and under in-place compaction that second resolve is not idempotent: from- and
        // to-addresses share one page span, so an address this page already produced is itself a
        // from-index of the same table.  `latest` above is already the make-load-good answer, so
        // colour it -- ColourResolvedRefField keeps all three checks and drops only the repeat
        // lookup, the same shape already used by the mark closure (Mark.cpp:361).
        RefField<> newField = ColourResolvedRefField(latest, provenance);
        bool healed = false;
        if (oldField.GetFieldValue() != newField.GetFieldValue()) {
            healed = HealSlot(field, oldField.GetFieldValue(), newField.GetFieldValue(),
                              HealSite::WCollectorRemapYoungRoots);
            if (healed) {
                ++remapped;
            }
        }
#if defined(MRT_TESTABLE_INTERNALS)
        NoteRemapYoungRootsTestReceipt(field, rawVal, healed, is_store_good(field));
#endif
    };

    std::unordered_set<MAddress> remset = Heap::GetHeap().GetRememberedSet().Snapshot();
    for (MAddress slot : remset) {
        if (slot == 0) {
            continue;
        }
        // ZRemsetTableIterator admits the current old page, then remap_current applies the
        // load barrier to every bit in that page's current remembered face; it does not ask
        // whether the containing object is live (zRemembered.cpp:395-458). Our remembered
        // bitmap is heap-wide instead of page-local, so reproduce only that page admission.
        // ClearRegion removes both faces before reuse; reject slots whose old carrier has
        // already left the page table before dereferencing the recorded address.
        RegionInfo* holderPage = RegionInfo::TryGetRegionInfoAt(slot);
        if (holderPage == nullptr || !holderPage->IsValidRegion() || holderPage->IsFreeRegion() ||
            holderPage->IsGarbageRegion() || holderPage->IsYoungRegion()) {
            continue;
        }
        // slotwitness: the fail-closed records reached from here print tag/edge/host/slot and the
        // slot owner's region facts, and this walk was the only remap producer that set none of
        // them -- every permhole witness from RemapYoungRoots reads `tag=none slot=0`, so the
        // record cannot say whether the refused value came out of a live holder or out of a
        // from-copy this cycle already compacted.  ZGC never has to ask: remap_current iterates
        // per *page* out of the page table and a page's remembered bitmap dies with the page
        // (zGeneration.cpp:1483-1499; zRemembered.cpp:451-461).  Ours is a flat address set, so
        // the holder has to be named on the record.
        remapField(*reinterpret_cast<RefField<>*>(slot), remsetSeen, remsetColoured, remsetRemapped,
                   remsetDoubleBad);
    }

    auto remapRoot = [this, youngMask, oldMask](ObjectRef& root, size_t& seen, size_t& coloured,
                                                size_t& remapped, size_t& doubleBad) {
        ++seen;
        const uintptr_t rawVal = raw(root.LoadPlain());
        const auto kind = RemapYoungRootsLogic::Classify(rawVal, youngMask, oldMask);
        if (kind != RemapYoungRootsLogic::Kind::Uncoloured) {
            ++coloured;
        }
        if (kind == RemapYoungRootsLogic::Kind::DoubleBad) {
            ++doubleBad;
        }
        ForwardUpdateRawRef(root);
        if (raw(root.LoadPlain()) != rawVal) {
            ++remapped;
        }
    };

    RootSlotVisitor staticVisitor = [&](RootSlot& root) {
        remapRoot(root, staticSeen, staticColoured, staticRemapped, staticDoubleBad);
    };
    Heap::GetHeap().VisitStaticRoots(staticVisitor);

    MutatorManager::Instance().VisitAllMutators([&](Mutator& mutator) {
        RootVisitor visitor = [&](ObjectRef& root) {
            size_t doubleBad = 0;
            remapRoot(root, stackSeen, stackColoured, stackRemapped, doubleBad);
        };
        DerivedPtrVisitor derivedVisitor = [this](BasePtrType basePtr, DerivedSlot& derived) {
            BaseObject* knownBase = is_null(basePtr) ? nullptr :
                to_object(safe(uncolor_bits(to_zpointer(raw(basePtr)))));
            (void)FixMinorEvacuatedSlot(derived, knownBase, nullptr);
        };
        mutator.VisitHeapReferences(visitor, derivedVisitor);
    });

    RootVisitor otherVisitor = [&](ObjectRef& root) {
        remapRoot(root, otherSeen, otherColoured, otherRemapped, otherDoubleBad);
    };
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&otherVisitor);
    collectorResources.GetFinalizerProcessor().VisitRawPointers(otherVisitor);
    Heap::GetHeap().VisitAllExportRoots(otherVisitor);

    // ZGenerationOld::remap_young_roots completes colored roots, uncolored
    // roots, and the current remembered set before old relocate-start
    // (zGeneration.cpp:1458-1523). Only that coverage completion retires the
    // forwarding authority; a cycle count is not a lifetime proof.
    ForwardingTable::ReclaimRetired("old-remap-young-roots-complete");
    // A8 triggers reclaim; it does not publish mark coverage.

    LOG(RTLOG_ERROR,
        "[A8REMAP] remset seen=%zu coloured=%zu remapped=%zu doubleBad=%zu "
        "static seen=%zu coloured=%zu remapped=%zu doubleBad=%zu "
        "stack seen=%zu coloured=%zu remapped=%zu "
        "other seen=%zu coloured=%zu remapped=%zu doubleBad=%zu flipSeq=%lu",
        remsetSeen, remsetColoured, remsetRemapped, remsetDoubleBad, staticSeen, staticColoured,
        staticRemapped, staticDoubleBad, stackSeen, stackColoured, stackRemapped,
        otherSeen, otherColoured, otherRemapped, otherDoubleBad,
        static_cast<unsigned long>(FlipSeq().load(std::memory_order_relaxed)));
    // Table destroy ⇔ remap coverage finished (zRelocationSet.cpp:191-200).
    ForwardingTable::ReclaimRetired("post-remap-reset-relocation-set");
}
void WCollector::PreforwardFinalizerProcessorRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    collectorResources.GetFinalizerProcessor().VisitRawPointers(visitor);
}

void WCollector::PreforwardConcurrencyModelRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitor);
}

void WCollector::PreforwardDiscoveredExternObjects()
{
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CHECK(discoveredExternObjects.empty());
    CurrentizeValueRootMap(cycleRefWorkStack);
}

void WCollector::PreforwardAllResurrectExportFromObjects()
{
    std::lock_guard<std::mutex> lg(resurrectExportMtx);
    CurrentizeValueRootSet(resurrectedExportObjectes);
    CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase);
}
void WCollector::StartRelocationTasks()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    GCWorkers& workers = GetWorkers();
#if defined(MRT_TESTABLE_INTERNALS)
    if (GetCycleReason() == GC_REASON_YOUNG) RunRemapWindowTestHook(1, nullptr, nullptr);
#endif
    if (GetCycleReason() == GC_REASON_YOUNG) manager.StartForwardFromRegions<Generation::Young>(workers);
    else manager.StartForwardFromRegions<Generation::Old>(workers);
}

void WCollector::Preforward()
{
    ScopedEntryTrace trace("CJRT_GC_PREFORWARD");
    MRT_PHASE_TIMER("Preforward");
    {
        // OpenJDK zGeneration.cpp:1175-1200: isolate pause_relocate_start from the
        // concurrent root-preforward work below. ScopedLightSync emits its matching
        // rec=stw record, including rendezvous and held time.
        ScopedLightSync scopedLightSync("Preforward", true, GCPhase::GC_PHASE_PREFORWARD);
        // ZStat samples pause/concurrent kind when the timer is constructed, so enter
        // ScopedLightSync first. Destruction order also closes this timer before mutators
        // resume, keeping the whole phase in the pause account.
        MRT_PHASE_TIMER("old.relocate_start");
        RegionInfo::AdvanceCompactRouteTableGracePeriod();
        // fwdgrace: this sync does not go through TransitionToGCPhase, so the arena grace
        // period has to be advanced alongside the route-table one or the two drift apart.
        ForwardDataManager::AdvanceGracePeriod();
        // OpenJDK zGeneration.cpp:1054-1063: Phase 8 remaps young roots under the driver
        // lock *before* pause_relocate_start flips the old remap bits (zGeneration.cpp:1503-1508).
        RemapYoungRoots();
        // This collector relocates both generations in one full-GC relocation set. Match the two
        // generation relocate-start flips while mutators are stopped, before any root is forwarded.
        CsetEmptyWho::ClassifyCycle();
        flip_young_relocate_start();
        flip_old_relocate_start();
        StartRelocationTasks();
    }

    RegionManager& manager = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    if (GetCycleReason() == GC_REASON_YOUNG) {
        manager.DrainForwardFromRegions<Generation::Young>();
    } else {
        manager.DrainForwardFromRegions<Generation::Old>();
    }
    GCWorkers& workers = GetWorkers();
    std::atomic<unsigned> next{ 0 };
    class RootsTask final : public GCWorkerTask {
    public:
        RootsTask(WCollector& collector, std::atomic<unsigned>& cursor) : collector(collector), next(cursor) {}
        void Work(uint32_t) override
        {
            for (unsigned family = next.fetch_add(1); family < 6; family = next.fetch_add(1)) {
                switch (family) {
                    case 0:
                        collector.PreforwardConcurrencyModelRoots();
                        break;
                    case 1:
                        collector.PreforwardFinalizerProcessorRoots();
                        break;
                    case 2:
                        collector.PreforwardAllExportFromRoots();
                        break;
                    case 3:
                        collector.PreforwardStaticRoots();
                        break;
                    case 4:
                        collector.PreforwardDiscoveredExternObjects();
                        break;
                    default:
                        collector.PreforwardAllResurrectExportFromObjects();
                        break;
                }
            }
        }
    private:
        WCollector& collector;
        std::atomic<unsigned>& next;
    } roots(static_cast<WCollector&>(*this), next);
    workers.Run(roots);
    if (HealCoverage::kHealCoverageCensus) {
        HealCoverage::CensusAfterPublication(
            currentRemapColour, FlipSeq().load(std::memory_order_relaxed), "major-preforward");
    }
}

// N2 (MINOR_CONCURRENCY_0805 §八 T-C): CAS-install resolved target under multi-worker fix.
// Same-value concurrent writes converge; first writer wins. Counters for positive control.
namespace {
std::atomic<size_t> g_minorRefCasFail{ 0 };
std::atomic<size_t> g_minorRefCasOk{ 0 };

// installdomain (ZGC mark_and_remember shape, GC-thread side): before installing a
// from/ghost-from address into a heap slot (or forwarding it), ensure the survivor
// bit that GetRoute will read is set.
//
// Two windows:
//   (1) pass1 before PrepareForwardable: region is still from (not yet ghost). Mark
//       current liveInfo; PrepareForwardable does liveInfo0 = liveInfo (pointer copy)
//       so the paint is snapshotted into the route domain.
//   (2) after PrepareForwardable while routeState==FORWARDABLE: MarkObject writes the
//       same LiveInfo that liveInfo0 points at — visible to GetRoute, not wiped by
//       ClearLiveInfo (that already ran at PrepareYoung).
// After ROUTED, liveByteCount/geometry are frozen — do not paint (tooLate counter).
void EnsureRouteDomainMembership(WCollector* collector, BaseObject* obj)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!Collector::PlausibleManagedObjectGate("EnsureRouteDomain", obj)) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!obj->IsValidObject()) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (collector->IsUnmovableFromObject(obj)) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool isGhost = collector->IsGhostFromObject(obj);
    const bool isFrom = collector->IsFromObject(obj);
    if (!isGhost && !isFrom) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(obj));
    // Prefer ghost face when present (what GetRoute reads); else current liveInfo.
    LiveInfo* face = region->GetLiveInfo0ForProbe();
    if (face == nullptr) {
        face = region->GetLiveInfo();
    }
    bool alreadyInDomain = false;
    if (isGhost) {
        alreadyInDomain = region->IsRouteSurvivedObject(offset);
    } else if (region->IsYoungRegion()) {
        MarkView<Generation::Young> view = region->GetMarkView<Generation::Young>();
        alreadyInDomain = region->IsSurvivedObject(view, face, offset);
    } else {
        // oracleblack round 10, face b: the nested young cycle can promote this region
        // before its discharge walk resolves a slot into it. A promoted region is an old
        // region now; binding the young view trips GetMarkView's sole-constructor CHECK
        // (RegionInfo.h:211). Consult the old face for the same survivorship question.
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        alreadyInDomain = region->IsSurvivedObject(view, face, offset);
    }
    if (alreadyInDomain) {
        g_installDomainAlready.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (isGhost) {
        // Only paint while FORWARDABLE: RouteOrCompactRegionImpl freezes liveByteCount.
        RegionInfo::RouteState rs = region->GetRouteState();
        if (rs != RegionInfo::RouteState::FORWARDABLE) {
            g_installDomainTooLate.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // Mark current liveInfo (post-snapshot: same pointer as liveInfo0 when non-null).
    (void)collector->MarkObject(obj);
    // If ghost face was null (snapshot of empty liveInfo), bind freshly allocated liveInfo
    // so GetRoute's liveInfo0!=null gate opens on the bits we just painted.
    if (isGhost) {
        region->BindLiveInfo0FromLiveIfNull();
    }
    LiveInfo* live = region->GetLiveInfo();
    LiveInfo* ghost = region->GetLiveInfo0ForProbe();
    RegionBitmap* ghostBitmap = ghost == nullptr ? nullptr : region->GetOwnerMarkBitmap(ghost);
    if (ghost != nullptr && ghost != live && ghostBitmap != nullptr) {
        size_t objSize = 0;
        if (Collector::PlausibleManagedObjectGate("EnsureRouteDomain.size", obj)) {
            objSize = obj->GetSize();
        }
        MAddress regionStart = region->GetRegionStart();
        size_t regionSize = static_cast<size_t>(region->GetRegionEnd() - regionStart);
        if (objSize > 0 && offset + objSize <= regionSize) {

            // MarkObject already maintained liveByteCount on the live face; ghost paint is
            // domain-visible bits only (do not double-count).
            (void)ghostBitmap->MarkBits(offset, objSize, regionSize);
        }
    }
    // Re-check: grant only counts if GetRoute face now accepts (positive control truth).
    ghost = region->GetLiveInfo0ForProbe();
    if (isGhost) {
        if (ghost != nullptr && region->IsRouteSurvivedObject(offset)) {
            g_installDomainGrant.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_installDomainTooLate.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // pre-snapshot from: paint lands on liveInfo; PrepareForwardable will copy pointer.
        g_installDomainGrant.fetch_add(1, std::memory_order_relaxed);
    }
}

// statresid: force ghost liveInfo0 paint while still FORWARDABLE (before any Route
// freezes geometry). Used by the root grant pass and as last-chance before Forward.
// Returns true when AdmitForRoute would accept `obj` after the paint attempt.
bool ForceRootRouteDomainWhileForwardable(WCollector* collector, BaseObject* obj)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return false;
    }
    if (!Collector::PlausibleManagedObjectGate("statresid.force_domain", obj)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(obj);
        if (host == nullptr || host == obj) {
            return false;
        }
        obj = host;
        if (!Collector::PlausibleManagedObjectGate("statresid.force_domain.host", obj)) {
            return false;
        }
    }
    EnsureRouteDomainMembership(collector, obj);
    RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
        region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    }
    if (region == nullptr || !region->IsYoungRegion()) {
        return false;
    }
    // Only paint while FORWARDABLE — after ROUTING/ROUTED/COMPACTED liveByteCount is
    // frozen (S2); late MarkBits would desync Admit from geometry.
    if (region->GetRouteState() != RegionInfo::RouteState::FORWARDABLE) {
        LiveInfo* g0 = region->GetLiveInfo0ForProbe();
        size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(obj));
        return g0 != nullptr && region->IsRouteSurvivedObject(offset);
    }
    (void)collector->MarkObject(obj);
    region->BindLiveInfo0FromLiveIfNull();
    LiveInfo* g0 = region->GetLiveInfo0ForProbe();
    size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(obj));
    RegionBitmap* ghostBitmap = g0 == nullptr ? nullptr : region->GetOwnerMarkBitmap(g0);
    if (g0 != nullptr && ghostBitmap != nullptr) {
        if (!region->IsRouteSurvivedObject(offset)) {
            size_t objSize = 0;
            if (Collector::PlausibleManagedObjectGate("statresid.force_domain.size", obj)) {
                objSize = obj->GetSize();
            }
            size_t regionSize = static_cast<size_t>(region->GetRegionEnd() - region->GetRegionStart());
            if (objSize > 0 && offset + objSize <= regionSize) {

                // MarkObject above already counted liveByteCount when first paint on live.
                // Ghost-only MarkBits must not double-count (FYS0 OverflowException risk).
                (void)ghostBitmap->MarkBits(offset, objSize, regionSize);
            }
        }
    }
    g0 = region->GetLiveInfo0ForProbe();
    return g0 != nullptr && region->IsRouteSurvivedObject(offset);
}
} // namespace

// Install a logical resolved target into a heap field. Callers cannot supply a
// pre-encoded RefField: this controlled entry applies the current heap colour here.
// On CAS fail, accept the peer's update (major TryUpdateRefFieldImpl shape).
bool WCollector::CasInstallResolvedTarget(RefField<>& field, MAddress expected, zaddress target,
                                          HealSite site, HealNull allowNull) const
{
    BaseObject* object = to_object(target);
    if (object != nullptr) {
        CHECK_DETAIL(Heap::IsHeapAddress(object),
                     "resolved heal target must be a heap address target=%p", object);
        CHECK_DETAIL(Collector::JudgeHandOutTarget(object) == HandVerdict::Usable,
                     "resolved heal target must be usable target=%p", object);
        CHECK_DETAIL(!IsStaleStoreValue(object),
                     "resolved heal target must not remain in forwarding domain target=%p", object);
    }
    zpointer desired = is_null(target) ? zpointer::null : ColourStoreGood(target).GetFieldValue();
    if (expected == raw(desired)) {
        return true;
    }
    if (HealSlot(field, to_zpointer(expected), desired, site, allowNull)) {
        g_minorRefCasOk.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_minorRefCasFail.fetch_add(1, std::memory_order_relaxed);
    return true;
}

BaseObject* WCollector::ResolveMinorReference(RefField<>& field, const ScopedStopTheWorld* stw,
                                              bool holderIsCurrentMinorRoot,
                                              bool* preservedByCurrentRoot) const
{
    (void)stw;
    (void)holderIsCurrentMinorRoot;
    (void)preservedByCurrentRoot;

    RefField<> observed(field);
    BaseObject* from = to_object(observed.GetTargetObject());
    if (from == nullptr || !Heap::IsHeapAddress(from)) {
        return from;
    }

    // zBarrier.inline.hpp:294-343: both old-colour and apparently current
    // references pass through make-load-good before the concrete slot is
    // healed. Colour alone is not forwarding provenance.
    const ForwardingProvenance provenance{ ForwardingHolderKind::Remset, nullptr, &field };
    BaseObject* resolved = ResolveStoreValue(from, provenance);
    CHECK_DETAIL(resolved != nullptr && Heap::IsHeapAddress(resolved),
                 "minor resolve requires a heap to-address from=%p", from);
    CHECK_DETAIL(Collector::JudgeHandOutTarget(resolved) == HandVerdict::Usable,
                 "minor resolve requires a usable target from=%p resolved=%p", from, resolved);
    CHECK_DETAIL(!IsStaleStoreValue(resolved),
                 "minor resolve must not install a relocation-set address from=%p resolved=%p",
                 from, resolved);

    const HealSite site = IsOldPointer(observed)
        ? HealSite::WCollectorMinorResolveOldForward
        : HealSite::WCollectorMinorResolveLoadGoodForward;
    (void)CasInstallResolvedTarget(field, raw(observed.GetFieldValue()), from_object(resolved), site);
    return resolved;
}
BaseObject* WCollector::ResolveMinorReference(RootSlot& root, const ScopedStopTheWorld* stw) const
{
    (void)stw;
    zaddress_unsafe observed = root.LoadPlain();
    HeapSlot<> observedBits(to_zpointer(raw(observed)));
    BaseObject* from = to_object(observedBits.GetTargetObject());
    if (from == nullptr || !Heap::IsHeapAddress(from)) {
        return from;
    }

    // ZUncoloredRootProcessOopClosure applies the load barrier and writes the
    // resolved address back uncolored (zGeneration.cpp:1458-1523).
    const ForwardingProvenance provenance{ ForwardingHolderKind::StackSlot, this, &root };
    BaseObject* resolved = ResolveStoreValue(from, provenance);
    CHECK_DETAIL(resolved != nullptr && Heap::IsHeapAddress(resolved),
                 "minor root resolve requires a heap to-address from=%p", from);
    CHECK_DETAIL(Collector::JudgeHandOutTarget(resolved) == HandVerdict::Usable,
                 "minor root resolve requires a usable target from=%p resolved=%p", from, resolved);
    CHECK_DETAIL(!IsStaleStoreValue(resolved),
                 "minor root resolve must not install a relocation-set address from=%p resolved=%p",
                 from, resolved);

    const HealSite site = IsOldPointer(observedBits)
        ? HealSite::WCollectorResolveRootOldForward
        : HealSite::WCollectorResolveRootLoadGoodForward;
    HealRootWriteback(root, resolved, site);
    return resolved;
}
bool WCollector::FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase,
                                      const ScopedStopTheWorld* stw,
                                      bool holderIsCurrentMinorRoot) const
{
    // N1: major-style CAS tolerate (TryUpdateRefFieldImpl family). Under multi-worker
    // fix, CAS fail is normal (peer already updated) — abort assertion was serial-only.
    RefField<> oldField(field);
    BaseObject* observedTarget = to_object(oldField.GetTargetObject());
    // A derived value is not an oop and must never enter the ordinary
    // forwarding lookup. Resolve the proven base first, then reconstruct the
    // same offset from the load-good base (ZGC derived-root/base-pointer
    // ordering; zBarrier.inline.hpp:294-343).
    if (knownBase != nullptr && observedTarget != nullptr &&
        Heap::IsHeapAddress(observedTarget) && Heap::IsHeapAddress(knownBase)) {
        const MAddress targetAddress = reinterpret_cast<MAddress>(observedTarget);
        const MAddress baseAddress = reinterpret_cast<MAddress>(knownBase);
        RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(targetAddress);
        RegionInfo* baseRegion = RegionInfo::TryGetRegionInfoAt(baseAddress);
        const bool baseValid = targetAddress > baseAddress && targetRegion == baseRegion &&
            Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot.knownBase", knownBase);
        if (!baseValid) {
            return false;
        }
        const size_t offset = static_cast<size_t>(targetAddress - baseAddress);
        if (offset >= RegionSpace::GetAllocSize(*knownBase)) {
            return false;
        }
        const ForwardingProvenance provenance{ ForwardingHolderKind::Derived, knownBase, &field };
        BaseObject* resolvedBase = ResolveStoreValue(knownBase, provenance);
        CHECK_DETAIL(resolvedBase != nullptr && Heap::IsHeapAddress(resolvedBase) &&
                         Collector::JudgeHandOutTarget(resolvedBase) == HandVerdict::Usable,
                     "derived heal requires a resolved base base=%p resolved=%p offset=%zu",
                     knownBase, resolvedBase, offset);
        const MAddress oldVal = raw(oldField.GetFieldValue());
        const MAddress interiorAddress = reinterpret_cast<MAddress>(resolvedBase) + offset;
        if (oldVal != interiorAddress) {
            (void)CasInstallInteriorColoured(field, to_zpointer(oldVal), resolvedBase, offset,
                                             HealSite::WCollectorMinorFixInteriorForward);
        }
        return true;
    }
    BaseObject* target = ResolveMinorReference(field, stw, holderIsCurrentMinorRoot);
    // Static / RO slots may hold non-heap objects (never evacuated). Colouring them
    // changes the bit pattern so equal-skip misses, then CAS faults on RELRO.
    // Same heap gate as ForwardUpdateRawRef / FindToVersion.
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    // h3seed2/3 乙 residual: live holder field still points at a region that minor
    // already CollectRegion'd (ClearUnits). Prefer silent null over UAF; CAS so
    // concurrent fix peers can win. Pre-evac H3 samples the prior cycle's residue —
    // nulling here clears it before the next VERIFY_HEAP inventory.
    // Criterion: RegionInfo::IsFreeRegion|IsGarbageRegion at this Fix call (file:line).
    if (ScrubMinorFreeTarget(field, target, true, holderIsCurrentMinorRoot)) {
        return true;
    }
    // interiorsrc2 / introot: value may be RawArray+8 (derived interior). Relocate via host;
    // keep the interior payload. Storage is HeapSlot (fields/remset), so publish
    // it with StoreGood; stackmap DerivedSlot remains plain.
    // ScopedPlainWriter tags DerivedLegal column, not K1 HeapSlot plain.
    if (knownBase != nullptr) {
        MAddress targetAddress = reinterpret_cast<MAddress>(target);
        MAddress baseAddress = reinterpret_cast<MAddress>(knownBase);
        size_t offset = targetAddress > baseAddress ? targetAddress - baseAddress : 0;
        RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(targetAddress);
        RegionInfo* baseRegion = RegionInfo::TryGetRegionInfoAt(baseAddress);
        bool allowedOffset = offset == 8u || offset == 16u || offset == 24u || offset == 32u;
        bool verifiedBase = allowedOffset && targetRegion == baseRegion &&
            Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot.knownBase", knownBase) &&
            offset < RegionSpace::GetAllocSize(*knownBase);
        if (!verifiedBase) {
            return false;
        }
    }
    if (knownBase != nullptr || !Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot", target)) {
        BaseObject* host = knownBase != nullptr ? knownBase : Collector::TryRecoverInteriorBase(target);
        RegionInfo* hostRegion = host == nullptr ? nullptr :
            RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(host));
        const bool hostHasForwardingFace = knownBase != nullptr ||
            (hostRegion != nullptr && hostRegion->GetLiveInfo0ForProbe() != nullptr);
        if (hostHasForwardingFace && host != nullptr && IsGhostFromObject(host) &&
            !IsUnmovableFromObject(host)) {
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), host);
            BaseObject* toHost = const_cast<WCollector*>(this)->ForwardObject(host);
            if (toHost == nullptr) {
                Collector::FailClosedLoad(
                    "WCollector::FixMinorEvacuatedSlot.field-interior-unresolved", host,
                    reinterpret_cast<uintptr_t>(&field),
                    ForwardingProvenance{ ForwardingHolderKind::Derived, knownBase, &field });
            }
            size_t offset = static_cast<size_t>(reinterpret_cast<uintptr_t>(target) -
                                                reinterpret_cast<uintptr_t>(host));
            MAddress oldVal = raw(oldField.GetFieldValue());
            MAddress interiorAddress = reinterpret_cast<MAddress>(toHost) + offset;
            if (oldVal != interiorAddress) {
                (void)CasInstallInteriorColoured(field, to_zpointer(oldVal), toHost, offset,
                                                 HealSite::WCollectorMinorFixInteriorForward);
            }
            return true;
        }
        // Gate rejected; host unknown or not forwarded — preserve the interior
        // address, but the heap carrier is still fully coloured.
        MAddress oldVal = raw(oldField.GetFieldValue());
        MAddress interiorAddress = reinterpret_cast<MAddress>(target);
        if (oldVal != interiorAddress) {
            (void)CasInstallInteriorColoured(field, to_zpointer(oldVal), target,
                                             HealSite::WCollectorMinorFixInteriorPreserve);
        }
        return false;
    }
    HeapSlot<> oldBits(oldField);
    BaseObject* oldObj = to_object(oldBits.GetTargetObject());
    // resolveto: Resolve already rewrote FROM→TO. TO sits in a Compacted ghost
    // (in-place pack). Forward/Admit indexes liveInfo0 by from-offset — feeding TO
    // misses → leave-alone. Keep the already-installed to.
    RegionInfo* targetRegion = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(target));
    const bool compactDestination = targetRegion != nullptr &&
        targetRegion->IsCompactRouteDestination(reinterpret_cast<MAddress>(target));
    const bool alreadyTo = (target != oldObj) || compactDestination;
    BaseObject* current = target;
    const bool hasForwardingFace = targetRegion != nullptr && targetRegion->GetLiveInfo0ForProbe() != nullptr;
    if (!alreadyTo && hasForwardingFace && IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
        // installdomain: route-domain grant before ForwardObject → GetRoute.
        EnsureRouteDomainMembership(const_cast<WCollector*>(this), target);
        current = const_cast<WCollector*>(this)->ForwardObject(target);
    }
    // ForwardObject null = movable ghost with no to-version (survivor-gate miss).
    // Drop the edge; do not reinstall the from address that is about to be reclaimed.
    if (current == nullptr) {
        // zBarrier.inline.hpp:294-343 never publishes a null substitute for a
        // non-null reference whose forwarding lookup missed. The current thread
        // must finish relocation (or fail closed); HealSlot's null arm is not a
        // substitute for an unresolved product.
        (void)HealSlot(field, field.GetFieldValue(), zpointer::null,
                       HealSite::WCollectorMinorFixForwardNull, HealNull::Disallow);
        Collector::FailClosedLoad(
            "WCollector::FixMinorEvacuatedSlot.unresolved", target,
            static_cast<uintptr_t>(raw(field.GetFieldValue())),
            ForwardingProvenance{ ForwardingHolderKind::Remset, nullptr, &field });
    }
    // ForwardObject may return the same interior if gated; re-check before colouring.
    if (!Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot.postfwd", current)) {
        MAddress oldVal = raw(field.GetFieldValue());
        MAddress interiorAddress = reinterpret_cast<MAddress>(current);
        if (oldVal != interiorAddress) {
            (void)CasInstallInteriorColoured(field, to_zpointer(oldVal), current,
                                             HealSite::WCollectorMinorFixInteriorPostForward);
        }
        return false;
    }
    // plainroots: stack/reg root slots → plain current; heap remset/fields → Phase C colour.
    // Plain on heap was the trust-state install that AssertColouredWriteIfEnabled fires on.
    RefField<> newField = RootSlotWriteback(current, field);
    MAddress oldVal = raw(oldField.GetFieldValue());
    MAddress newVal = raw(newField.GetFieldValue());
    if (oldVal == newVal) {
        return false;
    }
    // Re-read after resolve (resolve may have CAS-installed plain already).
    oldVal = raw(field.GetFieldValue());
    if (oldVal == newVal) {
        return false;
    }
    if (HealSlot(field, to_zpointer(oldVal), to_zpointer(newVal),
                 HealSite::WCollectorMinorFixForwarded)) {
        g_minorRefCasOk.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // CAS fail: accept if current == desired or already a plain/newer install (major style).
    g_minorRefCasFail.fetch_add(1, std::memory_order_relaxed);
    MAddress cur = raw(field.GetFieldValue());
    if (cur == newVal) {
        return true;
    }
    // Peer may have installed same logical target via ResolveMinorReference first
    // (old tagged → plain) then another worker forwarded; either is a valid fix.
    return true;
}

bool WCollector::FixMinorEvacuatedSlot(RootSlot& root, const ScopedStopTheWorld* stw) const
{
    MAddress oldValue = raw(root.LoadPlain());
    HeapSlot<> observedBits(to_zpointer(oldValue));
    BaseObject* observed = to_object(observedBits.GetTargetObject());
    if (observed != nullptr && Heap::IsHeapAddress(observed) &&
        !Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot", observed)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(observed);
        if (host != nullptr && IsGhostFromObject(host) && !IsUnmovableFromObject(host)) {
            // A bare ForwardingTable::FindTo asks for a receipt that only exists once the
            // host has actually been relocated.  While its page is still FORWARDABLE no
            // receipt has been written yet, so the miss says nothing about the host -- and
            // measured, that is the miss this branch was reporting: route=1 compacted=0
            // young=1 marked=1 survived=1 isStart=1, a live object whose page had not been
            // routed.  ZGC has one rule here and the barrier is the *producer*:
            // ZRelocate::relocate_object looks the address up and, on a miss, performs the
            // relocation itself (zRelocate.cpp:382-416).  The base branch below already
            // spells that out (ForceRootRouteDomainWhileForwardable + ForwardObject, retried
            // once, then the versioned table).  Resolve the interior host through the same
            // producer so the two halves of one mechanism cannot disagree about when a
            // from-address is resolvable.
            BaseObject* toHost = nullptr;
            RegionInfo* hostRegion = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(host));
            if (hostRegion != nullptr && hostRegion->GetLiveInfo0ForProbe() != nullptr) {
                (void)ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), host);
                toHost = const_cast<WCollector*>(this)->ForwardObject(host);
                if (toHost == nullptr &&
                    ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), host)) {
                    toHost = const_cast<WCollector*>(this)->ForwardObject(host);
                }
            }
            if (toHost == nullptr) {
                const ForwardingProvenance provenance{
                    ForwardingHolderKind::StackSlot, this, &root
                };
                BaseObject* viaTable = FindToVersion(host).GetOrFailClosed(
                    "WCollector::FixMinorEvacuatedSlot.interior", provenance);
                if (viaTable != nullptr && Heap::IsHeapAddress(viaTable) && viaTable->IsValidObject()) {
                    toHost = viaTable;
                }
            }
            if (toHost == nullptr) {
                Collector::FailClosedLoad(
                    "WCollector::FixMinorEvacuatedSlot.interior-unresolved", host,
                    reinterpret_cast<uintptr_t>(&root),
                    ForwardingProvenance{ ForwardingHolderKind::StackSlot, this, &root });
            }
            BaseObject* toInterior = reinterpret_cast<BaseObject*>(
                reinterpret_cast<uintptr_t>(toHost) +
                (reinterpret_cast<uintptr_t>(observed) - reinterpret_cast<uintptr_t>(host)));
            HealRootWriteback(root, toInterior, HealSite::WCollectorFixRootInteriorForward);
            return toInterior != observed;
        }
        HealRootWriteback(root, observed, HealSite::WCollectorPreserveRootInterior);
        return false;
    }
    BaseObject* target = ResolveMinorReference(root, stw);
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    HeapSlot<> oldBits(to_zpointer(oldValue));
    BaseObject* oldObj = to_object(oldBits.GetTargetObject());
    // resolveto: Resolve already remapped FROM→TO. Do not Admit the to-address
    // against the from-offset bitmap (offpast same-target probe: sameObj=0).
    RegionInfo* targetRegion = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(target));
    const bool compactDestination = targetRegion != nullptr &&
        targetRegion->IsCompactRouteDestination(reinterpret_cast<MAddress>(target));
    const bool alreadyTo = (target != oldObj) || compactDestination;
    BaseObject* current = target;
    const bool hasForwardingFace = targetRegion != nullptr && targetRegion->GetLiveInfo0ForProbe() != nullptr;
    if (!alreadyTo && hasForwardingFace && IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
        // Last-chance domain paint while FORWARDABLE (grant pass covers the bulk case;
        // this catches roots dirtied after the grant pass or parallel races).
        (void)ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), target);
        current = const_cast<WCollector*>(this)->ForwardObject(target);
        // Third disposition (statresid): if still null and region still FORWARDABLE,
        // force-paint once more and retry Forward — never HealRoot(null), never leave
        // a reclaimable from named by a live root without a second attempt.
        if (current == nullptr) {
            if (ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), target)) {
                current = const_cast<WCollector*>(this)->ForwardObject(target);
            }
        }
    }
    if (current == nullptr) {

        // I2: Forward miss still consults FindToVersion/receipt. Stale miss
        // refuses silently leaving from (seqnum-bounded table already rejects
        // expired entries). ⛔ Do not reinstall from; ⛔ do not StorePlain(null).
        const ForwardingProvenance provenance{ ForwardingHolderKind::StackSlot, this, &root };
        BaseObject* viaTable = FindToVersion(target).GetOrFailClosed(
            "WCollector::FixMinorEvacuatedSlot", provenance);
        if (viaTable != nullptr && viaTable != target && Heap::IsHeapAddress(viaTable) &&
            viaTable->IsValidObject()) {
            HealRootWriteback(root, viaTable, HealSite::WCollectorFixRootForwarded);
            return true;
        }
        Collector::FailClosedLoad(
            "WCollector::FixMinorEvacuatedSlot.unresolved", target,
            reinterpret_cast<uintptr_t>(&root),
            ForwardingProvenance{ ForwardingHolderKind::StackSlot, this, &root });
    }
    if (!Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot.postfwd", current)) {
        HealRootWriteback(root, current, HealSite::WCollectorFixRootPostForwardInterior);
        return false;
    }
    MAddress newValue = reinterpret_cast<MAddress>(current);
    if (oldValue == newValue && raw(root.LoadPlain()) == newValue) {
        return false;
    }
    HealRootWriteback(root, current, HealSite::WCollectorFixRootForwarded);
    return true;
}

bool WCollector::FixMinorEvacuatedSlot(DerivedSlot& derived, BaseObject* knownBase,
                                      const ScopedStopTheWorld* stw) const
{
    (void)stw;

    zaddress_unsafe observed = derived.LoadDerived();
    if (knownBase == nullptr || is_null(observed)) {
        return false;
    }
    BaseObject* derivedObject = to_object(safe(uncolor_bits(to_zpointer(raw(observed)))));
    MAddress baseAddress = reinterpret_cast<MAddress>(knownBase);
    MAddress derivedAddress = reinterpret_cast<MAddress>(derivedObject);
    if (derivedObject == nullptr || derivedAddress < baseAddress) {
        return false;
    }
    size_t offset = derivedAddress - baseAddress;

    // fixenum: the mark visitor turns a derived pair into a temporary base root. The
    // fix visitor must instead preserve the pair's offset and rewrite its real slot,
    // exactly as GCPhasePreForward does after forwarding the host.
    BaseObject* currentBase = knownBase;
    RegionInfo* baseRegion = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(knownBase));
    if (Heap::IsHeapAddress(knownBase) &&
        Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot.derivedBase", knownBase) &&
        baseRegion != nullptr && baseRegion->GetLiveInfo0ForProbe() != nullptr &&
        IsGhostFromObject(knownBase) && !IsUnmovableFromObject(knownBase)) {
        (void)ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), knownBase);
        currentBase = const_cast<WCollector*>(this)->ForwardObject(knownBase);
        if (currentBase == nullptr) {
            Collector::FailClosedLoad(
                "WCollector::FixMinorEvacuatedSlot.derived-base-unresolved", knownBase,
                reinterpret_cast<uintptr_t>(&derived),
                ForwardingProvenance{ ForwardingHolderKind::Derived, knownBase, &derived });
        }
    }

    RootSlot fixedBase;
    HealRootWriteback(fixedBase, currentBase, HealSite::WCollectorFixRootForwarded);
    RebaseDerived(derived, fixedBase, offset);
    return raw(observed) != raw(derived.LoadDerived());
}

void WCollector::FixMinorRootSlots(const ScopedStopTheWorld* stw)
{
    size_t callN = g_fixMinorRootSlotsCalls.fetch_add(1, std::memory_order_relaxed);
    const size_t entryBefore = g_resolveRootEntry.load(std::memory_order_relaxed);
    const size_t oldBefore = g_resolveRootOld.load(std::memory_order_relaxed);
    const size_t nullBefore = g_resolveRootHealNull.load(std::memory_order_relaxed);
    if (NullslotProbeEnabled() && callN < 32) {
        GCPhase phase = Heap::GetHeap().GetGCPhase();
        std::fprintf(stderr,
                     "[GCV2][nullslot] path=fix_minor_roots begin n=%zu phase=%s(%u) "
                     "entry=%zu old=%zu healNull=%zu\n",
                     callN, Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase), entryBefore, oldBefore,
                     nullBefore);
        std::fflush(stderr);
    }
    // statresid grant-before-route: paint every root-named young ghost into liveInfo0
    // *before* any Forward/Route. Per-slot Ensure+Forward (old shape) Routes the whole
    // region on the first root of a shared region, freezes liveByteCount (COMPACTED/ROUTED),
    // then later roots in the same region hit admit_miss + leave-alone → reclaim UAF
    // (GetSize si_addr=0xffff…). Two-pass: grant all, then fix.
    RootVisitor grantVisitor = [this](ObjectRef& root) {
        zaddress_unsafe observed = root.LoadPlain();
        if (is_null(observed)) {
            return;
        }
        HeapSlot<> bits(to_zpointer(raw(observed)));
        BaseObject* obj = to_object(bits.GetTargetObject());
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
            return;
        }
        if (IsGhostFromObject(obj) && !IsUnmovableFromObject(obj)) {
            (void)ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), obj);

        } else if (!Collector::PlausibleManagedObjectGate("statresid.grant_pass", obj)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(obj);
            if (host != nullptr && IsGhostFromObject(host) && !IsUnmovableFromObject(host)) {
                (void)ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), host);
            }
        }
    };
    MutatorManager::Instance().VisitAllMutators(
        [&grantVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(grantVisitor); });
    Heap::GetHeap().VisitStaticRoots(grantVisitor);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&grantVisitor);
    collectorResources.GetFinalizerProcessor().VisitRawPointers(grantVisitor);
    Heap::GetHeap().VisitAllExportRoots(grantVisitor);

    RootVisitor rawRootVisitor = [this, stw](ObjectRef& root) {
#if defined(MRT_GC_UNIT_TESTS)
        NoteLargeArrayInitRootVisit(LargeArrayRootVisitSite::MINOR_RELOCATE,
                                    to_object(safe(root.LoadPlain(std::memory_order_acquire))));
#endif
        (void)FixMinorEvacuatedSlot(root, stw);
    };
    DerivedPtrVisitor derivedVisitor = [this, stw](BasePtrType basePtr, DerivedSlot& derived) {
        BaseObject* knownBase = is_null(basePtr) ? nullptr :
            to_object(safe(uncolor_bits(to_zpointer(raw(basePtr)))));
        (void)FixMinorEvacuatedSlot(derived, knownBase, stw);
    };
    // fixenum: VisitMutatorRoots exposes derived pairs only as temporary base roots and
    // deliberately leaves their real slots for PreForward. Minor fix needs the typed
    // HeapReferenceMap callback so its root set matches mark without losing writeback.
    MutatorManager::Instance().VisitAllMutators(
        [&rawRootVisitor, &derivedVisitor](Mutator& mutator) {
            mutator.VisitHeapReferences(rawRootVisitor, derivedVisitor);
        });
    Heap::GetHeap().VisitStaticRoots(rawRootVisitor);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rawRootVisitor);
    collectorResources.GetFinalizerProcessor().VisitRawPointers(rawRootVisitor);
    Heap::GetHeap().VisitAllExportRoots(rawRootVisitor);
    if (NullslotProbeEnabled() && callN < 32) {
        std::fprintf(stderr,
                     "[GCV2][nullslot] path=fix_minor_roots end n=%zu dEntry=%zu dOld=%zu dHealNull=%zu "
                     "entry=%zu old=%zu healNull=%zu\n",
                     callN, g_resolveRootEntry.load(std::memory_order_relaxed) - entryBefore,
                     g_resolveRootOld.load(std::memory_order_relaxed) - oldBefore,
                     g_resolveRootHealNull.load(std::memory_order_relaxed) - nullBefore,
                     g_resolveRootEntry.load(std::memory_order_relaxed),
                     g_resolveRootOld.load(std::memory_order_relaxed),
                     g_resolveRootHealNull.load(std::memory_order_relaxed));
        std::fflush(stderr);
    }
}

// fixinput: FixMinorObjectSlots reader-side accounting (default on, cheap atomics).
// Reject arm must not silent-drop a real interior edge: recover host when Plausible.
// tip-in-heap / non-object: no legitimate field edges — account + sample, no invent.
namespace {
std::atomic<size_t> g_fixinputReject{ 0 };
std::atomic<size_t> g_fixinputRecover{ 0 };
std::atomic<size_t> g_fixinputUnrecoverable{ 0 };
} // namespace

void WCollector::FixMinorObjectSlots(BaseObject* object, const ScopedStopTheWorld* stw)
{
    // secondclass ②: belt-and-braces — refuse null tip before HasRefField.
    if (object == nullptr || !object->IsValidObject()) {
        return;
    }
    // fixinput / nilclass 丙: mark side already uses PlausibleManagedObjectGate
    // (PushYoungObject / TraceYoungClosure); Fix only had IsValidObject (tip≠null).
    // Coloured heap ref as tip (tip-in-heap) still passes IsValidObject → SEGV_nil in
    // ForEachBitmapWord. Reuse gate semantics at the consumer; do not relax the gate.
    if (!Collector::PlausibleManagedObjectGate("FixMinorObjectSlots", object)) {
        size_t n = g_fixinputReject.fetch_add(1, std::memory_order_relaxed) + 1;
        BaseObject* host = Collector::TryRecoverInteriorBase(object);
        // Only rescan when host itself is a real managed object (classic RawArray+8).
        // tip-in-heap residuals must not invent a false host via ClassifyInteriorOffset.
        if (host != nullptr && host != object &&
            Collector::PlausibleManagedObjectGate("FixMinorObjectSlots.host", host)) {
            g_fixinputRecover.fetch_add(1, std::memory_order_relaxed);
            FixMinorObjectSlots(host, stw);
            return;
        }
        g_fixinputUnrecoverable.fetch_add(1, std::memory_order_relaxed);
        // Edge disposition: not a managed object header — no legitimate field edges.
        if (n <= 16) {
            LOG(RTLOG_ERROR,
                "[GCV2][fixinput] reject FixMinorObjectSlots obj=%p tip=%p n=%zu "
                "reason=non-object-no-host (edge: no field walk; host unknown)",
                object, object->GetTypeInfo(), n);
        }
        return;
    }
    if (!object->HasRefField()) {
        return;
    }
    // eatarm brackets the host so an IOR can be attributed to the object being fixed;
    // nullgate names the edge inside. Both are gated and neither subsumes the other.

    object->ForEachRefField([this, object, stw](RefField<>& field) {
        (void)FixMinorEvacuatedSlot(field, nullptr, stw);
    });

}

void WCollector::EvacuateYoungRegions(const std::vector<BaseObject*>& reachableVec,
                                       const MinorSlotSet& rememberedSlots,
                                       const MinorObjectSet& currentMinorRoots,
                                       bool refFixSlotsCoveredByReachable,
                                       const MinorInteriorBaseMap& interiorBases,
                                       std::unique_ptr<ScopedStopTheWorld>* stw)
{
    RegionManager& manager = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    auto postEvacPoint = [this](const char* point, bool runHeap = true) {
        if (!kVerifyPostEvac) {
            return;
        }
        // Breadcrumb first (survives if VerifyHeap SEGV); force=true skips VERIFY_HEAP env.
        VLOG(REPORT, "[GCV2][verify][post-evac] enter point=%s run=%zu", point, minorTotalRuns + 1);
        if (runHeap) {
            VerifyHeapObjects(point);
            VLOG(REPORT, "[GCV2][verify][post-evac] point=%s run=%zu", point, minorTotalRuns + 1);
        }
    };
    // ZGC scans holders only through load-good addresses after relocate
    // (zBarrier.inline.hpp:294-343; zGeneration.cpp:1490-1523). reachableVec
    // was captured before Compact/Forward and therefore contains from-space
    // object addresses; passing those directly to ForwardObject after compact
    // reinterprets a cleared old location as a new relocation request.
    auto currentObject = [this](BaseObject* object) {
        const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, object, &object };
        BaseObject* const resolved = ResolveStoreValue(object, provenance);
        CHECK_DETAIL(resolved != nullptr && Heap::IsHeapAddress(resolved) &&
                         Collector::JudgeHandOutTarget(resolved) == HandVerdict::Usable,
                     "minor holder must resolve load-good before field scan from=%p resolved=%p",
                     object, resolved);
        return resolved;
    };

    // ZGC Phase 7/8 (zGeneration.cpp:573-580, 918-931, 850-853): pause_relocate_start
    // is flip + set_phase(Relocate) + _relocate.start(); object copy is concurrent.
    // Flip is the trap that makes mutator loads take the self-heal / relocate_object
    // path; without it, concurrent copy is the empty window concreffix measured.
    auto liveStw = [stw]() -> const ScopedStopTheWorld* {
        return (stw != nullptr && *stw != nullptr) ? stw->get() : nullptr;
    };
    const bool doYoungFlip = true;
    GCWorkers& workers = GetWorkers();

    std::vector<MAddress> remsetVec;
    remsetVec.assign(rememberedSlots.begin(), rememberedSlots.end());

    std::vector<std::pair<MAddress, MAddress>> currentRootRanges;
    currentRootRanges.reserve(currentMinorRoots.size());
    for (BaseObject* root : currentMinorRoots) {
        MAddress start = reinterpret_cast<MAddress>(root);
        currentRootRanges.emplace_back(start, start + RegionSpace::GetAllocSize(*root));
    }
    auto holderIsCurrentRoot = [&currentRootRanges](MAddress slot) {
        return std::any_of(currentRootRanges.begin(), currentRootRanges.end(),
                           [slot](const auto& range) { return slot >= range.first && slot < range.second; });
    };

    auto fixHeapSlice = [this, &reachableVec, &remsetVec, &interiorBases, &currentObject, &liveStw,
                         &holderIsCurrentRoot](
                            size_t beginObj, size_t endObj, size_t beginSlot, size_t endSlot,
                            size_t& objectsTaken) {
        const ScopedStopTheWorld* evacStw = liveStw();
        for (size_t i = beginObj; i < endObj; ++i) {
            FixMinorObjectSlots(currentObject(reachableVec[i]), evacStw);
            ++objectsTaken;
        }
        for (size_t i = beginSlot; i < endSlot; ++i) {
            MAddress slot = remsetVec[i];
            if (Heap::IsHeapAddress(slot)) {
                auto known = interiorBases.find(slot);
                BaseObject* knownBase = known != interiorBases.end() ? known->second : nullptr;
                (void)FixMinorEvacuatedSlot(HeapSlotAt<>(slot), knownBase, evacStw,
                                            holderIsCurrentRoot(slot));
            }
        }
    };

    auto fixHeapParallelOnly = [this, &reachableVec, &remsetVec, &fixHeapSlice](GCWorkers& pool) {
        const size_t dispelAtEntry = RegionInfo::GetDispelGhostCount();
        const size_t nObj = reachableVec.size();
        const size_t nSlot = remsetVec.size();
        const uint32_t heapWorkers = pool.ActiveWorkers();
        std::vector<size_t> objectsTaken(heapWorkers, 0);
        std::atomic<size_t> objCursor{ 0 };
        std::atomic<size_t> slotCursor{ 0 };
        const size_t objChunk = std::max<size_t>(64, (nObj + static_cast<size_t>(heapWorkers) * 4 - 1) /
                                                        (static_cast<size_t>(heapWorkers) * 4 + 1));
        const size_t slotChunk = std::max<size_t>(64, (nSlot + static_cast<size_t>(heapWorkers) * 4 - 1) /
                                                         (static_cast<size_t>(heapWorkers) * 4 + 1));
        class HeapTask final : public GCWorkerTask {
        public:
            HeapTask(decltype(fixHeapSlice) slice, std::atomic<size_t>& objs, std::atomic<size_t>& slots,
                     size_t nObjects, size_t nSlots, size_t objectChunk, size_t slotChunk,
                     std::vector<size_t>& taken)
                : fixHeapSlice(slice), objCursor(objs), slotCursor(slots), nObj(nObjects), nSlot(nSlots),
                  objChunk(objectChunk), slotChunk(slotChunk), objectsTaken(taken) {}
            void Work(uint32_t id) override
            {
                size_t* taken = &objectsTaken[id];
                for (;;) {
                    size_t o0 = nObj;
                    size_t o1 = nObj;
                    size_t s0 = nSlot;
                    size_t s1 = nSlot;
                    bool got = false;
                    if (objCursor.load(std::memory_order_relaxed) < nObj) {
                        o0 = objCursor.fetch_add(objChunk, std::memory_order_relaxed);
                        if (o0 < nObj) {
                            o1 = std::min(o0 + objChunk, nObj);
                            got = true;
                        } else {
                            o0 = o1 = nObj;
                        }
                    }
                    if (slotCursor.load(std::memory_order_relaxed) < nSlot) {
                        s0 = slotCursor.fetch_add(slotChunk, std::memory_order_relaxed);
                        if (s0 < nSlot) {
                            s1 = std::min(s0 + slotChunk, nSlot);
                            got = true;
                        } else {
                            s0 = s1 = nSlot;
                        }
                    }
                    if (!got) {
                        break;
                    }
                    fixHeapSlice(o0, o1, s0, s1, *taken);
                }
            }
        private:
            decltype(fixHeapSlice) fixHeapSlice;
            std::atomic<size_t>& objCursor;
            std::atomic<size_t>& slotCursor;
            size_t nObj;
            size_t nSlot;
            size_t objChunk;
            size_t slotChunk;
            std::vector<size_t>& objectsTaken;
        } task(fixHeapSlice, objCursor, slotCursor, nObj, nSlot, objChunk, slotChunk, objectsTaken);
        pool.Run(task);
        const size_t dispelAtExit = RegionInfo::GetDispelGhostCount();
        CHECK_DETAIL(dispelAtExit == dispelAtEntry,
                     "T-D ghost dispel during concurrent heap ref_fix entry=%zu exit=%zu",
                     dispelAtEntry, dispelAtExit);
        size_t active = 0;
        std::string takenStr;
        for (size_t i = 0; i < objectsTaken.size(); ++i) {
            if (objectsTaken[i] != 0) {
                ++active;
            }
            if (i != 0) {
                takenStr += ',';
            }
            takenStr += std::to_string(objectsTaken[i]);
        }
        VLOG(REPORT,
             "[GCV2][reffix][conc_heap] workers_active=%zu workers_scheduled=%u objects_taken=[%s] "
             "nObj=%zu nSlot=%zu cas_ok=%zu cas_fail=%zu concurrent=1",
             active, heapWorkers, takenStr.c_str(), nObj, nSlot,
             g_minorRefCasOk.load(std::memory_order_relaxed),
             g_minorRefCasFail.load(std::memory_order_relaxed));
    };

    // Earliest post-mark checkpoint: still before any fix/forward mutates refs.
    postEvacPoint("evac-enter", true);

    {
        // minortime: ⑦ ref fix (preforward roots + fixForwardedReferences)
        MRT_PHASE_TIMER("young.ref_fix");

        // ZGC relocate_start (zGeneration.cpp:918-931): flip remap colour then
        // enter Relocate. Product path.
        //
        // fliporder: that citation covers only half of what ZGC does here.  ZGC installs the
        // relocation set at zGeneration.cpp:254, inside the *concurrent* select_relocation_set,
        // and only then runs pause_relocate_start -> relocate_start -> flip_relocate_start
        // (:918 -> :922 -> :651).  So when ZGC's colour flips, the set of pages that will move is
        // already fixed and published.
        //
        // Ours flipped first and prepared from-space afterwards (PrepareForwardTable<Young> below),
        // which opens a window where the current remap colour is already the new one while no
        // region is marked FROM yet.  Anything painted store-good in that window names an object
        // whose region is about to become FROM: once it is copied the slot is load-good and names
        // the from-version, so the read barrier's fast path hands it straight to the mutator with
        // ObjectState::FORWARDED still in its header -- and the compiler reads that header as one
        // 64-bit word, so (3 << 48) enters an address and faults non-canonically.
        //
        // Measured: BarrierPhase::FORWARD hand-outs are 100% hasTo=1, unmov=0, slotGood=1, i.e. the
        // target really was forwarded, is not in an unmovable region, and the slot was load-good --
        // which after a flip can only mean it was written after that flip.
        //
        // Our own major path already has the ZGC order: PrepareForwardTable<Old> at :2533 runs
        // before flip_young/old_relocate_start at :2552-2553.  The two paths disagreed.
        {
            MRT_PHASE_TIMER("young.ref_fix_prepare");

            // iorfix: PrepareForwardTable FIRST so liveInfo0 snapshots the closed mark
            // domain while every from region is still FORWARDABLE, THEN pass1 Fix/Forward.
            // Prior order let FixMinorRootSlots RouteRegion before the domain snapshot.
            TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
            fwdTable.PrepareForwardTable<Generation::Young>();
            // zGeneration.cpp:1503-1508: install forwarding then flip remap bits.
            if (doYoungFlip) {
                flip_young_relocate_start();
            }
#if defined(MRT_TESTABLE_INTERNALS)
            // Prepared and flipped, still POST_TRACE: a test driver can enter
            // the ordinary phase-refused mutator path without sealing copying.
            RunRemapWindowTestHook(7, nullptr, nullptr);
#endif
            // Publish the relocate phase and submit page work while the
            // existing young pause still excludes mutator execution. Root
            // transition may now wait for a real page task on allocation failure.
            Heap::GetHeap().InstallBarrier(GCPhase::GC_PHASE_PREFORWARD);
            Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_PREFORWARD);
            StartRelocationTasks();
            // zRelocate.cpp:1289-1300 generation workers run relocate_task before
            // any consumer add_and_wait (zRelocate.cpp:393-405). Drain is
            // GCWorkers::Run: it publishes and joins page work. TransitionToGCPhase
            // synchronously visits mutator roots (TracingCollector.h:463) which
            // may WaitForPageForwarding, so workers must already be running.
            manager.DrainForwardFromRegions<Generation::Young>();
            TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);
            postEvacPoint("pre-fix-forwarded", false);
        }

        // pass1 root fix after the domain snapshot.
        // pass1 is load-bearing for previous-gen residual (MINOR_CONCURRENCY §七 T-A).
        {
            MRT_PHASE_TIMER("young.ref_fix_root_pass1");
            FixMinorRootSlots(liveStw());
            PreforwardDiscoveredExternObjects();
            PreforwardAllResurrectExportFromObjects();
            postEvacPoint("post-preforward-roots", false); // breadcrumb only — avoid SEGV before fix body
        }

        // Reset CAS counters for this fix window (positive-control visibility).
        g_minorRefCasFail.store(0, std::memory_order_relaxed);
        g_minorRefCasOk.store(0, std::memory_order_relaxed);

    }

    {
        TransitionToGCPhase(GCPhase::GC_PHASE_FORWARD, true);
        {
            stw->reset();
            MRT_PHASE_TIMER("young.concurrent_relocate");
            VLOG(REPORT, "[GCV2][relocate][conc] concurrent_relocate start nObj=%zu flip=1",
                 reachableVec.size());
            ForwardFromSpace();
#if defined(MRT_TESTABLE_INTERNALS)
            RunRemapWindowTestHook(4, nullptr, nullptr);
#endif
            *stw = std::make_unique<ScopedStopTheWorld>("young post-relocate", true,
                                                        GCPhase::GC_PHASE_FORWARD);
            manager.FinishIncompleteFromRegions();
        }
        VLOG(REPORT, "[GCV2][relocate][conc] concurrent_relocate done; STW re-entered");
        postEvacPoint("post-forward-pre-reclaim", true);
        {
            MRT_PHASE_TIMER("young.ref_fix_bulk");
            g_minorRefCasFail.store(0, std::memory_order_relaxed);
            g_minorRefCasOk.store(0, std::memory_order_relaxed);
            FixMinorRootSlots(liveStw());
            PreforwardDiscoveredExternObjects();
            PreforwardAllResurrectExportFromObjects();
            remsetVec.assign(rememberedSlots.begin(), rememberedSlots.end());
            {
                // ZGC immediately scans buffered entries that crossed the young
                // flip (zStoreBarrierBuffer.cpp:162-187). Publish all mutator
                // buffers before the active-face Snapshot used for this ref fix.
                StoreBarrierBuffer::FlushAll(Heap::GetHeap().GetRememberedSet());
                std::unordered_set<MAddress> concRemset =
                    Heap::GetHeap().GetRememberedSet().Snapshot();
                remsetVec.reserve(remsetVec.size() + concRemset.size());
                for (MAddress slot : concRemset) {
                    remsetVec.push_back(slot);
                }
                VLOG(REPORT,
                     "[GCV2][relocate][conc_stw] remset pre=%zu conc_new=%zu total=%zu",
                     rememberedSlots.size(), concRemset.size(), remsetVec.size());
            }
            fixHeapParallelOnly(workers);
        }
        {
            size_t rej = g_fixinputReject.load(std::memory_order_relaxed);
            size_t rec = g_fixinputRecover.load(std::memory_order_relaxed);
            size_t unr = g_fixinputUnrecoverable.load(std::memory_order_relaxed);
            if (rej != 0 || rec != 0 || unr != 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][fixinput] reject=%zu recover=%zu unrecoverable=%zu",
                    rej, rec, unr);
            }
            manager.ExpireKeptFromPreviousCycle();
            if (HealCoverage::kHealCoverageCensus) {
                HealCoverage::CensusAfterPublication(
                    currentRemapColour, FlipSeq().load(std::memory_order_relaxed),
                    "young-ref-fix-conc");
            }
        }
    }

    {
        // minortime: ⑧ finish inside evacuate (promote residual + remset rebuild + reassemble)
        MRT_PHASE_TIMER("young.evac_finish");
        // Residual remset walk no longer runs here; residualPromote stays 0.
        // The walk is young.conc_promote_walk after STW3 release.
        size_t residualPromoteRecords = 0;
        // Positive-control only (rebuildgate): force one live young region so the
        // rebuild gate must open. Prefer leaving a residual young undemoted; if
        // residualPromote path is empty (product real_load: residual≡0), re-tag
        // the first minor candidate as young after demote. Default off.
        {
        // Decision + Register + Promote stay in STW3 (O(regions)). The remset walk
        // moved to young.conc_promote_walk after STW3 release (zRelocate.cpp:1257-1306).
        for (RegionInfo* region : minorCandidateRegions) {
            if (region->IsYoungRegion()) {
                // markwater2: allocating pages never entered the route plan
                // (zGeneration.cpp:211-213). Leave them young on unmovableFrom;
                // next PrepareYoung ClearLiveInfo re-snapshots the watermark.
                MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
                const bool hasObjectLiveness = region->IsLargeRegion() ||
                    region->GetMarkBitmap(promotionView) != nullptr || region->GetResurrectBitmap() != nullptr;
                if (!PromotedRegionDomain::ResidualPromotionHasClosedLiveness(
                        region->HasMarkStartAllocGap(), region->IsLiveCountAuthoritative(),
                        hasObjectLiveness)) {
                    continue;
                }
                if (kPageAgeAdaptiveTenuring &&
                    !ShouldPromoteAge(region->GetYoungAge(), GetGCStats().tenuringThreshold)) {
                    if (region->IsLoneFromRegion() || region->IsFromRegion()) {
                        manager.EnlistStayYoungSurvivor(region);
                    } else if (region->GetRegionType() != RegionInfo::RegionType::RECENT_FULL_REGION) {
                        RegionManager::FinishStayYoungInPlace(region);
                    }
                    continue;
                }
                // Residual candidates not forwarded above (e.g. raw-pointer pinned):
                // still demote to old. Remset walk is ZRelocateAddRemsetForFlipPromoted
                // (zRelocate.cpp:1257-1306): Register+Promote here under STW3, walk after
                // STW3 release (young.conc_promote_walk). Do not Record here — that walk
                // is the STW cost this lane moves out.
                region->PreserveRetainedLiveInfo();
                PromotedRegionDomain::Register(region, PromotedRegionDomain::RegisterPath::Residual);
                PromotedRegionDomain::NoteRegisterGate(static_cast<uint32_t>(GC_REASON_YOUNG),
                                                       /*site*/ 2, /*registered*/ true);
                (void)region->PromoteYoungRegion(promotionView);
            }
        }
        }
        size_t promotedPathRecords = RegionManager::ConsumePromotedCrossGenEdgeCount();

        const size_t liveYoungRegions = RegionInfo::GetYoungRegionCount();
        VLOG(REPORT,
             "[GCV2Minor] remembered-set promoteReplay=%zu residualPromote=%zu "
             "youngRegionCount=%zu",
             promotedPathRecords, residualPromoteRecords, liveYoungRegions);



    }

    // ZRelocateAddRemsetForFlipPromoted runs after STW3 release, still in FORWARD.
    // Keep the forwarding receipts alive across this concurrent walk: a short retire
    // safepoint below performs PrepareForwardTable only after DischargeAll has resolved
    // every promoted field. This preserves the prerequisite recorded by 3ddac725f8
    // without violating PromotedRegionDomain.h's off-STW lifecycle contract.
    CHECK_DETAIL(stw != nullptr && *stw != nullptr,
                 "promoted-domain discharge must release an active STW3 owner");
    stw->reset();
    {
        MRT_PHASE_TIMER("young.conc_promote_walk");
        if (PromotedRegionDomain::Enabled()) {
            RememberedSet& remsetForDomain = Heap::GetHeap().GetRememberedSet();
            size_t domainEdges = PromotedRegionDomain::DischargeAll(
                [this](RefField<>& field) -> BaseObject* { return ResolveMinorReference(field); },
                [&remsetForDomain](MAddress slot) { remsetForDomain.Record(slot); });
            PromotedRegionDomain::NoteRecordCall(static_cast<uint32_t>(GC_REASON_YOUNG),
                                                 /*site*/ 3, domainEdges);
            PromotedRegionDomain::DumpReconcile(minorTotalRuns + 1, "conc_promote_walk");
            PromotedRegionDomain::DumpProcessTotals("conc_promote_walk");
            VLOG(REPORT, "[PROMODOMAIN] dischargeEdges=%zu", domainEdges);
        } else {
            PromotedRegionDomain::DumpCoverageByReason("conc_promote_walk_domain_off");
        }
    }

    *stw = std::make_unique<ScopedStopTheWorld>("young retire forwarding", true,
                                                GCPhase::GC_PHASE_FORWARD);
    {
        MRT_PHASE_TIMER("young.evac_retire");
        {
        // PROBE evacct: how much of young.evac_finish is the second PrepareForwardTable<Young>?
        MRT_PHASE_TIMER("young.evac_prepare_next");
        fwdTable.PrepareForwardTable<Generation::Young>();
        }
        // zRelocate.cpp:1041-1047 cycle-end completeness: no ROUTED-unfinished page.
        manager.FinishIncompleteFromRegions();
        manager.ReassembleFromSpace();
    }
}
// permhole receiptization (steer1): RouteObject is geometric (ROUTED before Copy fills
// tip). A tip-valid to is a *receipt* (copy happened). A geometric to with tip==0 is only
// a plan — never hand it to make_load_good / IdleBarrier self-heal (THIRD_mutator hang).
//
// Contract of this wait:
//   ① return tip-valid to (receipt), or
//   ② fail the relocation invariant;
//   ③ never return a from address or a null-tip geometric address.
// Distinct from 4e75f2cc: that path is RouteObject *miss* (no plan) on a ghost about to
// be reclaimed — returning from there reinstalls a dying address. Here RouteObject *hit*
// with no tip yet: while still ROUTED/ROUTING, from is not yet CollectRegion'd.
// After object/region publish (FORWARDED|COMPACTED) tip must exist if the plan was real;
// missing tip = permanent hole = invariant violation → CHECK (not hang, not geometric to).
//
// Diag: MRT_GCV2_WAITFWD=1 counts enter / tip-ready / give-up (gate before counter work).

// inplaceto: after an in-place compaction the from-layout and the to-layout occupy the *same*
// page span, so the page-scoped ghost-from predicate cannot tell a stale from-address from an
// address that has already been relocated.  ZGC never has to tell them apart: a to-pointer
// carries the remapped colour, its barrier fast path returns before the forwarding table is
// consulted, and zRelocate.cpp:382-389 is therefore only ever entered with a from-address.
// Our root words are plain (no colour), so the discriminator has to be rebuilt from the page's
// own geometry -- the same geometry ZGC records for this exact overlap in
// ZForwarding::in_place_relocation_start (zForwarding.cpp:55-64, _in_place_top_at_start) and
// consumes in ZHeap::is_in (zHeap.cpp:202-208).
//
// The three cases are mutually exclusive and jointly exhaustive for a compacted page whose
// forwarding lookup missed:
//
//   survived(off)              the from-livemap covers this offset, so PublishKeptInPlaceReceipts
//                              (RegionManager.cpp:2151-2199) owed a receipt for it and there is
//                              none -> the invariant is broken, refuse.
//   off < allocPtr             the in-place compaction wrote the to-layout over this offset; no
//                              from object is covered here and none ever was, so the address is
//                              a to-address (or an interior of one) and is already current.
//   off >= allocPtr            the abandoned tail above the new top: the from copy is gone and no
//                              to-object was written here -> nothing can be named, refuse.
//
// Measured on NW256/256MB, 3/3 verbatim: a base register root held from-offset 33480 at mark and
// to-offset 27320+2048 at the major PreForward, with the table mapping 33480 onto 27320
// (revBaseHit=1 revBaseFromOff=33480).  The root was current; the walk asked anyway.
// kAlreadyTo is split by what the page's own size walk says the address *is*.  ZGC's heap oop
// fields hold object starts by construction -- interior pointers exist only as derived oops
// paired with a base in an oop map (oopMap.cpp:404-424) and never in a field -- so an interior
// reaching a heap-field consumer is not a to-address that needs recognising, it is a value that
// names nothing.  Only the root-side consumers, where an interior is a legal register value, may
// take kAlreadyToInterior.
enum class CompactedMissClass : uint8_t { kReceiptOwed, kAlreadyToStart, kAlreadyToInterior,
                                          kAbandonedTail };

static CompactedMissClass ClassifyCompactedMiss(RegionInfo* region, BaseObject* obj)
{
    const MAddress addr = reinterpret_cast<MAddress>(obj);
    const MAddress start = region->GetRegionStart();
    const MAddress allocPtr = region->GetRegionAllocPtr();
    if (addr < start) {
        return CompactedMissClass::kAbandonedTail;
    }
    const size_t off = static_cast<size_t>(addr - start);
    // Inside an in-place compaction the from- and to-layouts share one span, so
    // "the from-livemap covers off" and "off is a published destination" are both true of the
    // same address whenever some from-object landed on top of another from-object's start.  The
    // three cases above are therefore NOT disjoint in that overlap, and asking the livemap first
    // classified a live to-object start as an owed receipt: measured on NW256/256MB, a root at
    // to-offset 18584 with survived=1 isStart=1 whose reverse lookup named from-offset 37256 as
    // the object copied there (revHit=1 revBaseHit=1) was refused as try.compacted-no-receipt.
    // Provenance is a claim only the page's own table can attest, so ask the table before the
    // livemap -- ZGC resolves the identical overlap from ZForwarding::_in_place_top_at_start plus
    // the forwarding entry, never from liveness (zForwarding.cpp:55-64; zHeap.cpp:202-208).
    if (addr < allocPtr) {
        ZForwarding* provenance = ForwardingTable::GetCovering(start);
        MAddress revFrom = 0;
        if (provenance != nullptr && provenance->find_from_by_to(addr, &revFrom) && revFrom >= start) {
            return CompactedMissClass::kAlreadyToStart;
        }
    }
    if (region->IsOwnerSurvivedObject(off)) {
        return CompactedMissClass::kReceiptOwed;
    }
    if (addr >= allocPtr) {
        return CompactedMissClass::kAbandonedTail;
    }
    // The to-layout size walk is the discriminator, so it runs before the class is decided, not
    // only for the diagnostic below.  A page whose walk cannot name a container for this address
    // has published nothing that covers it: refuse.
    size_t contOff = 0;
    size_t contSize = 0;
    size_t contDelta = 0;
    unsigned contFound = 0;
    if (region->IsLargeRegion()) {
        contFound = 1;
        contOff = 0;
        contSize = static_cast<size_t>(allocPtr - start);
        contDelta = off;
    } else {
        MAddress position = start;
        for (size_t steps = 0; position < allocPtr && steps < (1u << 20); ++steps) {
            BaseObject* o = reinterpret_cast<BaseObject*>(position);
            if (!MapleRuntime::PlausibleManagedObjectGate("inplacepop-walk", o)) {
                break;
            }
            const size_t allocSize = RegionSpace::GetAllocSize(*o);
            if (allocSize == 0) {
                break;
            }
            if (addr >= position && addr < position + allocSize) {
                contFound = 1;
                contOff = static_cast<size_t>(position - start);
                contSize = allocSize;
                contDelta = static_cast<size_t>(addr - position);
                break;
            }
            position += allocSize;
        }
    }
    if (contFound == 0) {
        return CompactedMissClass::kAbandonedTail;
    }
    return contDelta == 0 ? CompactedMissClass::kAlreadyToStart
                          : CompactedMissClass::kAlreadyToInterior;
}


// portmutreloc: ZRelocate::relocate_object's retain/copy/release leg (zRelocate.cpp:391-406).
//
// The three pieces map one-to-one onto machinery that already exists here:
//
//   forwarding->retain_page(&_queue)   ->  RegionInfo::TryLockReadFromRegion()
//   relocate_object_inner(...)         ->  ForwardObjectImpl(obj, forwarding), whose
//                                          ForwardObjectExclusive does RouteObject (= ZGC's
//                                          alloc_object_for_relocation, except our
//                                          to-address is pre-planned so it cannot fail for
//                                          want of memory), CopyObject (= object_copy_disjoint)
//                                          and UnlockObject(FORWARDED) (= forwarding->insert)
//   forwarding->release_page()         ->  RegionInfo::UnlockReadFromRegion()
//
// TryForwardObject (below) already composes exactly these three, which is why this is a reuse
// and not a second implementation. What was missing was a caller on the mutator's remap
// funnel: relocate_or_remap_object never had this leg, so a mutator that arrived before the
// copy either got the from pointer back or waited for a worker.
//
// nullptr means the current thread did not acquire the page. The caller may
// consume a receipt installed by the owning copier, but may not use the from
// address as an alternate result.
BaseObject* WCollector::WaitForPageForwarding(BaseObject* obj, ForwardingTable::Owner owner) const
{
    if (!owner || ZForwardingLife::CurrentPageWork() == owner.get()) return nullptr;
    const MAddress from = reinterpret_cast<MAddress>(obj);
    if (const MAddress found = owner->resolve_life(owner->find(from))) {
        return reinterpret_cast<BaseObject*>(found);
    }
    auto& queue = static_cast<RegionSpace&>(theAllocator).GetRegionManager().GetRelocationRequestQueue();
    const auto request = queue.Add(owner);
    CHECK_DETAIL(request.accepted, "relocation request has no page task from=%#zx", from);
    (void)queue.Wait(request.request);
    return reinterpret_cast<BaseObject*>(owner->resolve_life(owner->find(from)));
}

BaseObject* WCollector::TryMutatorRelocate(BaseObject* obj, RegionInfo* forwarding) const
{
    MutatorRelocate::NoteAttempt();
    // ForwardObjectImpl opens with CHECK(phase == PREFORWARD || FORWARD). relocate_or_remap
    // is reachable from barriers in other phases, so screen here rather than trip that CHECK.
    GCPhase phase = GetGCPhase();
    if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
        MutatorRelocate::NoteFallback(MutatorRelocate::Fallback::PHASE);
        return nullptr;
    }
    // retain_page. A try-lock, so a losing mutator falls back instead of blocking -- ZGC's
    // retain_page also gives up (returns false) when the page is claimed or released.
    RegionInfo::RetainScope lease(forwarding);
    if (!lease.ok()) {
        MutatorRelocate::NoteFallback(MutatorRelocate::Fallback::RETAIN_FAILED);
        return WaitForPageForwarding(obj, lease.HoldForwarding());
    }
    // zRelocate.cpp:393-395: retain_page then assert is_phase_relocate.
    // SetGCPhase publishes before handshake, so a mutator that retained across
    // FORWARD→IDLE must not enter ForwardObjectImpl's CHECK. Release and let
    // the existing FindToVersion / wait legs consume the published table.
    phase = GetGCPhase();
    if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
        lease.Release();
        MutatorRelocate::NoteFallback(MutatorRelocate::Fallback::PHASE);
        return nullptr;
    }
    MutatorRelocate::NoteRetainOk();
    // A mutator can publish a previously white from-object after young mark
    // terminated. Admit it before copying; a next-minor remset entry is too late.
    // This is the late-store leg corresponding to zBarrier.inline.hpp:695-716.
    EnsureRouteDomainMembership(const_cast<WCollector*>(this), obj);
    // The scope is what lets ForwardObjectExclusive attribute the copy to this thread. Counting
    // at this call site instead would conflate "this mutator copied the object" with "this
    // mutator retained and then found a worker had already copied it" -- and only the first of
    // those is evidence that the ported leg does anything.
    const bool wasForwarded = obj->IsForwarded();
    MutatorRelocate::EnterScope();
    BaseObject* toVersion = const_cast<WCollector*>(this)->ForwardObjectImpl(obj, forwarding, lease);
    MutatorRelocate::LeaveScope();
    lease.Release(); // release_page
    if (wasForwarded) {
        MutatorRelocate::NoteAlreadyForwarded();
    }
    if (toVersion == nullptr) {
        MutatorRelocate::NoteFallback(MutatorRelocate::Fallback::COPY_FAILED);
        return WaitForPageForwarding(obj, lease.HoldForwarding());
    }
    if (toVersion == obj) {
        // ForwardObjectImpl resolved to the from address: not a relocation. Let the old legs
        // decide what to hand back rather than short-circuiting them with an unmoved pointer.
        MutatorRelocate::NoteFallback(MutatorRelocate::Fallback::COPY_FAILED);
        return nullptr;
    }
    return toVersion;
}

BaseObject* WCollector::ResolveStoreValue(BaseObject* ref, const ForwardingProvenance& provenance) const
{
    // zBarrier.inline.hpp:695-716 store_barrier_on_heap_oop_field:
    // color_store_good includes remap. A movable ghost-from value must go
    // through the same relocate_or_remap funnel as the load barrier
    // (zRelocate.cpp:382-416) before it is painted store-good.
    BaseObject* current = ref;
    for (;;) {
        if (current == nullptr || !Heap::IsHeapAddress(current)) {
            return current;
        }
        if (IsAlreadyToStoreValue(current)) {
            return current;
        }
        const MAddress currentAddr = reinterpret_cast<MAddress>(current);
        RegionInfo* currentRegion = RegionInfo::GetGhostFromRegionAt(currentAddr);
        if (currentRegion != nullptr && currentRegion->IsCompactRouteDestination(currentAddr) &&
            Collector::JudgeHandOutTarget(current) == HandVerdict::Usable) {
            // Dense in-place destinations share the from page's address range.
            // Their presence in the completed compact route table is the
            // positive relocation receipt; region membership alone must not
            // reinterpret the packed to-address as another from-address.
            return current;
        }
        // inplaceto: the test above pairs a structural question (is this a compact-route
        // destination?) with a content heuristic on the header word, so an *interior* of a
        // relocated object -- whose header word is zero by construction, HandVerdict::ZeroHeader
        // -- can never satisfy it.  Measured, NW256/256MB 3/3: regionStart+4856 refused here with
        // verdict=2, while the table maps from 4872 onto to 4840 and the page layout holds a
        // 48-byte object at 4840 containing it (revBaseHit=1 revBaseFromOff=4872 contDelta=16).
        // The page geometry answers the structural question without reading the payload, which is
        // the order ZGC uses: the forwarding read never depends on the from copy's bytes
        // (zRelocate.cpp:382-389).
        if (currentRegion != nullptr && currentRegion->IsCompacted() &&
            ClassifyCompactedMiss(currentRegion, current) == CompactedMissClass::kAlreadyToStart) {
            // An address-shaped compact destination is not, by itself, a
            // load-good value.  In particular the from header may still be
            // FORWARDED (or a zero header for an interior-shaped probe), so
            // kAlreadyToStart is only a geometric classification.  The
            // resolve postcondition is the same as every other receipt hop:
            // only a Usable object may leave this function.  Keep the
            // non-Usable case on the receipt/relocate path below, which either
            // finds the explicit identity receipt or fails closed.
            if (Collector::JudgeHandOutTarget(current) == HandVerdict::Usable) {
                return current;
            }
        }
        FindToVersionResult found = FindToVersion(current);
        // A forwarding entry qualifies one hop, not necessarily the final
        // load-good value. The destination can already belong to the next
        // relocation set; follow that address-keyed forwarding generation too.
        // ZGC's load barrier returns only after remap/relocate has produced the
        // current address (zBarrier.inline.hpp:294-343; zRelocate.cpp:382-416).
        if (BaseObject* to = found.found()) {
            const HandVerdict verdict = Collector::JudgeHandOutTarget(to);
            if (verdict == HandVerdict::Usable) {
                // from->from is the explicit whole-page in-place receipt
                // (zRelocate.cpp:862-925,1013-1037), not a lookup miss.
                return to;
            }
            if (to != current) {
                current = to;
                continue;
            }
            // Identity with a still-forwarded header is not a hop. Finish
            // relocate_or_remap (zRelocate.cpp:382-416).
        }

        // A missing receipt is not a terminal miss while the from-region is
        // retained: the current thread completes relocation before publishing
        // the healed value (zBarrier.inline.hpp:294-343).
        RegionInfo* ghost = currentRegion;
        if (ghost == nullptr) {
            RegionInfo* live = RegionInfo::TryGetRegionInfoAt(currentAddr);
            if (live != nullptr && live->IsCompacted()) {
                const CompactedMissClass cls = ClassifyCompactedMiss(live, current);
                if (cls == CompactedMissClass::kAlreadyToStart &&
                    Collector::JudgeHandOutTarget(current) == HandVerdict::Usable) {
                    return current;
                }
            }
            if (live != nullptr && !live->IsFreeRegion() && !live->IsGarbageRegion() &&
                Collector::JudgeHandOutTarget(current) == HandVerdict::Usable &&
                !current->IsForwarded()) {
                return current;
            }
            const ForwardingTable::LookupResult lookup =
                Collector::JudgeHandOutTarget(current) == HandVerdict::ZeroHeader
                    ? ForwardingTable::LookupResult{ 0, ForwardingTable::ToAnswer::Unarmed,
                                                     ForwardingTable::ToUnavailableCause::None, false, false,
                                                     ForwardingTable::ToAnswer::Unarmed,
                                                     ForwardingTable::ToAnswer::Unarmed, false, false, 0 }
                    : ForwardingTable::LookupTo(currentAddr);
            LOG(RTLOG_ERROR,
                "[FWDTABLE][resolve-miss] site=no-forwarding consumer=WCollector::ResolveStoreValue "
                "holder_kind=%s holder=%p slot=%p stage=%s writer_kind=%s "
                "incoming_source_kind=%s source_slot=%p working_copy_slot=%p "
                "field_type=%s field_offset=%zu "
                "from=%p from_region=%p region_type=%u generation=%u "
                "in_current_relocation_set=%u table_id=%#zx lookup_state=%u lookup_cause=%u "
                "publication_generation=%llu from_page_epoch=%llu lifeId=%llu "
                "retired_lookup=%u gc_phase=%u ghost=0 compacted=%u route=%u lookup.to=%p "
                "publication_closed=%u verdict=%u",
                ForwardingProvenance::KindName(provenance.kind), provenance.holder, provenance.slot,
                ForwardingProvenance::StageName(provenance.stage),
                ForwardingProvenance::WriterName(provenance.writerKind),
                ForwardingProvenance::SourceName(provenance.incomingSourceKind), provenance.sourceSlot,
                provenance.workingCopySlot, ForwardingProvenance::FieldName(provenance.fieldKind),
                provenance.fieldOffset,
                static_cast<void*>(current), static_cast<void*>(live),
                live != nullptr ? static_cast<unsigned>(live->GetRegionType()) : 0xffu,
                live != nullptr ? static_cast<unsigned>(live->generation_id()) : 0xffu,
                lookup.currentMembership ? 1u : 0u, static_cast<size_t>(lookup.tableId),
                static_cast<unsigned>(lookup.answer), static_cast<unsigned>(lookup.unavailableCause),
                static_cast<unsigned long long>(lookup.publicationGeneration),
                static_cast<unsigned long long>(lookup.fromPageEpoch),
                static_cast<unsigned long long>(lookup.fromPageLifeId),
                static_cast<unsigned>(lookup.retiredAnswer), static_cast<unsigned>(GetGCPhase()),
                live != nullptr && live->IsCompacted() ? 1u : 0u,
                live != nullptr ? static_cast<unsigned>(live->GetRouteState()) : 0u,
                reinterpret_cast<void*>(lookup.to),
                lookup.publicationClosed ? 1u : 0u,
                static_cast<unsigned>(Collector::JudgeHandOutTarget(current)));
            FailClosedLoad("WCollector::ResolveStoreValue.no-forwarding", current, 0, provenance);
        }
        // A pointer with ghost membership belongs to a published forwarding
        // generation. Even after its route state changes it cannot be
        // reclassified as a non-member; only an explicit receipt or completed
        // relocation qualifies a value (zRelocate.cpp:408-415).
        if (ghost->IsUnmovableFromRegion() &&
            Collector::JudgeHandOutTarget(current) == HandVerdict::Usable) {
            return current;
        }
        BaseObject* resolved = relocate_or_remap_object(current, ghost->generation_id(), provenance);
        if (resolved == nullptr) {
            FailClosedLoad("WCollector::ResolveStoreValue.unresolved", current, 0, provenance);
        }
        if (resolved == current) {
            // In-place completion must have published its identity receipt;
            // without it, returning current would recreate the removed
            // lookup-miss fallback.
            FindToVersionResult identity = FindToVersion(current);
            if (identity.found() == current &&
                Collector::JudgeHandOutTarget(current) == HandVerdict::Usable) {
                return current;
            }
            // zGeneration.inline.hpp:131-135: forwarding table gone → safe(addr).
            // Ghost can be dispelled between the membership check and
            // relocate_or_remap; that is not a missing identity receipt.
            if (Collector::JudgeHandOutTarget(current) == HandVerdict::Usable &&
                !current->IsForwarded() &&
                RegionInfo::GetGhostFromRegionAt(currentAddr) == nullptr) {
                return current;
            }
            FailClosedLoad("WCollector::ResolveStoreValue.missing-identity", current, 0, provenance);
        }
        current = resolved;
    }
}

BaseObject* WCollector::ForwardObject(BaseObject* obj)
{
    // ZGC returns the original address only when forwarding-table membership
    // is absent (zGeneration.inline.hpp:131-140).  A stale RegionInfo face is
    // still membership and must resolve through a receipt or fail closed.
    // markfloor: stack/reg roots may hold RawArray+8 interiors (tip=length). Do not
    // GetSize/CopyObject them; leave the slot unchanged (caller keeps obj).
    if (!Collector::PlausibleManagedObjectGate("WCollector::ForwardObject", obj)) {
        // tipnull: uncopied movable ghost is not VisitLive success.
        if (IsGhostFromObject(obj) && !IsUnmovableFromObject(obj)) {
            // receiptfirst: ZRelocate::relocate_object opens with
            // `forwarding->find(from_addr, &cursor)` and returns on a hit
            // (zRelocate.cpp:382-389); forward_object then asserts that read answers
            // (zRelocate.cpp:411-415).  The receipt is consulted *before* anything is
            // read out of the from copy, and it has to be: relocation copies the object
            // away and reclamation is allowed to clear the from payload afterwards --
            // that cleared payload is exactly what HandVerdict::ZeroHeader names.  So a
            // content heuristic (PlausibleManagedObjectGate reads the tip word) must
            // never be a precondition for reading the table; ordering it first refuses
            // addresses whose to-version is already published.  A ghost-from movable
            // address is precisely the population ZGC calls relocate_object on -- a page
            // with a live ZForwarding -- so the table read belongs here, not after.
            if (BaseObject* published = FindToVersion(obj).found()) {
                return published;
            }
            // inplaceto: an interior of an *already relocated* object fails the content gate the
            // same way a stale from-address does, and on a compacted-in-place page both live in
            // the one span.  Classify by geometry rather than by the gate's answer.
            RegionInfo* ghostRegion = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
            if (ghostRegion != nullptr && ghostRegion->IsCompacted()) {
                const CompactedMissClass cls = ClassifyCompactedMiss(ghostRegion, obj);
                // Registers may hold interiors; heap fields may not, which is why the field
                // consumers above take only the start class.
                if (cls == CompactedMissClass::kAlreadyToStart ||
                    cls == CompactedMissClass::kAlreadyToInterior) {
                    return obj;
                }
            }
            return nullptr;
        }
        return obj;
    }
    BaseObject* to = TryForwardObject(obj);
    if (to != nullptr) {
        return to;
    }
    // GetRoute survivor gate / exclusive soft-miss: a movable ghost-from with no
    // to-version is not a stable address. Returning `obj` here reinstalls a from
    // pointer that CollectRegion is about to reclaim → UAF / HANG under ALOT.
    // Unmovable / non-ghost still keep `obj` (in-place / not in route domain).
    if (IsGhostFromObject(obj) && !IsUnmovableFromObject(obj)) {
        return nullptr;
    }
    return obj;
}

BaseObject* WCollector::TryForwardObject(BaseObject* obj)
{
    if (!Collector::PlausibleManagedObjectGate("WCollector::TryForwardObject", obj)) {
        // receiptfirst: same order as ForwardObject above -- zRelocate.cpp:382-389 reads
        // the forwarding table before the from copy is touched at all.  TryForwardObject
        // is the direct entry point for ForwardUpdateRawRef / FixMinorEvacuatedSlot, so
        // the published receipt has to be reachable from here too.
        if (IsGhostFromObject(obj) && !IsUnmovableFromObject(obj)) {
            if (BaseObject* published = FindToVersion(obj).found()) {
                return published;
            }
        }
        return nullptr;
    }
#if defined(MRT_GC_UNIT_TESTS)
    if (g_routeLookupTestContext != nullptr) {
        g_routeLookupTestContext->gatePassed = true;
        g_routeLookupTestContext->heapAddress = true;
    }
#endif
    RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
        return nullptr;
    }

#if defined(MRT_GC_UNIT_TESTS)
    if (g_routeLookupTestContext != nullptr) {
        g_routeLookupTestContext->receiptChecked = true;
    }
#endif
    if (BaseObject* mapped = FindToVersion(obj).found()) {
        return mapped;
    }
#if defined(MRT_GC_UNIT_TESTS)
    if (g_routeLookupTestContext != nullptr) {
        g_routeLookupTestContext->compactedChecked = true;
    }
#endif
    if (region->IsCompacted()) {
        // Compacted page: PublishKeptInPlaceReceipts installs identity forwarding for every
        // live object start recorded in the livemap (RegionManager.cpp:2151-2199).  A table
        // miss here is classified by the page's own geometry (see ClassifyCompactedMiss).
        switch (ClassifyCompactedMiss(region, obj)) {
            case CompactedMissClass::kAlreadyToStart:
            case CompactedMissClass::kAlreadyToInterior:
                return obj;
            case CompactedMissClass::kReceiptOwed:
                return nullptr;
            case CompactedMissClass::kAbandonedTail:
                return nullptr;
        }
    }
    const GCPhase phase = GetGCPhase();
    if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
        return nullptr;
    }
#if defined(MRT_GC_UNIT_TESTS)
    if (g_routeLookupTestContext != nullptr) {
        g_routeLookupTestContext->phaseAllowed = true;
    }
#endif

#if defined(MRT_GC_UNIT_TESTS)
    const bool routeRegion = fwdTable.RouteRegion(region);
    if (g_routeLookupTestContext != nullptr) {
        g_routeLookupTestContext->routeRegionCalled = true;
        g_routeLookupTestContext->routeRegion = routeRegion;
    }
    if (routeRegion) {
#else
    if (fwdTable.RouteRegion(region)) {
#endif
        // secondclass ①: GetRoute is geometric plan; retain before copying or
        // consuming from-side state (else null-tip → HasRefField SEGV si_addr=0x8).
        RegionInfo::RetainScope lease(region);
        if (lease.ok()) {
#if defined(MRT_GC_UNIT_TESTS)
            if (g_routeLookupTestContext != nullptr) {
                g_routeLookupTestContext->retained = true;
            }
#endif
            // zRelocate.cpp:393-395: retain_page then assert is_phase_relocate.
            // SetGCPhase can publish IDLE while this thread holds the retain.
            // Release and consume only the forwarding-table answer in that case.
            const GCPhase retainedPhase = GetGCPhase();
            if (retainedPhase != GCPhase::GC_PHASE_PREFORWARD &&
                retainedPhase != GCPhase::GC_PHASE_FORWARD) {
#if defined(MRT_GC_UNIT_TESTS)
                if (g_routeLookupTestContext != nullptr) {
                    g_routeLookupTestContext->retainedPhaseAllowed = false;
                }
#endif
                lease.Release();
                const ForwardingProvenance provenance{ ForwardingHolderKind::StackSlot, this, &obj };
                return FindToVersion(obj).GetOrFailClosed(
                    "WCollector::TryForwardObject.phase", provenance);
            }
#if defined(MRT_GC_UNIT_TESTS)
            if (g_routeLookupTestContext != nullptr) {
                g_routeLookupTestContext->retainedPhaseAllowed = true;
            }
#endif
            BaseObject* toVersion = ForwardObjectImpl(obj, region, lease);
            lease.Release();
#if defined(MRT_GC_UNIT_TESTS)
            // The existing plan-only bridge intercepts ForwardObjectImpl before
            // copying; its miss is an admission observation, not allocation failure.
            if (g_routeLookupTestContext != nullptr) return toVersion;
#endif
            return toVersion != nullptr ? toVersion : WaitForPageForwarding(obj, lease.HoldForwarding());
        }
        // ZGC's relocate_object (zRelocate.cpp:362-393) calls forward_object
        // after retain_page refuses; it never retries the retain with sched_yield.
        // Our n<0 refusal is immediate (the caller may already hold an outer pin),
        // so a table miss is allowed here. Returning null makes ForwardRegion's
        // receipt audit keep the page instead of spinning outside a safepoint.
        const ForwardingProvenance provenance{ ForwardingHolderKind::StackSlot, this, &obj };
        return WaitForPageForwarding(obj, lease.HoldForwarding());
    }
    // ZRelocate::relocate_object ends *every* path that did not itself produce a to-address with
    // ZRelocate::forward_object -- one last read of the forwarding table (zRelocate.cpp:382-410,
    // the trailing `return forward_object(forwarding, from_addr)` at :409).  It has to: the page
    // may have been relocated in place while we were asking, and in-place relocation is what
    // populates the table.
    //
    // RegionManager::RouteRegion returning false is exactly that case.  It answers false in two
    // structurally different ways (RegionManager.h:806-825): the page is already COMPACTED, or
    // RouteOrCompactRegionImpl just compacted it in place and set COMPACTED.  Neither means "no
    // to-version"; both mean "the to-version is an identity receipt published from the livemap by
    // the in-place compaction" (RegionManager.cpp:2151-2199 PublishKeptInPlaceReceipts).  The
    // IsCompacted() test above cannot cover it -- it runs *before* this call, and this call is
    // what makes the page compacted.
    //
    // RegionManager::ComputeRoute already spells the predicate as
    // `RouteRegion(r) || r->IsCompacted()` (RegionManager.h:1012); this consumer read only the
    // first half, so a root naming a live object on a compacted-in-place page was refused with the
    // answer sitting in the table.  Observed: NW256/256MB 3/3 abort at
    // Mutator::GCPhasePreForward.forward_object with route=COMPACTED marked=1 inRange=1.
    if (region->IsCompacted()) {
        // A miss here is not "unset": RouteRegion answered false because this call is what
        // compacted the page in place.  Which of the three compacted-miss cases it is comes from
        // the page geometry, not from the fact that the lookup missed (see ClassifyCompactedMiss).
        switch (ClassifyCompactedMiss(region, obj)) {
            case CompactedMissClass::kAlreadyToStart:
            case CompactedMissClass::kAlreadyToInterior:
                return obj;
            case CompactedMissClass::kReceiptOwed:
                return FindToVersion(obj).GetOrFailClosed(
                    "WCollector::TryForwardObject.compact-in-place",
                    ForwardingProvenance{ ForwardingHolderKind::StackSlot, this, &obj });
            case CompactedMissClass::kAbandonedTail:
                return FindToVersion(obj).GetOrFailClosed(
                    "WCollector::TryForwardObject.compact-in-place",
                    ForwardingProvenance{ ForwardingHolderKind::StackSlot, this, &obj });
        }
    }
    // Not routed and not compacted: RouteRegion took its ghost soft-miss return
    // (RegionManager.h:796-805), i.e. the ghost bit was cleared under us and this page is no
    // longer in the route domain at all.  ZGC's counterpart is ZForwardingTable::get == NULL
    // (zForwardingTable.inline.hpp:36-46): the page was never selected.
    return WaitForPageForwarding(obj, ForwardingTable::RetainPageOwner(region));
}

#if defined(MRT_GC_UNIT_TESTS)
WCollector::RouteLookupTestResult WCollector::RouteLookupForTest(BaseObject* fromObj)
{
    RouteLookupTestResult result;
    struct ContextScope {
        WCollector::RouteLookupTestResult*& slot;
        WCollector::RouteLookupTestResult* previous;
        explicit ContextScope(WCollector::RouteLookupTestResult*& context,
                              WCollector::RouteLookupTestResult* current)
            : slot(context), previous(context)
        {
            slot = current;
        }
        ~ContextScope() { slot = previous; }
    } scope(g_routeLookupTestContext, &result);
    (void)TryForwardObject(fromObj);
    return result;
}
#endif

BaseObject* WCollector::ForwardObjectImpl(BaseObject* obj, RegionInfo* ghostFromRegion,
                                          const RegionInfo::RetainScope& lease)
{
    if (!lease.covers(ghostFromRegion)) {
        const MAddress fromAddr = reinterpret_cast<MAddress>(obj);
        if (const MAddress hit = ForwardingTable::FindTo(fromAddr)) {
            return reinterpret_cast<BaseObject*>(hit);
        }
        return WaitForPageForwarding(obj, lease.HoldForwarding());
    }
#if defined(MRT_TESTABLE_INTERNALS)
    RunRemapWindowTestHook(8, ghostFromRegion, obj);
#endif
    CHECK(GetGCPhase() == GCPhase::GC_PHASE_PREFORWARD || GetGCPhase() == GCPhase::GC_PHASE_FORWARD);
#if defined(MRT_GC_UNIT_TESTS)
    if (g_routeLookupTestContext != nullptr) {
        g_routeLookupTestContext->plan = RoutePlan{ nullptr };
        g_routeLookupTestContext->hookReached = true;
        return nullptr;
    }
#endif
    // zRelocate.cpp:382-410 relocate_object: find hit → return; else retain
    // already held by the caller lease; inner allocate→copy→insert (CAS
    // winner). No object-header TryLock admission.
    const MAddress fromAddr = reinterpret_cast<MAddress>(obj);
    if (const MAddress hit = ForwardingTable::FindTo(fromAddr)) {
        return reinterpret_cast<BaseObject*>(hit);
    }
    if (obj->IsForwarded()) {
        auto toObj = GetForwardPointer(obj, ghostFromRegion);
        if (toObj != nullptr) {
            return toObj;
        }
    }
    RegionInfo* page = ghostFromRegion;
    if (page == nullptr) {
        page = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
        if (page == nullptr) {
            page = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        }
    }
    return RelocateObjectInner(obj, nullptr, page);
}

BaseObject* WCollector::ForwardObjectExclusive(BaseObject* obj)
{
    RegionInfo* page = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (page == nullptr) {
        page = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    }
    if (page == nullptr) {
        return nullptr;
    }
    return RelocateObjectInner(obj, nullptr, page);
}

BaseObject* WCollector::RelocateObjectInner(BaseObject* obj, BaseObject* planned, RegionInfo* copyPage)
{
    const MAddress fromAddr = reinterpret_cast<MAddress>(obj);
    if (const MAddress hit = ForwardingTable::FindTo(fromAddr)) {
        return reinterpret_cast<BaseObject*>(hit);
    }
    if (!Collector::PlausibleManagedObjectGate("WCollector::RelocateObjectInner", obj)) {
        return nullptr;
    }
    BaseObject* toObj = planned;
    const size_t size = RegionSpace::GetAllocSize(*obj);
    bool allocatedHere = false;
    if (toObj == nullptr) {
        AllocBuffer* buf = AllocBuffer::GetOrCreateAllocBuffer();
        RegionInfo* tl = buf->GetRegion();
        if (tl != nullptr && tl != RegionInfo::NullRegion()) {
            toObj = reinterpret_cast<BaseObject*>(tl->Alloc(size));
        }
        if (toObj == nullptr) {
            toObj = reinterpret_cast<BaseObject*>(
                buf->Allocate(size, AllocType::MOVEABLE_OBJECT));
        }
        if (toObj == nullptr) {
            return nullptr;
        }
        allocatedHere = true;
    }
    BaseObject* result = nullptr;
    ForwardingTable::Publication publication = ForwardingTable::EnsurePublicationBeforeCopy(
        copyPage, reinterpret_cast<MAddress>(obj));
    if (publication) {
        DLOG(FORWARD, "forward obj %p<%p>(%zu) to %p", obj, obj->GetTypeInfo(), size, toObj);
        CopyObject(*obj, *toObj, size);
        if (toObj != obj) {
            toObj->SetStateCode(ObjectState::NORMAL);
        }
        std::atomic_thread_fence(std::memory_order_release);
        if (toObj == obj || ToHeaderCovered(toObj)) {
            const ZForwarding::Receipt receipt = ForwardingTable::InstallMapping(
                publication, reinterpret_cast<MAddress>(obj), reinterpret_cast<MAddress>(toObj));
            const MAddress mapped = receipt.address;
            if (ForwardingTable::ReceiptAllowsForwarded(mapped)) {
                if (MutatorRelocate::StatsOn()) {
                    MutatorRelocate::Role role = MutatorRelocate::Role::MUTATOR;
                    if (IsGcThread()) {
                        role = MutatorRelocate::Role::GC;
                    } else if (IsRuntimeThread()) {
                        role = MutatorRelocate::Role::OTHER_RT;
                    }
                    MutatorRelocate::NoteAnyCopy(role);
                    if (MutatorRelocate::InScope()) {
                        MutatorRelocate::NoteSelfCopy(size, role);
                    }
                }
                obj->SetStateCode(ObjectState::FORWARDED);
#if defined(MRT_TESTABLE_INTERNALS)
                RunRemapWindowTestHook(6, copyPage, obj);
#endif
                result = reinterpret_cast<BaseObject*>(mapped);
            }
        }
    } else {
        const MAddress pageStart = copyPage == nullptr ? 0 : copyPage->GetRegionStart();
        const uint64_t entries = ForwardingTable::GetEntries(fromAddr) == nullptr ? 0 : 1;
        LOG(RTLOG_ERROR,
            "[GCV2][first-visitor] publication refused obj=%p page=%p pageStart=%#zx entries=%llu route=%u done=%u ref=%d",
            obj, copyPage, static_cast<size_t>(pageStart), static_cast<unsigned long long>(entries),
            copyPage == nullptr ? 0U : static_cast<unsigned>(copyPage->GetRouteState()),
            copyPage == nullptr ? 0U : static_cast<unsigned>(copyPage->IsForwardingDone()),
            copyPage == nullptr ? 0 : copyPage->ForwardingRefCount());
    }
    if (allocatedHere && toObj != obj && result != toObj) {
        if (RegionInfo* dest = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(toObj))) {
            (void)dest->UndoAllocObjectAtomic(reinterpret_cast<uintptr_t>(toObj), size);
        }
    }
    return result;
}

BaseObject* WCollector::ForwardObjectExclusive(BaseObject* obj, BaseObject* toObj, RegionInfo* copyPage)
{
    return RelocateObjectInner(obj, toObj, copyPage);
}
} // namespace MapleRuntime
