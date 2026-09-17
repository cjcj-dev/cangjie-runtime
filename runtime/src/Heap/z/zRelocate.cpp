// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zAbort.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zJNICritical.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/WCollector/WCollector.h"

#include <array>
#include <atomic>
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

#include "Concurrency/Concurrency.h"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zThreadLocalData.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Common/SuspendibleThreadSet.h"
#include "Heap/z/zUncoloredRoot.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "Heap/z/zStackWatermark.hpp"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zRemembered.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/WCollector/WCollectorInternal.h"

#include "Heap/z/zPageAllocator.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/Collector/CopyCollector.h"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zUtils.inline.hpp"
#include "Heap/z/zArray.inline.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"


namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
void NoteRawRemapYoungRootsTestReceipt(ObjectRef& root, uintptr_t before);
void NoteRemapYoungRootsTestReceipt(RefField<>& field, uintptr_t before, bool healed,
                                           bool storeGoodAfter);
#endif



namespace WCollectorInternal {
} // namespace WCollectorInternal
// installdomain: positive control — how often Resolve/Fix would install a ghost-from that is
// outside GetRoute's liveInfo0 survivor domain. Grant paints that bit before route geometry.
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
    // instead of using a pointer that this race can leave null. Heap::page
    // itself CHECKs (early-stop) on a genuine no-owner invariant break.
    ZPage* regionInfo = ZPage::GetGhostFromRegionAt(reinterpret_cast<uintptr_t>(obj));
    if (regionInfo == nullptr) {
        regionInfo = Heap::page(reinterpret_cast<uintptr_t>(obj));
    }
    return regionInfo->IsUnmovableFromRegion();
}

void WCollector::CheckStoreGoodTarget(const char* consumer, BaseObject* target,
                                      const ForwardingProvenance& provenance) const
{
    // zAddress.inline.hpp:store_good consumes an already current address.
    // The originating load/root operation performed generation-specific remap.
    (void)consumer;
    (void)ValidateCurrentValue(target, provenance);
}

template<bool forward>
bool WCollector::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                       BaseObject*& toObj, const ForwardingProvenance& provenance) const
{
    RefField<> oldRef(field);
    if (IsLoadBad(oldRef)) {
        fromObj = to_object(oldRef.GetTargetObject());
        if (forward) {
            toObj = const_cast<WCollector*>(this)->relocate_or_remap_object(
                fromObj, static_cast<ZGenerationId>(remap_generation(oldRef)));
        } else {
            toObj = FindToVersion(fromObj, static_cast<Generation>(remap_generation(oldRef))).GetOrFailClosed(
                "WCollector::TryUpdateRefFieldImpl", provenance);
        }
        if (toObj == nullptr) {
            return false;
        }
        // R7：写回必须经规范色单产地，禁 plain RefField<>(toObj)。
        // expected 仍是 observed-raw（oldRef.GetFieldValue()）；模板 = GetAndTryTagRefField。
        RefField<> tmpField = GetAndTryTagRefField(toObj);
        if (field.CompareExchange(oldRef.GetFieldValue(), tmpField.GetFieldValue())) {
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
        const bool isValidTarget = target->IsValidObject();
        // Anchor main 2f1bc8355e92dbf01c063050b5c9a2947c711d64
        CHECK_DETAIL(isValidTarget, "TryUntagRefField encounters invalid tagged target %p at field %p", target,
                     &field);
        // TRUST_STATE_KILL_PLAN Phase 1: API retained, but HeapSlot write-back is current colour
        // (not plain). Read path no longer calls this; residual callers must not re-install trust.
        RefField<> newRef = GetAndTryTagRefField(target);
        if (field.CompareExchange(oldRef.GetFieldValue(), newRef.GetFieldValue())) {
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

void WCollector::RemapYoungRoots()
{
    SuspendibleThreadSetJoiner joiner;
    MRT_PHASE_TIMER(ZStatPhases::PRemapYoungRoots);
    // zGeneration.cpp:1483-1523: remembered fields, all colored roots, then threads.
    ZRemsetTableIterator remsetIter(&Heap::GetHeap().remembered(), false);
    Heap::GetHeap().remembered().remap_current(&remsetIter);
    VisitAllColoredRoots([](NativeSlot& root) { (void)ZBarrier::ReadStaticRef(root); });
    RootVisitor visitor = [this](ObjectRef& root) {
        const zaddress_unsafe observed = root.LoadPlain();
        // ZGeneration::remap_object (zGeneration.inline.hpp:142-151): only
        // the selected generation's forwarding table qualifies this root.
        // Old relocation may already have installed its table before this
        // young-remap pass. Conversely a promoted source can still belong
        // to the young table, so the page's current generation is not a gate.
        const MAddress observedAddr = raw(observed);
        if (observedAddr != 0 &&
            generation_forwarding_table(Generation::Young).get(observedAddr) != nullptr) {
            const ZGenerationId id = ZGenerationId::young;
            (void)relocate_or_remap_object(to_object(safe(observed)), id);
            ZUncoloredRoot::process_no_keepalive(reinterpret_cast<zaddress_unsafe*>(&root), ZPointerLoadGoodMask);
        }
#if defined(MRT_TESTABLE_INTERNALS)
        NoteRawRemapYoungRootsTestReceipt(root, raw(observed));
#endif
    };
    VisitStrongPlainRoots(visitor, [&](Mutator& mutator) {
        // Cangjie stack maps may name stack objects/headerless records. Expand
        // their plain fields before remapping, as verification and mark do.
        // ZGC zStackWatermark.cpp:164-214 processes each oop frame slot.
        RootVisitor heapRoots = [&](ObjectRef& root) {
            mutator.VisitHeapRootSlots(root, visitor);
        };
        DerivedPtrVisitor derived = Mutator::MakeDerivedRootVisitor(visitor);
        size_t frames = 0;
        if (!StackWatermarkSet::finish_processing(mutator, heapRoots, heapRoots,
                __atomic_load_n(ZPointerStoreGoodMaskLowOrderBitsAddr, __ATOMIC_ACQUIRE), &derived, frames)) {
            mutator.VisitHeapReferences(heapRoots, derived);
        }
    });
}





void WCollector::PreforwardDiscoveredExternObjects(Generation generation)
{
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CHECK(discoveredExternObjects.empty());
    CurrentizeValueRootMap(cycleRefWorkStack, generation);
}

void WCollector::PreforwardAllResurrectExportFromObjects(Generation generation)
{
    std::lock_guard<std::mutex> lg(resurrectExportMtx);
    CurrentizeValueRootSet(resurrectedExportObjectes, generation);
    CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, generation);
}
void WCollector::StartRelocationTasks(GCCycleGeneration generation)
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    ZWorkers& workers = GetWorkers(generation);
    if (generation == GCCycleGeneration::YOUNG) manager.StartForwardFromRegions<Generation::Young>(workers);
    else manager.StartForwardFromRegions<Generation::Old>(workers);
}

bool WCollector::Preforward()
{
    ScopedEntryTrace trace("CJRT_GC_PREFORWARD");
    MRT_PHASE_TIMER(ZStatPhases::PPreforward);
    {
        DriverLocker locker(collectorResources);
        // zGeneration.cpp:1054-1063: remap under the driver lock before pausing.
        RemapYoungRoots();
        if (ZAbort::should_abort()) {
            return false;
        }
        // OpenJDK zGeneration.cpp:1175-1200: isolate pause_relocate_start from the
        // concurrent root-preforward work below. ScopedLightSync emits its matching
        // rec=stw record, including rendezvous and held time.
        ZJNICritical::block();
        ScopedLightSync scopedLightSync("Preforward", true, GCPhase::GC_PHASE_PREFORWARD);
        ZVerify::BeforeZOperation();
        // GCLOG samples pause/concurrent kind when the timer is constructed, so enter
        // ScopedLightSync first. Destruction order also closes this timer before mutators
        // resume, keeping the whole phase in the pause account.
        MRT_PHASE_TIMER(ZStatPhases::POldRelocateStart);
        ThreadGCData::VisitOwners([](ThreadGCData& data, Mutator*, ThreadLocalData*) {
            data.storeBarrierBuffer->install_base_pointers();
        });
        ZGlobalsPointers::flip_old_relocate_start();
        ZVerify::OnColorFlip();
        StartRelocationTasks(GCCycleGeneration::OLD);
        ZJNICritical::unblock();
    }

    RegionManager& manager = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    manager.DrainForwardFromRegions<Generation::Old>();
    ZWorkers& workers = GetWorkers(GCCycleGeneration::OLD);
    const std::function<void()> families[] = {
        [&] { VisitAllColoredRoots([](NativeSlot& root) { (void)ZBarrier::ReadStaticRef(root); }); },
        [&] { VisitStrongPlainRoots([this](ObjectRef& root) {
            const zaddress_unsafe observed = root.LoadPlain();
            BaseObject* oldObj = to_object(safe(observed));
            if (oldObj != nullptr && Heap::IsHeapAddress(oldObj)) {
                (void)relocate_or_remap_object(oldObj, ZGenerationId::old);
            }
            ZUncoloredRoot::process_no_keepalive(reinterpret_cast<zaddress_unsafe*>(&root), ZPointerLoadGoodMask);
        }, {}); },
        [&] { PreforwardDiscoveredExternObjects(Generation::Old); },
        [&] { PreforwardAllResurrectExportFromObjects(Generation::Old); }
    };
    // zArray.hpp:104 ZArrayParallelIterator: workers claim root families.
    class RootsTask final : public ZTask {
    public:
        RootsTask(const std::function<void()>* families, size_t count)
            : ZTask("ZRelocateRootsTask"), iter(families, count) {}
        void work() override
        {
            for (std::function<void()> family; iter.next(&family);) {
                family();
            }
        }
    private:
        ZArrayParallelIterator<std::function<void()>> iter;
    } roots(families, sizeof(families) / sizeof(families[0]));
    {
        SuspendibleThreadSetJoiner joiner;
        workers.run(&roots);
    }
    return true;
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
//       same livemap the carrier points at — visible to GetRoute.
// After ROUTED, liveByteCount/geometry are frozen — do not paint (tooLate counter).
// Livemap the route reads for this region: the from-page carrier's map when
// one is published, otherwise the page's own (zForwarding.hpp:44-110 keeps
// the whole from ZPage; only its livemap is retained here).
static ZLiveMap* RouteLiveMap(ZPage* region, ZGenerationId& id)
{
    const ZForwarding::FromPageView* from = region->GetFromPageView();
    if (from != nullptr) {
        id = static_cast<Generation>(from->owner) == Generation::Young ? ZGenerationId::young
                                                                       : ZGenerationId::old;
        return from->livemap;
    }
    id = region->generation_id();
    return &region->livemap();
}

void EnsureRouteDomainMembership(WCollector* collector, BaseObject* obj)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
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
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(obj));
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
    const zaddress addr = from_object(obj);
    bool alreadyInDomain = false;
    if (isGhost) {
        alreadyInDomain = region->IsRouteSurvivedObject(offset);
    } else {
        // Prefer ghost face when present (what GetRoute reads); else the page's own livemap.
        ZGenerationId id;
        ZLiveMap* face = RouteLiveMap(region, id);
        alreadyInDomain = face != nullptr && face->get(id, region->bit_index(addr));
    }
    if (alreadyInDomain) {
        g_installDomainAlready.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (isGhost) {
        // Only paint while FORWARDABLE: RelocateClaimedPage freezes liveByteCount.
        if (region->IsForwardingDone() || region->IsRoutingState()) {
            g_installDomainTooLate.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // Mark the current livemap (post-snapshot: same map as the carrier's when non-null).
    (void)collector->MarkObject(obj);
    // If ghost face was null (snapshot of empty livemap), bind freshly allocated livemap
    // so GetRoute's from-livemap gate opens on the bits we just painted.
    if (isGhost) {
        region->BindFromPageLiveMapIfNull();
    }
    ZLiveMap* live = &region->livemap();
    ZLiveMap* ghost = region->FromPageLiveMap();
    if (ghost != nullptr && ghost != live) {
        // MarkObject already maintained live bytes on the live face; ghost paint is
        // domain-visible bits only (do not double-count).
        ZGenerationId id;
        (void)RouteLiveMap(region, id);
        bool incLive = false;
        (void)ghost->set(id, region->bit_index(addr), false, incLive);
    }
    // Re-check: grant only counts if GetRoute face now accepts (positive control truth).
    ghost = region->FromPageLiveMap();
    if (isGhost) {
        if (ghost != nullptr && region->IsRouteSurvivedObject(offset)) {
            g_installDomainGrant.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_installDomainTooLate.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // pre-snapshot from: paint lands on the page livemap; PrepareForwardable will copy pointer.
        g_installDomainGrant.fetch_add(1, std::memory_order_relaxed);
    }
}

// statresid: force ghost livemap paint while still FORWARDABLE (before any Route
// freezes geometry). Used by the root grant pass and as last-chance before Forward.
// Returns true when AdmitForRoute would accept `obj` after the paint attempt.
bool ForceRootRouteDomainWhileForwardable(WCollector* collector, BaseObject* obj)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return false;
    }
    EnsureRouteDomainMembership(collector, obj);
    ZPage* region = ZPage::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
        region = Heap::page(reinterpret_cast<MAddress>(obj));
    }
    if (region == nullptr || !region->IsYoungRegion()) {
        return false;
    }
    size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(obj));
    // Only paint while FORWARDABLE — after ROUTING/ROUTED/COMPACTED liveByteCount is
    // frozen (S2); late marking would desync Admit from geometry.
    if (region->IsForwardingDone() || region->IsRoutingState()) {
        return region->FromPageLiveMap() != nullptr && region->IsRouteSurvivedObject(offset);
    }
    (void)collector->MarkObject(obj);
    region->BindFromPageLiveMapIfNull();
    ZLiveMap* g0 = region->FromPageLiveMap();
    if (g0 != nullptr && !region->IsRouteSurvivedObject(offset)) {
        // MarkObject above already counted live bytes when first paint on live.
        // Ghost-only paint must not double-count (FYS0 OverflowException risk).
        ZGenerationId id;
        (void)RouteLiveMap(region, id);
        bool incLive = false;
        (void)g0->set(id, region->bit_index(from_object(obj)), false, incLive);
    }
    g0 = region->FromPageLiveMap();
    return g0 != nullptr && region->IsRouteSurvivedObject(offset);
}
} // namespace

// Install a logical resolved target into a heap field. Callers cannot supply a
// pre-encoded RefField: this controlled entry applies the current heap colour here.
// On CAS fail, accept the peer's update (major TryUpdateRefFieldImpl shape).
bool WCollector::CasInstallResolvedTarget(RefField<>& field, MAddress expected, zaddress target,
                                          bool allowNull) const
{
    BaseObject* object = to_object(target);
    if (object != nullptr) {
        CHECK_DETAIL(Heap::IsHeapAddress(object),
                     "resolved heal target must be a heap address target=%p", object);
        CHECK_DETAIL(Collector::JudgeHandOutTarget(object) == HandVerdict::Usable,
                     "resolved heal target must be usable target=%p", object);
    }
    zpointer desired = is_null(target) ? zpointer::null : RefField<>(ZAddress::store_good(target)).GetFieldValue();
    if (expected == raw(desired)) {
        return true;
    }
    const zpointer observed = to_zpointer(expected);
    auto loadGood = [this](zpointer value) {
        RefField<> probe(value);
        return is_null(probe.GetTargetObject()) || ZPointer::is_load_good(probe.GetFieldValue());
    };
    if (loadGood(observed)) {
        return true;
    }
    ZBarrier::self_heal(ZBarrier::is_load_good_or_null_fast_path,
                        reinterpret_cast<volatile zpointer*>(&field), observed, desired,
                        allowNull);
    const bool healed = true;
    if (healed) {
        g_minorRefCasOk.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_minorRefCasFail.fetch_add(1, std::memory_order_relaxed);
    return true;
}

BaseObject* WCollector::ResolveMinorReference(RefField<>& field, const ScopedStopTheWorld* stw) const
{
    (void)stw;

    RefField<> observed(field);
    BaseObject* from = to_object(observed.GetTargetObject());
    if (from == nullptr || !Heap::IsHeapAddress(from)) {
        return from;
    }

    // zBarrier.inline.hpp:294-343: both old-colour and apparently current
    // references pass through make-load-good before the concrete slot is
    // healed. Colour alone is not forwarding provenance.
    const ForwardingProvenance provenance{ ForwardingHolderKind::Remset, nullptr, &field };
    BaseObject* resolved = make_load_good(observed, provenance);
    CHECK_DETAIL(resolved != nullptr && Heap::IsHeapAddress(resolved),
                 "minor resolve requires a heap to-address from=%p", from);
    CHECK_DETAIL(Collector::JudgeHandOutTarget(resolved) == HandVerdict::Usable,
                 "minor resolve requires a usable target from=%p resolved=%p", from, resolved);

    (void)CasInstallResolvedTarget(field, raw(observed.GetFieldValue()), from_object(resolved), false);
    return resolved;
}
BaseObject* WCollector::ResolveMinorReference(RootSlot& root, const ScopedStopTheWorld* stw) const
{
    (void)stw;
    zaddress_unsafe observed = root.LoadPlain();
    BaseObject* from = to_object(safe(observed));
    if (from == nullptr || !Heap::IsHeapAddress(from)) {
        return from;
    }

    // ZUncoloredRootProcessOopClosure applies the load barrier and writes the
    // resolved address back uncolored (zGeneration.cpp:1458-1523).
    const ForwardingProvenance provenance{ ForwardingHolderKind::StackSlot, this, &root };
    BaseObject* resolved = ResolveStoreValue(from, provenance, Generation::Young);
    CHECK_DETAIL(resolved != nullptr && Heap::IsHeapAddress(resolved),
                 "minor root resolve requires a heap to-address from=%p", from);
    CHECK_DETAIL(Collector::JudgeHandOutTarget(resolved) == HandVerdict::Usable,
                 "minor root resolve requires a usable target from=%p resolved=%p", from, resolved);

    ZUncoloredRoot::process_no_keepalive(reinterpret_cast<zaddress_unsafe*>(&root), ZPointerLoadGoodMask);
    return resolved;
}
bool WCollector::FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase,
                                      const ScopedStopTheWorld* stw) const
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
        ZPage* targetRegion = Heap::page(targetAddress);
        ZPage* baseRegion = Heap::page(baseAddress);
        const bool baseValid = targetAddress > baseAddress && targetRegion == baseRegion;
        if (!baseValid) {
            return false;
        }
        const size_t offset = static_cast<size_t>(targetAddress - baseAddress);
        if (offset >= RegionSpace::GetAllocSize(*knownBase)) {
            return false;
        }
        const ForwardingProvenance provenance{ ForwardingHolderKind::Derived, knownBase, &field };
        BaseObject* resolvedBase = ResolveStoreValue(knownBase, provenance, Generation::Young);
        CHECK_DETAIL(resolvedBase != nullptr && Heap::IsHeapAddress(resolvedBase) &&
                         Collector::JudgeHandOutTarget(resolvedBase) == HandVerdict::Usable,
                     "derived heal requires a resolved base base=%p resolved=%p offset=%zu",
                     knownBase, resolvedBase, offset);
        const MAddress oldVal = raw(oldField.GetFieldValue());
        const MAddress interiorAddress = reinterpret_cast<MAddress>(resolvedBase) + offset;
        if (oldVal != interiorAddress) {
            (void)CasInstallInteriorColoured(field, to_zpointer(oldVal), resolvedBase, offset);
        }
        return true;
    }
    BaseObject* target = ResolveMinorReference(field, stw);
    // Static / RO slots may hold non-heap objects (never evacuated). Colouring them
    // changes the bit pattern so equal-skip misses, then CAS faults on RELRO.
    // Same heap gate as FindToVersion.
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    // h3seed2/3 乙 residual: live holder field still points at a region that minor
    // already CollectRegion'd (ClearUnits). Prefer silent null over UAF; CAS so
    // concurrent fix peers can win. Pre-evac H3 samples the prior cycle's residue —
    // nulling here clears it before the next VERIFY_HEAP inventory.
    // Criterion: ZPage::IsFreeRegion|IsGarbageRegion at this Fix call (file:line).
    if (ScrubMinorFreeTarget(field, target, true)) {
        return true;
    }
    HeapSlot<> oldBits(oldField);
    BaseObject* oldObj = to_object(oldBits.GetTargetObject());
    // resolveto: Resolve already rewrote FROM→TO. TO sits in a Compacted ghost
    // (in-place pack). Forward/Admit indexes liveInfo0 by from-offset — feeding TO
    // misses → leave-alone. Keep the already-installed to.
    ZPage* targetRegion = ZPage::GetGhostFromRegionAt(reinterpret_cast<MAddress>(target));
    const bool compactDestination = targetRegion != nullptr &&
        targetRegion->IsCompactRouteDestination(reinterpret_cast<MAddress>(target));
    const bool alreadyTo = (target != oldObj) || compactDestination;
    BaseObject* current = target;
    const bool hasForwardingFace = targetRegion != nullptr && targetRegion->FromPageLiveMap() != nullptr;
    if (!alreadyTo && hasForwardingFace && IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
        // installdomain: route-domain grant before ForwardObject → GetRoute.
        EnsureRouteDomainMembership(const_cast<WCollector*>(this), target);
        current = const_cast<WCollector*>(this)->ForwardObject(target, Generation::Young);
    }
    // ForwardObject null = movable ghost with no to-version (survivor-gate miss).
    // Drop the edge; do not reinstall the from address that is about to be reclaimed.
    if (current == nullptr) {
        // zBarrier.inline.hpp:294-343 never publishes a null substitute for a
        // non-null reference whose forwarding lookup missed. The current thread
        // must finish relocation (or fail closed); a null CAS is not a
        // substitute for an unresolved product.
        (void)field.CompareExchange(field.GetFieldValue(), zpointer::null);
        Collector::FailClosedLoad(
            "WCollector::FixMinorEvacuatedSlot.unresolved", target,
            static_cast<uintptr_t>(raw(field.GetFieldValue())),
            ForwardingProvenance{ ForwardingHolderKind::Remset, nullptr, &field });
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
    if (field.CompareExchange(to_zpointer(oldVal), to_zpointer(newVal))) {
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
    BaseObject* target = ResolveMinorReference(root, stw);
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    BaseObject* oldObj = to_object(to_zaddress(oldValue));
    // resolveto: Resolve already remapped FROM→TO. Do not Admit the to-address
    // against the from-offset bitmap (offpast same-target probe: sameObj=0).
    ZPage* targetRegion = ZPage::GetGhostFromRegionAt(reinterpret_cast<MAddress>(target));
    const bool compactDestination = targetRegion != nullptr &&
        targetRegion->IsCompactRouteDestination(reinterpret_cast<MAddress>(target));
    const bool alreadyTo = (target != oldObj) || compactDestination;
    BaseObject* current = target;
    const bool hasForwardingFace = targetRegion != nullptr && targetRegion->FromPageLiveMap() != nullptr;
    if (!alreadyTo && hasForwardingFace && IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
        // Last-chance domain paint while FORWARDABLE (grant pass covers the bulk case;
        // this catches roots dirtied after the grant pass or parallel races).
        (void)ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), target);
        current = const_cast<WCollector*>(this)->ForwardObject(target, Generation::Young);
        // Third disposition (statresid): if still null and region still FORWARDABLE,
        // force-paint once more and retry Forward — never HealRoot(null), never leave
        // a reclaimable from named by a live root without a second attempt.
        if (current == nullptr) {
            if (ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), target)) {
                current = const_cast<WCollector*>(this)->ForwardObject(target, Generation::Young);
            }
        }
    }
    if (current == nullptr) {

        // I2: Forward miss still consults FindToVersion/receipt. Stale miss
        // refuses silently leaving from (seqnum-bounded table already rejects
        // expired entries). ⛔ Do not reinstall from; ⛔ do not StorePlain(null).
        const ForwardingProvenance provenance{ ForwardingHolderKind::StackSlot, this, &root };
        BaseObject* viaTable = FindToVersion(target, Generation::Young).GetOrFailClosed(
            "WCollector::FixMinorEvacuatedSlot", provenance);
        if (viaTable != nullptr && viaTable != target && Heap::IsHeapAddress(viaTable) &&
            viaTable->IsValidObject()) {
            ZUncoloredRoot::process_no_keepalive(reinterpret_cast<zaddress_unsafe*>(&root), ZPointerLoadGoodMask);
            return true;
        }
        Collector::FailClosedLoad(
            "WCollector::FixMinorEvacuatedSlot.unresolved", target,
            reinterpret_cast<uintptr_t>(&root),
            ForwardingProvenance{ ForwardingHolderKind::StackSlot, this, &root });
    }
    MAddress newValue = reinterpret_cast<MAddress>(current);
    if (oldValue == newValue && raw(root.LoadPlain()) == newValue) {
        return false;
    }
    ZUncoloredRoot::process_no_keepalive(reinterpret_cast<zaddress_unsafe*>(&root), ZPointerLoadGoodMask);
    return true;
}

bool WCollector::FixMinorEvacuatedSlot(DerivedSlot& derived, BaseObject* knownBase,
                                      const ScopedStopTheWorld* stw) const
{
    const zaddress_unsafe observed = derived.LoadDerived();
    if (knownBase == nullptr) {
        return false;
    }
    RootVisitor root = [this, stw](RootSlot& slot) { (void)FixMinorEvacuatedSlot(slot, stw); };
    auto closure = Mutator::MakeDerivedRootVisitor(root);
    closure(to_zaddress_unsafe(reinterpret_cast<MAddress>(knownBase)), derived);
    return raw(observed) != raw(derived.LoadDerived());
}

void WCollector::FixMinorRootSlots(const ScopedStopTheWorld* stw)
{
    // The phase handshake has already completed each stack watermark. Only
    // non-frame plain carriers and colored storage remain at this entry.
        RootVisitor rawRootVisitor = [this, stw](ObjectRef& root) {
        (void)FixMinorEvacuatedSlot(root, stw);
    };
    VisitStrongPlainRoots(rawRootVisitor, {});
    VisitAllColoredRoots([](NativeSlot& root) { (void)ZBarrier::ReadStaticRef(root); });

}

void WCollector::FixMinorObjectSlots(BaseObject* object, const ScopedStopTheWorld* stw)
{
    // secondclass ②: belt-and-braces — refuse null tip before HasRefField.
    if (object == nullptr || !object->IsValidObject()) {
        return;
    }
    if (!object->HasRefField()) {
        return;
    }
    // eatarm brackets the host so an IOR can be attributed to the object being fixed;
    // nullgate names the edge inside. Both are gated and neither subsumes the other.

    ZIterator::basic_oop_iterate_safe(object, [this, object, stw](RefField<>& field) {
        (void)FixMinorEvacuatedSlot(field, nullptr, stw);
    });

}

void WCollector::EvacuateYoungRegions(const std::vector<BaseObject*>& reachableVec,
                                       const MinorSlotSet& rememberedSlots,
                                       bool refFixSlotsCoveredByReachable,
                                       const MinorInteriorBaseMap& interiorBases,
                                       std::unique_ptr<ScopedStopTheWorld>* stw)
{
    RegionManager& manager = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    (void)reachableVec;
    (void)refFixSlotsCoveredByReachable;
    // ZGC Phase 7/8 (zGeneration.cpp:573-580, 918-931, 850-853): pause_relocate_start
    // is flip + set_phase(Relocate) + _relocate.start(); object copy is concurrent.
    // Flip is the trap that makes mutator loads take the self-heal / relocate_object
    // path; without it, concurrent copy is the empty window concreffix measured.
    auto liveStw = [stw]() -> const ScopedStopTheWorld* {
        return (stw != nullptr && *stw != nullptr) ? stw->get() : nullptr;
    };
    const bool doYoungFlip = true;
    ZWorkers& workers = GetWorkers(GCCycleGeneration::YOUNG);

    std::vector<MAddress> remsetVec;
    remsetVec.assign(rememberedSlots.begin(), rememberedSlots.end());

    // zRemembered.cpp:remap_current visits remembered slots. Stack completion
    // belongs to the phase watermark; it does not require a reachable-heap sweep.
    auto remapRemembered = [&](ZWorkers& pool) {
        // zArray.hpp:104 ZArrayParallelIterator over the remembered slots.
        ZArrayParallelIterator<MAddress> slots(remsetVec.data(), remsetVec.size());
        class RememberedTask final : public ZTask {
        public:
            explicit RememberedTask(std::function<void()> body) : ZTask("ZRemapRememberedTask"), body(std::move(body)) {}
            void work() override { body(); }
        private:
            std::function<void()> body;
        } task([&] {
            for (MAddress slot; slots.next(&slot);) {
                if (!Heap::IsHeapAddress(slot)) {
                    continue;
                }
                auto known = interiorBases.find(slot);
                BaseObject* base = known == interiorBases.end() ? nullptr : known->second;
                (void)FixMinorEvacuatedSlot(HeapSlotAt<>(slot), base, liveStw());
            }
        });
        pool.run(&task);
    };

    // Earliest post-mark checkpoint: still before any fix/forward mutates refs.

    {
        // minortime: ⑦ ref fix (preforward roots + fixForwardedReferences)
        MRT_PHASE_TIMER(ZStatPhases::PYoungRefFix);

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
        // target really was forwarded, is not in an unmovable region, and the slot was load-good --
        // which after a flip can only mean it was written after that flip.
        //
        // Our own major path already has the ZGC order: PrepareForwardTable<Old> at :2533 runs
        // before flip_young/old_relocate_start at :2552-2553.  The two paths disagreed.
        {
            MRT_PHASE_TIMER(ZStatPhases::PYoungRefFixPrepare);

            // iorfix: PrepareForwardTable FIRST so liveInfo0 snapshots the closed mark
            // domain while every from region is still FORWARDABLE, THEN pass1 Fix/Forward.
            // Prior order let FixMinorRootSlots RouteRegion before the domain snapshot.
            TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true, true);
            fwdTable.PrepareForwardTable<Generation::Young>();
            // ZGenerationYoung::collect: last abortpoint after selection,
            // before relocate-start. Once flipped, finish every remaining page.
            if (ZAbort::should_abort()) {
                return;
            }
            // zGeneration.cpp:1503-1508: install forwarding then flip remap bits.
            // ZGC pause() wraps VMOp_ZRelocateStartYoung with JNICritical block
            // (zGeneration.cpp:475-483, block_jni_critical at :832).
            ZJNICritical::block();
            if (doYoungFlip) {
                ThreadGCData::VisitOwners([](ThreadGCData& data, Mutator*, ThreadLocalData*) {
                    data.storeBarrierBuffer->install_base_pointers();
                });
                ZGlobalsPointers::flip_young_relocate_start();
                ZVerify::OnColorFlip();
            }
            // Publish the relocate phase and submit page work while the
            // existing young pause still excludes mutator execution. Root
            // transition may now wait for a real page task on allocation failure.
            Heap::GetHeap().SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_PREFORWARD);
            StartRelocationTasks(GCCycleGeneration::YOUNG);
            ZJNICritical::unblock();
            // The pause publishes the work domain. Eager roots relocate one
            // object themselves; allocation failure uses the same in-place page
            // task on this thread (advisor 161024, compiler prerequisite #498).
            TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true, true);
        }

        // pass1 root fix after the domain snapshot.
        // pass1 is load-bearing for previous-gen residual (MINOR_CONCURRENCY §七 T-A).
        {
            MRT_PHASE_TIMER(ZStatPhases::PYoungRefFixRootPass1);
            FixMinorRootSlots(liveStw());
            PreforwardDiscoveredExternObjects(Generation::Young);
            PreforwardAllResurrectExportFromObjects(Generation::Young);
        }

        // Reset CAS counters for this fix window (positive-control visibility).
        g_minorRefCasFail.store(0, std::memory_order_relaxed);
        g_minorRefCasOk.store(0, std::memory_order_relaxed);

    }

    {
        TransitionToGCPhase(GCPhase::GC_PHASE_FORWARD, true, true);
        {
            stw->reset();
            MRT_PHASE_TIMER(ZStatPhases::PYoungConcurrentRelocate);
            VLOG(REPORT, "[GCV2][relocate][conc] concurrent_relocate start nObj=%zu flip=1",
                 reachableVec.size());
            ForwardFromSpace(GCCycleGeneration::YOUNG);
            *stw = std::make_unique<ScopedStopTheWorld>("young post-relocate", true,
                                                        GCPhase::GC_PHASE_FORWARD);
            ZVerify::BeforeZOperation();
            manager.FinishIncompleteFromRegions(GCCycleGeneration::YOUNG);
        }
        VLOG(REPORT, "[GCV2][relocate][conc] concurrent_relocate done; STW re-entered");
        {
            MRT_PHASE_TIMER(ZStatPhases::PYoungRefFixBulk);
            g_minorRefCasFail.store(0, std::memory_order_relaxed);
            g_minorRefCasOk.store(0, std::memory_order_relaxed);
            FixMinorRootSlots(liveStw());
            PreforwardDiscoveredExternObjects(Generation::Young);
            PreforwardAllResurrectExportFromObjects(Generation::Young);
            remsetVec.assign(rememberedSlots.begin(), rememberedSlots.end());
            {
                // ZGC immediately scans buffered entries that crossed the young
                // flip (zStoreBarrierBuffer.cpp:162-187). Publish all mutator
                // buffers before the active-face Snapshot used for this ref fix.
                (void)ZMark::FlushAllGenerations();
                std::unordered_set<MAddress> concRemset;
                ZRemsetTableIterator remsetIter(&Heap::GetHeap().remembered(), false);
                for (ZRemsetTableEntry entry; remsetIter.next(&entry);) {
                    if (entry._page == nullptr) {
                        continue;
                    }
                    entry._page->oops_do_current_remembered([&](volatile zpointer* p) {
                        concRemset.insert(reinterpret_cast<MAddress>(p));
                    });
                }
                remsetVec.reserve(remsetVec.size() + concRemset.size());
                for (MAddress slot : concRemset) {
                    remsetVec.push_back(slot);
                }
                VLOG(REPORT,
                     "[GCV2][relocate][conc_stw] remset pre=%zu conc_new=%zu total=%zu",
                     rememberedSlots.size(), concRemset.size(), remsetVec.size());
            }
            remapRemembered(workers);
        }
    }

    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungEvacFinish);
        {
        // Select flip-promoted pages; field iteration runs after world release.
        for (ZPage* region : minorCandidateRegions) {
            if (region->IsYoungRegion()) {
                // markwater2: allocating pages never entered the route plan
                // (zGeneration.cpp:211-213). Leave them young on unmovableFrom.
                // ZPage::is_marked (zPage.inline.hpp:223-226): only a page marked
                // this cycle has object liveness to promote.
                if (region->IsAllocating() || !region->is_marked()) {
                    continue;
                }
                if (kPageAgeAdaptiveTenuring &&
                    !ShouldPromoteAge(region->GetYoungAge(), GetGCStats(GCCycleGeneration::YOUNG).tenuringThreshold)) {
                    if (region->IsLoneFromRegion() || region->IsFromRegion()) {
                        manager.EnlistStayYoungSurvivor(region);
                    } else if (!(region->OnNamedList("recent full regions"))) {
                        RegionManager::FinishStayYoungInPlace(region);
                    }
                    continue;
                }
                manager.AddFlipPromotedPage(region);
            }
        }
        }
    }

    // zRelocate.cpp:1289-1306: finish relocation before walking flip-promoted pages.
    // Keep forwarding entries available until every field has been remapped.
    CHECK_DETAIL(stw != nullptr && *stw != nullptr,
                 "flip-promoted page task must release an active STW3 owner");
    stw->reset();
    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungConcPromoteWalk);
        manager.RememberFlipPromotedPages(workers);

    }

    *stw = std::make_unique<ScopedStopTheWorld>("young retire forwarding", true,
                                                GCPhase::GC_PHASE_FORWARD);
    ZVerify::BeforeZOperation();
    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungEvacRetire);
        // zGeneration.cpp:563: keep this set until the next young mark-end reset.
        // zRelocate.cpp:1041-1047 cycle-end completeness: no ROUTED-unfinished page.
        manager.FinishIncompleteFromRegions(GCCycleGeneration::YOUNG);
        manager.ReassembleFromSpace();
    }
}
// ZBarrier::barrier / remap_young_relocated (zBarrier.inline.hpp:318-361).
// Resolve and heal the same preloaded word. Preserve mark/remember metadata;
// another writer's load-good value terminates the shared self-heal CAS loop.
static BaseObject* RemapPromotedField(Collector& collector, RefField<>& field, zpointer observed)
{
    RefField<> value(observed);
    auto loadGood = [](zpointer word) {
        return ZPointer::is_load_good_or_null(to_zpointer(raw(word)));
    };
    if (loadGood(observed)) {
        return to_object(value.GetTargetObject());
    }
    const ForwardingProvenance provenance{ ForwardingHolderKind::Remset, nullptr, &field };
    BaseObject* target = collector.make_load_good(value, provenance);
    CHECK_DETAIL(target != nullptr || !(!is_null_any(to_zpointer(raw(observed)))),
                 "promotion remap must preserve a non-null reference");
    // ZAddress::load_good: upgrade remap bits without claiming a marking epoch.
    const zpointer healed = ZAddress::load_good(from_object(target), observed);
    ZBarrier::self_heal(ZBarrier::is_load_good_or_null_fast_path,
                        reinterpret_cast<volatile zpointer*>(&field), observed, healed, false);
    return target;
}

// ZRelocateWork::update_remset_promoted_filter_and_remap_per_field
// (zRelocate.cpp:741-794). Unfinished young relocation is remembered for
// deferred remapping; a page worker must not wait on another page's work.
void RegionManager::RememberPromotedObject(BaseObject* object)
{
    if (!object->HasRefField()) {
        return;
    }
    Collector& collector = Heap::GetHeap().GetCollector();
    // zRelocate.cpp:798: this relocation-work consumer uses the unsafe entry.
    ZIterator::basic_oop_iterate(object, [&](RefField<>& field) {
        const zpointer observed = field.GetFieldValue();
        RefField<> value(observed);
        BaseObject* target = to_object(value.GetTargetObject());
        if (target != nullptr && Heap::IsHeapAddress(target)) {
            const MAddress address = reinterpret_cast<MAddress>(target);
            ZForwarding* forwarding = ZPointer::is_load_good(value.GetFieldValue()) ? nullptr :
                generation_forwarding_table(Generation::Young).get(address);
            const MAddress to = forwarding == nullptr ? address : forwarding->find(address);
            if (to == 0 || Heap::page(to)->IsYoungRegion()) {
                ZPage* holder = Heap::page(reinterpret_cast<MAddress>(&field));
                if (holder != nullptr) {
                    holder->remember(reinterpret_cast<volatile zpointer*>(&field));
                }
                return;
            }
        }
        // Only completed/non-relocating old targets reach eager remapping.
        // Unfinished young forwarding above stays deferred in the remset.
        RemapPromotedField(collector, field, observed);
    });
}

void RegionManager::RememberFlipPromotedPages(ZWorkers& workers)
{
    // zRelocate.cpp:1257-1306. Producers have finished before the worker gang
    // starts; pages and their livemaps stay alive until it joins.
    std::vector<ZPage::PromotionPage*> pages;
    {
        std::lock_guard<std::mutex> lock(flipPromotedMutex);
        for (const auto& page : flipPromotedPages) {
            pages.push_back(page.get());
        }
    }
    class PageTask final : public ZTask {
    public:
        PageTask(const std::vector<ZPage::PromotionPage*>& pages, const std::function<void(RefField<>&)>& remember)
            : ZTask("ZRelocateRemsetFlipPromotedPagesTask"), iter(pages.data(), pages.size()), remember(remember) {}
        void work() override
        {
            // zArray.hpp:104 ZArrayParallelIterator: workers claim promoted pages.
            for (ZPage::PromotionPage* page; iter.next(&page);) {
                page->ObjectIterate([&](BaseObject* object) {
                    RefFieldVisitor remapAndRemember = [&](RefField<>& field) {
                        const zpointer observed = field.GetFieldValue();
                        BaseObject* target = RemapPromotedField(Heap::GetHeap().GetCollector(), field, observed);
                        if (target != nullptr && Heap::IsHeapAddress(target) &&
                            Heap::page(reinterpret_cast<MAddress>(target))->IsYoungRegion()) {
                            // RegionManager owns access to the remset producer.
                            remember(field);
                        }
                    };
                    // zRelocate.cpp:1275: flip promotion passes the known
                    // klass through the safe entry before field dispatch.
                    ZIterator::basic_oop_iterate_safe(object, object->GetTypeInfo(), remapAndRemember);
                });
            }
        }
    private:
        ZArrayParallelIterator<ZPage::PromotionPage*> iter;
        const std::function<void(RefField<>&)> remember;
    } task(pages, [](RefField<>& field) {
        ZPage* holder = Heap::page(reinterpret_cast<MAddress>(&field));
        if (holder != nullptr) {
            holder->remember(reinterpret_cast<volatile zpointer*>(&field));
        }
    });
    workers.run(&task);
}

// permhole receiptization (steer1): RouteObject is geometric (ROUTED before Copy fills
// tip). A tip-valid to is a *receipt* (copy happened). A geometric to with tip==0 is only
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
//   survived(off)              the from-livemap covers this offset, so compact insert
//                              owed a receipt for it and there is none -> refuse.
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

static CompactedMissClass ClassifyCompactedMiss(ZPage* region, BaseObject* obj)
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
        ZForwarding* provenance = forwarding_for_page(region);
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
//   forwarding->retain_page(&_queue)   ->  ZPage::TryLockReadFromRegion()
//   relocate_object_inner(...)         ->  RelocateObjectInner
//   forwarding->release_page()         ->  ZPage::UnlockReadFromRegion()
//
// nullptr means the current thread did not acquire the page. The caller may
// consume a receipt installed by the owning copier, but may not use the from
// address as an alternate result.
BaseObject* WCollector::WaitForPageForwarding(BaseObject* obj, ZForwarding* owner) const
{
    if (!owner || ZForwarding::CurrentPageWork() == owner) return nullptr;
    const MAddress from = reinterpret_cast<MAddress>(obj);
    if (const MAddress found = owner->find(from)) {
        return reinterpret_cast<BaseObject*>(found);
    }
    auto& manager = static_cast<RegionSpace&>(theAllocator).GetRegionManager();
    if (MutatorManager::Instance().WorldStopped() && !owner->is_done()) {
        // #498: without return barriers roots are completed eagerly. There is
        // no concurrent page worker in this pause; reuse its in-place task.
        ZPage* page = owner->page();
        if (owner->table_generation() == static_cast<uint8_t>(Generation::Young)) {
            manager.ForwardClaimedPage<Generation::Young>(page, owner, false, true);
        } else {
            manager.ForwardClaimedPage<Generation::Old>(page, owner, false, true);
        }
        if (const MAddress winner = owner->find(from)) return reinterpret_cast<BaseObject*>(winner);
    }
    auto& queue = manager.GetZRelocateQueue();
    const auto request = queue.Add(owner);
    CHECK_DETAIL(request.accepted, "relocation request has no page task from=%#zx", from);
    queue.Wait(request.forwarding);
    return reinterpret_cast<BaseObject*>(owner->find(from));
}

BaseObject* WCollector::TryMutatorRelocate(BaseObject* obj, ZPage::RetainScope& lease) const
{
    // RelocateObjectInner is for relocate phase only. relocate_or_remap
    // is reachable from barriers in other phases, so screen here.
    GCPhase phase = GetGCPhase(static_cast<GCCycleGeneration>(ObjectGeneration(obj)));
    if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
        return nullptr;
    }
    // zForwarding.cpp:86-108: a claimed page waits for its task before
    // retain_page returns false; no source access follows a failed retain.
    if (!lease.ok()) {
        return WaitForPageForwarding(obj, lease.HoldForwarding());
    }
    // zRelocate.cpp:393-395: retain_page then assert is_phase_relocate.
    // SetGCPhase publishes before handshake, so a mutator that retained across
    // FORWARD→IDLE must not copy. Release and let
    // the existing FindToVersion / wait legs consume the published table.
    phase = GetGCPhase(static_cast<GCCycleGeneration>(ObjectGeneration(obj)));
    if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
        lease.Release();
        return nullptr;
    }
    // A mutator can publish a previously white from-object after young mark
    // terminated. Admit it before copying; a next-minor remset entry is too late.
    // This is the late-store leg corresponding to zBarrier.inline.hpp:695-716.
    EnsureRouteDomainMembership(const_cast<WCollector*>(this), obj);
    BaseObject* toVersion = const_cast<WCollector*>(this)->RelocateObjectInner(
        obj, lease.forwarding()->page());
    lease.Release(); // release_page
    if (toVersion == nullptr) {
        return WaitForPageForwarding(obj, lease.HoldForwarding());
    }
    if (toVersion == obj) {
        return nullptr;
    }
    return toVersion;
}

BaseObject* WCollector::ResolveStoreValue(BaseObject* ref, const ForwardingProvenance& provenance,
                                         Generation generation) const
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
        if (IsAlreadyToStoreValue(current, generation)) {
            return current;
        }
        const MAddress currentAddr = reinterpret_cast<MAddress>(current);
        ZPage* currentRegion = ZPage::GetGhostFromRegionAt(currentAddr);
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
        FindToVersionResult found = FindToVersion(current, generation);
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
        ZPage* ghost = currentRegion;
        if (ghost == nullptr) {
            ZPage* live = Heap::page(currentAddr);
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
            const MAddress lookupTo = forwarding_find(generation, currentAddr);
            LOG(RTLOG_ERROR,
                "[FWDTABLE][resolve-miss] site=no-forwarding consumer=WCollector::ResolveStoreValue "
                "holder_kind=%s holder=%p slot=%p stage=%s writer_kind=%s "
                "incoming_source_kind=%s source_slot=%p working_copy_slot=%p "
                "field_type=%s field_offset=%zu "
                "from=%p from_region=%p region_type=%u generation=%u "
                "in_current_relocation_set=%u table_id=%#zx lookup_state=%u "
                "from_page_epoch=%llu lifeId=%llu "
                "gc_phase=%u ghost=0 compacted=%u route=%u lookup.to=%p "
                " verdict=%u",
                ForwardingProvenance::KindName(provenance.kind), provenance.holder, provenance.slot,
                ForwardingProvenance::StageName(provenance.stage),
                ForwardingProvenance::WriterName(provenance.writerKind),
                ForwardingProvenance::SourceName(provenance.incomingSourceKind), provenance.sourceSlot,
                provenance.workingCopySlot, ForwardingProvenance::FieldName(provenance.fieldKind),
                provenance.fieldOffset,
                static_cast<void*>(current), static_cast<void*>(live),
                live != nullptr ? 0u : 0xffu,
                live != nullptr ? static_cast<unsigned>(live->generation_id()) : 0xffu,
                (currentAddr != 0 && generation_forwarding_table(generation).get(currentAddr) != nullptr) ? 1u : 0u, 0zu,
                0u,
                0ull,
                0ull,
                static_cast<unsigned>(GetGCPhase(GCCycleGeneration::OLD)),
                live != nullptr && live->IsCompacted() ? 1u : 0u,
                live != nullptr ? live->RelocateObserve() : 0u,
                reinterpret_cast<void*>(lookupTo),
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
        BaseObject* resolved = relocate_or_remap_object(current, static_cast<ZGenerationId>(generation), provenance);
        if (resolved == nullptr) {
            FailClosedLoad("WCollector::ResolveStoreValue.unresolved", current, 0, provenance);
        }
        if (resolved == current) {
            // In-place completion must have published its identity receipt;
            // without it, returning current would recreate the removed
            // lookup-miss fallback.
            FindToVersionResult identity = FindToVersion(current, generation);
            if (identity.found() == current &&
                Collector::JudgeHandOutTarget(current) == HandVerdict::Usable) {
                return current;
            }
            // zGeneration.inline.hpp:131-135: forwarding table gone → safe(addr).
            // Ghost can be dispelled between the membership check and
            // relocate_or_remap; that is not a missing identity receipt.
            if (Collector::JudgeHandOutTarget(current) == HandVerdict::Usable &&
                !current->IsForwarded() &&
                ZPage::GetGhostFromRegionAt(currentAddr) == nullptr) {
                return current;
            }
            FailClosedLoad("WCollector::ResolveStoreValue.missing-identity", current, 0, provenance);
        }
        current = resolved;
    }
}

BaseObject* WCollector::ForwardObject(BaseObject* obj, Generation generation)
{
    BaseObject* to = relocate_or_remap_object(obj, static_cast<ZGenerationId>(generation));
    if (to != nullptr && to != obj) {
        return to;
    }
    // GetRoute survivor gate / exclusive soft-miss: a movable ghost-from with no
    // to-version is not a stable address. Returning `obj` here reinstalls a from
    // pointer that CollectRegion is about to reclaim → UAF / HANG under ALOT.
    // Unmovable / non-ghost still keep `obj` (in-place / not in route domain).
    if (IsGhostFromObject(obj) && !IsUnmovableFromObject(obj)) {
        ZPage* region = ZPage::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
        BaseObject* waited = WaitForPageForwarding(obj, forwarding_for_page(region));
        if (waited != nullptr) {
            return waited;
        }
        if (const MAddress hit = forwarding_find(generation, reinterpret_cast<MAddress>(obj))) {
            return reinterpret_cast<BaseObject*>(hit);
        }
        // zRelocate.cpp:412-415: after wait, the table holds the winner. The page
        // worker copying this object (CurrentPageWork) must not wait on itself.
        if (ZForwarding::CurrentPageWork() != nullptr) {
            return nullptr;
        }
        CHECK_DETAIL(false, "should be forwarded from=%p", obj);
        return nullptr;
    }
    return obj;
}

BaseObject* WCollector::ForwardObjectExclusive(BaseObject* obj)
{
    ZPage* page = ZPage::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (page == nullptr) {
        page = Heap::page(reinterpret_cast<MAddress>(obj));
    }
    if (page == nullptr) {
        return nullptr;
    }
    return RelocateObjectInner(obj, page);
}

void WCollector::UpdateRemsetForFields(BaseObject* from, BaseObject* to)
{
    if (from == nullptr || to == nullptr || from == to) {
        return;
    }
    ZPage* toRegion = Heap::page(reinterpret_cast<MAddress>(to));
    if (toRegion == nullptr || toRegion->IsYoungRegion()) {
        return;
    }
    ZPage* fromRegion = ZPage::GetGhostFromRegionAt(reinterpret_cast<MAddress>(from));
    if (fromRegion == nullptr) {
        fromRegion = Heap::page(reinterpret_cast<MAddress>(from));
    }
    if (fromRegion != nullptr && !fromRegion->IsYoungRegion()) {
        const size_t sz = RegionSpace::GetAllocSize(*to);
        const uintptr_t fromLocal = fromRegion->local_offset(reinterpret_cast<MAddress>(from));
        auto iter = fromRegion->remset_iterator_limited_previous(fromLocal, sz);
        BitMap::idx_t index;
        while (iter.next(&index)) {
            const uintptr_t fieldOff = ZRememberedSet::to_offset(index) - fromLocal;
            toRegion->remember(reinterpret_cast<volatile zpointer*>(reinterpret_cast<MAddress>(to) + fieldOff));
        }
        return;
    }
    RegionManager::RememberPromotedObject(to);

}

BaseObject* WCollector::RelocateObjectInner(BaseObject* obj, ZPage* copyPage)
{
    const MAddress fromAddr = reinterpret_cast<MAddress>(obj);
    if (const MAddress hit = (forwarding_for_page(copyPage) != nullptr ? forwarding_for_page(copyPage)->find(fromAddr) : 0)) {
        BaseObject* to = reinterpret_cast<BaseObject*>(hit);
        UpdateRemsetForFields(obj, to);
        return to;
    }
    const size_t size = RegionSpace::GetAllocSize(*obj);
    // ZObjectAllocator::alloc_for_relocation: per-age shared allocation, non-blocking.
    const PageAge fromAge = copyPage->IsYoungRegion() ? to_pageage(copyPage->GetYoungAge()) : PageAge::old;
    const PageAge toAge = ComputeToAge(fromAge, GetGCStats(GCCycleGeneration::YOUNG).tenuringThreshold);
    auto& manager = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    BaseObject* toObj = reinterpret_cast<BaseObject*>(manager.AllocSharedObject(size, toAge, true));
    if (toObj == nullptr) return nullptr;
    BaseObject* result = nullptr;
    ZForwarding* publication = forwarding_for_page(copyPage);
    if (publication != nullptr) {
        DLOG(FORWARD, "forward obj %p<%p>(%zu) to %p", obj, obj->GetTypeInfo(), size, toObj);
        // zRelocate.cpp:369: a fresh to-page copy is disjoint.
        ZUtils::object_copy_disjoint(to_zaddress(reinterpret_cast<uintptr_t>(obj)),
                                     to_zaddress(reinterpret_cast<uintptr_t>(toObj)), size);
        if (toObj != obj) {
            toObj->SetStateCode(ObjectState::NORMAL);
        }
        std::atomic_thread_fence(std::memory_order_release);
        if (toObj == obj || (toObj != nullptr)) {
            const MAddress mapped = publication->insert(reinterpret_cast<MAddress>(obj), reinterpret_cast<MAddress>(toObj));
            if (mapped != 0) {
                obj->SetStateCode(ObjectState::FORWARDED);
                result = reinterpret_cast<BaseObject*>(mapped);
            }
        }
    } else {
        const MAddress pageStart = copyPage == nullptr ? 0 : copyPage->GetRegionStart();
        const uint64_t entries = forwarding_for_page(copyPage) == nullptr ? 0 : 1;
        LOG(RTLOG_ERROR,
            "[GCV2][first-visitor] publication refused obj=%p page=%p pageStart=%#zx entries=%llu route=%u done=%u ref=%d",
            obj, copyPage, static_cast<size_t>(pageStart), static_cast<unsigned long long>(entries),
            copyPage == nullptr ? 0U : static_cast<unsigned>(copyPage->RelocateObserve()),
            copyPage == nullptr ? 0U : static_cast<unsigned>(copyPage->IsForwardingDone()),
            copyPage == nullptr ? 0 : copyPage->ForwardingRefCount());
    }
    if (result != toObj) {
        if (ZPage* dest = Heap::page(reinterpret_cast<MAddress>(toObj))) {
            (void)dest->UndoAllocObjectAtomic(reinterpret_cast<uintptr_t>(toObj), size);
        }
    }
    if (result != nullptr) {
        UpdateRemsetForFields(obj, result);
    }
    return result;
}


} // namespace MapleRuntime

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
template<Generation G>
void ForwardTask<G>::work()
{
    detail::ExecuteForwardTask<G>(regionManager, fromRegionList);
}
#endif

template<Generation G>
void RegionManager::StartForwardFromRegions(ZWorkers& workers)
{
    CHECK(!relocationStarted);
    relocationStarted = true;
    relocationDrained = false;
    relocationWorkers = &workers;
    relocateQueue.BeginWorkers(workers.active_workers());
}

template<Generation G>
void RegionManager::DrainForwardFromRegions()
{
    if (relocationDrained) {
        return;
    }
    relocationDrained = true;
    if (relocationWorkers == nullptr) {
        ForwardFromRegions<G>();
        return;
    }
    ForwardTask<G> task(*this, fromRegionList);
    relocationWorkers->run(&task);
}

template<Generation G>
void RegionManager::ForwardFromRegions(ZWorkers& workers)
{
    if (!relocationStarted) {
        StartForwardFromRegions<G>(workers);
    }
    DrainForwardFromRegions<G>();
    relocationWorkers = nullptr;
    relocationStarted = false;
    relocationDrained = false;
}

template<Generation G>
void RegionManager::ForwardClaimedPage(ZPage* region, ZForwarding* owner, bool claimed, bool inPlace)
{
    if (!owner || (!claimed && !owner->claim())) return;
    ZForwarding::PageWorkScope work(owner);
    if (inPlace) {
        (void)fromRegionList.TryDeleteRegion(region);
        owner->set_in_place();
        CompactRegion(region);
    } else {
        ForwardRegion<G>(region);
    }
    // All page metadata and legacy helper work is finished. A nested drain
    // may already have consumed the construction token; otherwise drop it now.
    if (owner->ref_count().load(std::memory_order_acquire) != 0) owner->release_page();
    owner->detach_page();
    owner->mark_done();
    // From here on only forwarding/queue state may be touched.
    (void)relocateQueue.Complete(owner);
}


namespace {
void WaitCopiedObjectsUnlocked(ZPage* region)
{
    if (region == nullptr || region->IsFreeRegion()) {
        return;
    }
    ZForwarding::WaitPageDone(forwarding_for_page(region));
}

template<typename Fn>
void ForEachLiveObjectStart(ZPage* region, MAddress start, MAddress allocPtr, Fn&& fn)
{
    // ZPage::object_iterate (zPage.inline.hpp:319-331) over the original page's
    // livemap: the from-page carrier's map when published, else the page's own.
    ZGenerationId id;
    ZLiveMap* map = RouteLiveMap(region, id);
    if (map == nullptr) {
        return;
    }
    const int shift = region->object_alignment_shift();
    map->iterate(id, [&](BitMap::idx_t index) -> bool {
        const size_t offset = (index / 2) << shift;
        if (start + offset < allocPtr) {
            fn(from_region_addr(start + offset), offset);
        }
        return true;
    });
}

// ZGC's relocate() marks a forwarding life done only after every survivor has
// a forwarding receipt (zRelocate.cpp:1137-1153). Header state and compact
// geometry are not receipts: kept/in-place survivors must have an explicit
// from->from entry in the same active/retired table. Keep this check at the
// producer boundary so a receipt-less publication fails loudly.
bool VerifyRelocatedPage(ZPage* region, const char* site)
{
    // zRelocate.cpp:1006: verify before MarkForwardingDone/reset releases the
    // source livemap. ForwardRegion's outer return is too late for this check.
    if (ZVerifyForwarding && region != nullptr) {
        auto forwarding = forwarding_for_page(region);
        CHECK_DETAIL(static_cast<bool>(forwarding), "Missing forwarding at %s", site);
        forwarding->verify();
    }


    return true;
}
} // namespace

void RegionManager::ParkUnmovableFromRegion(ZPage* region)
{
    // youngconcfollow: callers already unlink the FROM node — TryDelete FROM here
    // would DecCounts a second time ("error count 1-0 16-0"). Only a GARBAGE node
    // can still sit on garbageRegionList (the CHECK at
    // TryTakeGarbageRegionAfterDispel, RegionManager.h:984); unlink it before the
    // rehome below so the garbage list cannot name a non-GARBAGE region.
    if (region != nullptr && region->IsGarbageRegion()) {
        garbageRegionList.TryDeleteRegion(region);
    }
    unmovableFromRegionList.PrependRegion(region);
}

void RegionManager::ExemptFromRegion(ZPage* region)
{
    ParkUnmovableFromRegion(region);
}

namespace {
bool IncompleteRouteUnpublished(ZPage* region)
{
    if (region == nullptr || region->IsFreeRegion()) {
        return false;
    }
    if (region->IsForwardingDone()) {
        return false;
    }
    return forwarding_for_page(region) != nullptr;
}
} // namespace

void RegionManager::FinishIncompleteFromRegions(GCCycleGeneration generation)
{
    // zRelocate.cpp:1041-1047: relocate() does not return with a half-copied page.
    std::vector<ZPage*> snap;
    auto push = [&snap](ZPage* region) {
        if (region != nullptr) {
            snap.push_back(region);
        }
    };
    ghostFromRegionList.VisitAllGhostRegions(push);
    fromRegionList.VisitAllRegions(push);
    unmovableFromRegionList.VisitAllRegions(push);
    garbageRegionList.VisitAllRegions(push);

    std::sort(snap.begin(), snap.end());
    snap.erase(std::unique(snap.begin(), snap.end()), snap.end());

    const bool young = generation == GCCycleGeneration::YOUNG;
    static std::atomic<size_t> g_zombieFinished{ 0 };
    static std::atomic<size_t> g_zombieKept{ 0 };
    size_t finished = 0;
    size_t kept = 0;

    for (ZPage* region : snap) {
        if (!IncompleteRouteUnpublished(region)) {
            continue;
        }
        if (region->IsUnmovableFromRegion()) {
            ++kept;
            continue;
        }
        if (region->IsGarbageRegion()) {
            garbageRegionList.TryDeleteRegion(region);
            ExemptFromRegion(region);
            ++kept;
            continue;
        }
        const bool wasFrom = region->IsFromRegion();
        if (wasFrom) {
            fromRegionList.TryDeleteRegion(region);
        }
        const bool canForward = region->IsLoneFromRegion() ||
            (region->IsThreadLocalRegion() && (region->IsRoutingState() || region->IsCompacted()));
        if (canForward) {
            if (young) {
                ForwardRegion<Generation::Young>(region);
            } else {
                ForwardRegion<Generation::Old>(region);
            }
            if (!IncompleteRouteUnpublished(region)) {
                ++finished;
                continue;
            }
        }
        if (region->IsFromRegion()) {
            fromRegionList.TryDeleteRegion(region);
        }
        if (region->IsLoneFromRegion() || region->IsFromRegion() || wasFrom) {
            ExemptFromRegion(region);
        }
        ++kept;
    }

    if (finished != 0) {
        g_zombieFinished.fetch_add(finished, std::memory_order_relaxed);
    }
    if (kept != 0) {
        g_zombieKept.fetch_add(kept, std::memory_order_relaxed);
    }
    const size_t finTot = g_zombieFinished.load(std::memory_order_relaxed);
    const size_t keptTot = g_zombieKept.load(std::memory_order_relaxed);
    if (finished != 0 || kept != 0 || finTot != 0 || keptTot != 0) {
        LOG(RTLOG_ERROR, "[GCV2][zombie] finished=%zu kept=%zu tot_finished=%zu tot_kept=%zu", finished, kept, finTot,
            keptTot);
    }

    for (ZPage* region : snap) {
        if (region == nullptr || region->IsFreeRegion()) {
            continue;
        }
        CHECK_DETAIL(!IncompleteRouteUnpublished(region),
                     "[GCV2][zombie] fourth state region=%p start=%#zx route=%u done=%u type=%u live=%zu "
                     "— cycle-end from-page not in {FORWARDED,COMPACTED,Exempt-kept}",
                     region, region->GetRegionStart(), static_cast<unsigned>(region->RelocateObserve()),
                     static_cast<unsigned>(region->IsForwardingDone()),
                     0u, (region->is_marked() ? region->live_bytes() : 0));
    }
}

void RegionManager::CollectFromSpaceGarbage()
{
    // cjpmnull2 5b31efeb mirrored onto this second reclaim entry: a page still
    // in the relocation set (route ∉ {FORWARDED,COMPACTED} and not Exempt-kept)
    // must not be merged into garbage. ZGC free_page never runs while the page
    // is in the relocation set (zGeneration.cpp:216-221).
    static std::atomic<size_t> g_fromGarbageSkip{ 0 };
    ZPage* region = fromRegionList.TakeHeadRegion();
    while (region != nullptr) {
        const bool complete = region->IsForwardingDone();
        if (!complete) {
            const size_t n = g_fromGarbageSkip.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8 || (n & (n - 1)) == 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][from-garbage-skip] n=%zu region=%p start=%#zx route=%u done=%u live=%zu "
                    "— skip CollectFromSpaceGarbage, Exempt",
                    n, region, region->GetRegionStart(), region->IsForwardingDone() ? 1u : 0u,
                    static_cast<unsigned>(region->IsForwardingDone()), (region->is_marked() ? region->live_bytes() : 0));
            }
            ExemptFromRegion(region);
        } else {
            garbageRegionList.PrependRegion(region);
        }
        region = fromRegionList.TakeHeadRegion();
    }
}

template<Generation G>
void RegionManager::ForwardFromRegions()
{
    detail::ExecuteForwardTask<G>(*this, fromRegionList);

    VLOG(REPORT, "forward %zu from-region units", fromRegionList.GetUnitCount());

    AllocBuffer* allocBuffer = AllocBuffer::GetAllocBuffer();
    if (LIKELY(allocBuffer != nullptr)) {
        allocBuffer->ClearRegion(); // clear region for next GC
    }
}


bool RegionManager::RelocateClaimedPage(ZPage* region)
{
    CHECK_DETAIL(region->GetRawPointerObjectCount() <= 0, "pinned region shouldn't be moved");
    MAddress regionStart = region->GetRegionStart();
    MAddress regionLimit = region->GetRegionAllocPtr();
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    bool allocFailed = false;
    ForEachLiveObjectStart(region, regionStart, regionLimit, [&](BaseObject* currentObj, size_t) {
        if (allocFailed) {
            return;
        }
        ZForwarding* liveFwd = forwarding_for_page(region);
        if (liveFwd != nullptr && liveFwd->find(reinterpret_cast<MAddress>(currentObj))) {
            return;
        }
        if (collector.ForwardObjectExclusive(currentObj) == nullptr) {
            allocFailed = true;
        }
    });
    if (allocFailed) {
        forwarding_for_page(region)->set_in_place();
        CompactRegion(region);
        return false;
    }
    VerifyRelocatedPage(region, "RelocateClaimedPage");
    return true;
}

void RegionManager::CompactRegion(ZPage* region)
{
    auto owner = forwarding_for_page(region);
    ZForwarding::PageWorkScope work(owner,
        owner && ZForwarding::CurrentPageWork() != owner);
    if (owner && owner->ref_count().load(std::memory_order_acquire) > 0) {
        owner->in_place_relocation_claim_page();
    }

    const bool fromYoung = region->IsYoungRegion();
    const PageAge fromAge = fromYoung ? to_pageage(region->GetYoungAge()) : PageAge::old;
    const PageAge toAge = ComputeToAge(fromAge, Heap::GetHeap().GetCollector().GetGCStats(GCCycleGeneration::YOUNG).tenuringThreshold);
    MAddress regionStart = region->GetRegionStart();
    DLOG(REGION, "compact region %p@[%#zx+%zu, %#zx) type %u", region, regionStart,
        (region->is_marked() ? region->live_bytes() : 0), region->GetRegionEnd(), 0u);
    MAddress regionLimit = region->GetRegionAllocPtr();
    ZForwarding* publication = forwarding_for_page(region);
    CHECK_DETAIL(publication != nullptr,
                 "compact forwarding table unavailable before copy region=%p range=[%#zx,%#zx)",
                 region, static_cast<size_t>(regionStart), static_cast<size_t>(region->GetRegionEnd()));
    region->SetRegionAllocPtr(regionStart);
    // ZGC zRelocate.cpp:838-861 start_in_place_relocation_prepare_remset: this page is its own
    // to-page, so its old remembered-set bits have to leave the face before the copy walk starts
    // writing the new ones.  What the walk does not hand back is dropped, which is
    // clear_remset_before_in_place_reuse (zRelocate.cpp:1027-1035).
    if (!fromYoung) {
        region->swap_remset_bitmaps();
    }
    // ZGC zRelocate.cpp:862-896: establish the to-page age before publishing
    // any in-place forwarding entry. The descriptor stays in the page table,
    // so promotion publishes its old identity here. PublishFromPageMetadata
    // already saved the source generation, livemap and birth sequence in
    // the forwarding carrier; ForEachLiveObjectStart consumes that snapshot,
    // not the new destination livemap allocated by PromoteYoungRegion.
    if (fromYoung) {
        if (toAge == PageAge::old) {
            region->PromoteYoungRegion();
        } else {
            region->reset(toAge);
        }
    }
    // ZPage::reset(to_age): only the actual in-place destination is born anew.
    region->ResetPageSequence();
    ForEachLiveObjectStart(region, regionStart, regionLimit, [&](BaseObject* currentObj, size_t offset) {
        const MAddress currentPtr = regionStart + offset;
        ZForwarding* liveFwd = forwarding_for_page(region);
        if (liveFwd != nullptr && liveFwd->find(currentPtr)) {
            return;
        }
        size_t size = currentObj->GetSize();
        MAddress toAddress = region->Alloc(size);
        BaseObject* toObj = from_region_addr(toAddress);
        DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
        // zRelocate.cpp:634-639: in-place relocation copies conjoint when the
        // new object overlaps the old one, disjoint otherwise.
        const zaddress fromAddr = to_zaddress(reinterpret_cast<uintptr_t>(currentObj));
        if (toAddress + size > currentPtr) {
            ZUtils::object_copy_conjoint(fromAddr, to_zaddress(toAddress), size);
        } else {
            ZUtils::object_copy_disjoint(fromAddr, to_zaddress(toAddress), size);
        }
        toObj->SetStateCode(ObjectState::NORMAL);
        std::atomic_thread_fence(std::memory_order_release);
        const MAddress receipt = publication->insert(currentPtr, toAddress);

        // ZGC zRelocate.cpp:652-731 update_remset_old_to_old: the bits covering the from copy
        // name field offsets inside this object, so they follow it to its new address.
        if (fromYoung && toAge == PageAge::old) {
            RememberPromotedObject(toObj);
        } else if (!fromYoung) {
            const uintptr_t fromLocal = region->local_offset(currentPtr);
            auto iter = region->remset_iterator_limited_previous(fromLocal, size);
            BitMap::idx_t index;
            while (iter.next(&index)) {
                const uintptr_t fieldOff = ZRememberedSet::to_offset(index) - fromLocal;
                region->remember(reinterpret_cast<volatile zpointer*>(toAddress + fieldOff));
            }
        }
    });

    MAddress cur = region->GetRegionAllocPtr();
    if (regionLimit > cur) {
        size_t reclaimSize = regionLimit - cur;
        HeapFiller::ZeroAndFill(cur, reclaimSize);
    }

    region->ResetCensusBoundary();
    VerifyRelocatedPage(region, "CompactRegion.whole");
    WaitCopiedObjectsUnlocked(region);
    region->MarkForwardingDone();

    // zForwarding.cpp:171-181 / zRelocate.cpp:1001-1047: the forwarding table
    // outlives page reuse. Do not put this page on the mutator TLAB list while
    // its table is live — RehomeCompactedInPlaceRegion keeps it collector-visible.
    RehomeCompactedInPlaceRegion(region);
}

void RegionManager::EnlistCompactedRegionForAllocator(ZPage* region)
{
    if (region == nullptr) {
        return;
    }
    bool claimed = false;
    if (region->IsFromRegion()) {
        claimed = fromRegionList.TryDeleteRegion(region);
    } else if (region->IsLoneFromRegion()) {
        claimed = true;
    } else if (region->IsGarbageRegion()) {
        claimed = garbageRegionList.TryDeleteRegion(region);
    } else if (region->IsThreadLocalRegion() || region->OnNamedList("recent full regions")) {
        return;
    }
    if (claimed) {
        tlRegionList.PrependRegion(region);
    }
}

// A region the forward path finished with in place has to stay reachable by a collection-set
// builder, and CompactRegion leaves it on tlRegionList, which no builder walks.
//
// ZGC gets this structurally: a page is in _page_table from ZHeap::alloc_page (zHeap.cpp:257) until
// ZHeap::free_page (:277), and select_relocation_set iterates that table
// (zGeneration.cpp:205-212), so allocator ownership and collection visibility are separate
// questions. Ours ties them together through a list, and the compact-in-place arm drops the
// allocator side without moving the region: AllocBuffer::ClearRegion (AllocBuffer.h:36-44) only
// nulls tlRegion, it does not unlink anything.
//
// The result is a region no path can reach again. It cannot be allocated from --
// AllocateThreadLocalRegion always takes a fresh region -- and it cannot be collected, because
// AssembleSmallGarbageCandidates and PrepareYoungGarbageCandidates walk fromRegionList,
// recentFullRegionList and unmovableFromRegionList, and neither walks tlRegionList. It is simply
// retained until the heap goes away.
//
// Same shape as the stay-young survivor that had to be re-homed earlier in this cycle: the work
// finished, and nothing put the region back where the next cycle looks.
void RegionManager::RehomeCompactedInPlaceRegion(ZPage* region)
{
    if (region == nullptr) {
        return;
    }
    bool claimed = false;
    if (region->IsFromRegion()) {
        claimed = fromRegionList.TryDeleteRegion(region);
    } else if (region->IsLoneFromRegion()) {
        claimed = true;
    } else if (region->IsGarbageRegion()) {
        claimed = garbageRegionList.TryDeleteRegion(region);
    } else if (region->IsThreadLocalRegion()) {
        claimed = tlRegionList.TryDeleteRegion(region);
    } else if (region->OnNamedList("recent full regions")) {
        return;
    }
    if (!claimed) {
        return;
    }
    recentFullRegionList.PrependRegion(region);
    RecentFullAccounting::Enqueue(1, region->GetUnitCount());
}



namespace {
bool StayYoungThisCycle(ZPage* region)
{
    if (!kPageAgeAdaptiveTenuring) {
        return false;
    }
    const uint32_t thr = Heap::GetHeap().GetCollector().GetGCStats(GCCycleGeneration::YOUNG).tenuringThreshold;
    return !ShouldPromoteAge(region->GetYoungAge(), thr);
}

} // namespace

void RegionManager::BumpYoungSurvivorAge(ZPage* region)
{
    uint8_t next = region->GetYoungAge();
    if (next < untype(PageAge::survivor14)) {
        region->reset(static_cast<PageAge>(next + 1));
    }
}

void RegionManager::FinishStayYoungInPlace(ZPage* region, bool advanceAge)
{
    if (advanceAge) {
        BumpYoungSurvivorAge(region);
    }
    WaitCopiedObjectsUnlocked(region);
    VerifyRelocatedPage(region, "FinishStayYoungInPlace");
    region->MarkForwardingDone();
    // The selected-set carrier remains queryable after payload release.
    // The next selection/reset retires its ghost/source view; completing this
    // page task does not revoke forwarding-table membership.
}

void RegionManager::EnlistStayYoungSurvivor(ZPage* region, bool advanceAge)
{
    FinishStayYoungInPlace(region, advanceAge);
    // evac_finish calls this on FROM regions still linked in fromRegionList.
    // PrependRegion overwrites next/prev without unlinking — later
    // CollectFromSpaceGarbage MergeRegionList walks a chain that now points
    // into recentFull, and DeleteRegionLocked SEGVs (r13=0, +0x14).
    bool claimed = false;
    if (region->IsFromRegion()) {
        claimed = fromRegionList.TryDeleteRegion(region);
    } else if (region->IsLoneFromRegion()) {
        claimed = true;
    } else if (region->IsGarbageRegion()) {
        claimed = garbageRegionList.TryDeleteRegion(region);
    } else if (region->IsThreadLocalRegion()) {
        claimed = tlRegionList.TryDeleteRegion(region);
    } else if (region->OnNamedList("recent full regions")) {
        return;
    }
    if (!claimed) {
        return;
    }
    recentFullRegionList.PrependRegion(region);
    RecentFullAccounting::Enqueue(1, region->GetUnitCount());
}

template<Generation G>
void RegionManager::ForwardRegion(ZPage* region)
{
    // zRelocate.cpp:993-1003. The owner outlives source-page retirement, so
    // the after check reads the forwarding table and destination objects only.
    auto verifyForwarding = forwarding_for_page(region);
    ZVerify::BeforeRelocation(verifyForwarding);
    struct VerifyAfterRelocation {
        ZForwarding* forwarding;
        ~VerifyAfterRelocation()
        {
            ZVerify::AfterRelocation(forwarding);
        }
    } verifyAfterRelocation { verifyForwarding };

    CHECK_DETAIL(region->IsFromRegion() || region->IsLoneFromRegion() || (region->IsThreadLocalRegion() &&
        (region->IsRoutingState() || region->IsCompacted())), "region type %u", 0u);

    DLOG(FORWARD, "try forward region %p @[0x%zx+%zu, 0x%zx) type %u, live bytes %zu",
        region, region->GetRegionStart(), region->GetRegionAllocatedSize(), region->GetRegionEnd(),
        0u, (region->is_marked() ? region->live_bytes() : 0));

    bool youngRegion = region->IsYoungRegion();
    if (youngRegion && !GenerationMayRelocateYoung(G)) {
        // The old generation has no authority to interpret a young page's
        // liveness or promote it.  Keep it for the young generation without
        // advancing survivor age (zGeneration.cpp:195-221).
        EnlistStayYoungSurvivor(region, false);
        return;
    }
    // oracleblack: the generational contract also guards this arm. The OLD pass stamps a
    // current-epoch mark face on young regions it never actually examines, so
    // "markedThisCycle ∧ live==0" holds vacuously for them and the residual f3-livehole
    // census (~128/run after the unmarked-arm gate below) was fed from here. Only the
    // YOUNG pass may prove a young region empty (zGeneration.cpp:216-221: each generation
    // frees only pages its own mark examined).
    if (region->IsRouteKnownEmpty() && !(youngRegion && G == Generation::Old)) {
        // cjpmnull2: IsKnownEmpty is now ZGC-shaped (this-cycle marked ∧ live==0).
        // Only those pages are empty; collect them (zGeneration.cpp:216-221).
        if (youngRegion) {
            region->PromoteYoungRegion();
        }

        CollectRegion<G>(region);
        return;
    }
    // Unmarked this cycle is not empty (zPage.inline.hpp:223-225). Still do
    // not keep every never-examined from-page: hangfloor showed young
    // neverExamined × Collect-skip fills the heap (10/10 HANG). Keep only
    // the two cjpmnull classes — residual live bytes, or a published plan
    // that has not been copied (route=3). live==0 FORWARDABLE is true dead.
    {
        const bool incompleteRoute = region->IsRoutingState() && !region->IsForwardingDone();
        ZGenerationId routeId;
        ZLiveMap* routeMap = RouteLiveMap(region, routeId);
        const bool routeMarked = routeMap != nullptr && routeMap->is_marked(routeId);
        const bool liveResidual = routeMarked && routeMap->live_bytes() > 0;
        // hangfloor: young neverExamined×keep fills the heap. Old from-pages
        // with payload are the 59-class (route=1 liveinfo_null, live-slots>0).
        // live==0 after THIS cycle's mark is freed at ExemptFromRegions
        // (zGeneration.cpp:216-221), before the page is FORWARDABLE. Do not
        // Collect here: VisitLive copies nothing then FORWARDED+Collect is
        // the NW 256MB keep-from UAF (pc=0x8aa8 reclaim_satb).
        //
        // oracleblack: generational contract on the young arm. A young region's liveness is
        // the MINOR's to judge -- a minor marks young via remset+roots, so "no mark bitmap"
        // after a minor really means empty and the collect below is legitimate. A MAJOR
        // never examines young objects at all: under a workload whose config never fires a
        // minor (cjpm at 12GB: youngRegionTriggerBytes=32MB unreached inside the crash
        // window, cycles are HEU-only), every young region is permanently bitmap-less and
        // the old arm collected them wholesale while marked old holders still referenced
        // their objects (f3-livehole census: 64-512/run, reason=region_free, from==latest,
        // targets clustered per region). ZGC: a page is freed only by the generation that
        // proved it empty (zPage.inline.hpp:223-225 seqnum, zGeneration.cpp:216-221).
        // Keep unexamined young in the OLD pass; the YOUNG pass keeps its collect right,
        // so the hangfloor regression (young garbage never reclaimed) cannot return.
        if (!routeMarked &&
            region->GetRegionAllocPtr() > region->GetRegionStart() &&
            (incompleteRoute || liveResidual || !youngRegion || G == Generation::Old)) {
        static std::atomic<size_t> g_fwdUnmarkedKeep{ 0 };
        size_t n = g_fwdUnmarkedKeep.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n - 1)) == 0) {
            LOG(RTLOG_ERROR,
                "[GCV2][fwd-unmarked-keep] n=%zu region=%p start=%#zx alloc=%#zx "
                "route=%u live=%zu — ExemptFromRegion (not marked this cycle)",
                n, region, region->GetRegionStart(), region->GetRegionAllocPtr(),
                static_cast<unsigned>(region->RelocateObserve()), routeMarked ? routeMap->live_bytes() : 0);
        }
        if (youngRegion && StayYoungThisCycle(region)) {
            EnlistStayYoungSurvivor(region);
            return;
        }
        if (youngRegion) {
            AddFlipPromotedPage(region);
        }
        ExemptFromRegion(region);
        return;
        }
    }

    if (!RelocateClaimedPage(region)) {
        // In-place relocation that copied nothing leaves the alloc pointer back at the
        // region start, so the size-walk over [start, start) is empty (RegionManager.cpp:
        // 665-668): the page holds no object to promote, no field to record an edge for and
        // nothing for a later discharge to walk.  ZGC reclaims such a page rather than
        // promoting it -- select_relocation_set hands every relocatable page its own mark
        // did not mark to register_empty_page and free_empty_pages frees it in bulk
        // (zGeneration.cpp:216-221 / 169-176).  And ZGC never routes an in-place relocated
        // from page into the flip-promoted remset walk at all: ZRelocateAddRemsetForFlipPromoted
        // is constructed over flip_promoted_pages() only (zRelocate.cpp:1304), the pages
        // promoted *without* relocation (ZFlipAgePagesTask, zRelocate.cpp:1334-1363).  The
        // in-place relocated from page goes on _in_place_relocate_promoted_pages
        // (zRelocate.cpp:896), and that array is read exactly once -- by
        // ZRelocationSet::reset's destroy_and_clear (zRelocationSet.cpp:200).
        if (region->GetRegionAllocPtr() <= region->GetRegionStart() &&
            !(youngRegion && G == Generation::Old)) {
            // ZGC frees such a page: select_relocation_set hands every relocatable page its
            // own mark did not mark to register_empty_page, and free_empty_pages returns it
            // to the page cache (zGeneration.cpp:216-221 / 169-176).  Keeping it homed on
            // recentFullRegionList instead leaves a *young* region whose alloc pointer has
            // been rewound to its start, and that has two measured consequences:
            //   - every stale pointer into its former contents still answers "my target is
            //     young" to the cross-gen edge walks, so a dead old object's field is
            //     replayed into the remembered set and then refused at ResolveStoreValue
            //     (NW256/256MB 3/3: slot=holder+0x2910 in an old RECENT_FULL region with
            //     holderSurvived=0 holderMarked=0, target=page+0xd100 with allocOff=0);
            //   - the page is re-selected as an empty relocation candidate every cycle --
            //     the same page re-entered this arm a cycle later with entryAllocOff=0.
            // RehomeCompactedInPlaceRegion (RegionManager.cpp:3766) put it on
            // recentFullRegionList, which is why CollectRegion's PrependRegion refused it
            // (GetRegionListOwner() == nullptr).  Take it back off that list first; the
            // ordinary empty-page arm above reaches CollectRegion the same way.
            if (region->GetRegionListOwner() == &recentFullRegionList) {
                const size_t units = region->GetUnitCount();
                recentFullRegionList.DeleteRegion(region);
                RecentFullAccounting::Dequeue(1, units);
            }
            CollectRegion<G>(region);
            return;
        }
        // ZGC zRelocate.cpp:868-896: in-place relocation uses one destination
        // age for both the page and its remembered fields. CompactRegion already
        // applied that age and remembered promoted objects, for workers and root
        // helpers alike. A surviving young page must keep that result here.
        return;
    }

    int32_t rawPointerCount = region->GetRawPointerObjectCount();
    CHECK(rawPointerCount == 0);
    // RelocateClaimedPage already copied each object and called
    // UpdateRemsetForFields for the CAS winner; do not repeat either walk.
    if (!youngRegion) {
        if (ZForwarding* forwarding = generation_forwarding_table(Generation::Old).get(region->GetRegionStart())) {
            forwarding->relocated_remembered_fields_after_relocate();
        }
    }

    {
        // zRelocate.cpp:1137-1152: the page worker finishes objects then
        // mark_done last (ForwardClaimedPage). Do not wait for own done here.
        // zRelocate.cpp:1152 — last act after every object on the page is relocated.
        VerifyRelocatedPage(region, "ForwardRegion");
        region->MarkForwardingDone();
        // ZGC reset_livemap (zForwarding.cpp:71-74 / zPage.cpp:115-117).
        {
            region->reset_livemap();
            if (youngRegion) {
                region->PromoteYoungRegion();
            }
        }
        // After-copy Collect zeros the from payload while live holders still name
        // it. ZGC free_page waits for detach (zRelocate.cpp:1041-1047) and keeps
        // the forwarding table until the next cycle (zRelocationSet.cpp:91-96).
        // cjpm coll_live: first young (cgen=0 fpath=2), then after that Exempt
        // old (cgen=1 fpath=2 route=5 ke=0 gh=1 reason=HEU). Exempt both; the
        // next Assemble/PrepareYoung re-enlists, ghost + entries stay until
        // PrepareFromRegionList. Residuals are settled above before done.
        ExemptFromRegion(region);
        return;
    }
}

template void RegionManager::ForwardFromRegions<Generation::Young>(ZWorkers&);
template void RegionManager::ForwardFromRegions<Generation::Old>(ZWorkers&);
template void RegionManager::ForwardFromRegions<Generation::Young>();
template void RegionManager::ForwardFromRegions<Generation::Old>();
#if defined(MRT_TESTABLE_INTERNALS)
template class ForwardTask<Generation::Young>;
template class ForwardTask<Generation::Old>;
#endif
template void RegionManager::StartForwardFromRegions<Generation::Young>(ZWorkers&);
template void RegionManager::StartForwardFromRegions<Generation::Old>(ZWorkers&);
template void RegionManager::DrainForwardFromRegions<Generation::Young>();
template void RegionManager::DrainForwardFromRegions<Generation::Old>();
template void RegionManager::ForwardClaimedPage<Generation::Young>(ZPage*, ZForwarding*, bool, bool);
template void RegionManager::ForwardClaimedPage<Generation::Old>(ZPage*, ZForwarding*, bool, bool);
template void RegionManager::ForwardRegion<Generation::Young>(ZPage*);
template void RegionManager::ForwardRegion<Generation::Old>(ZPage*);

} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zJNICritical.hpp"

#include <atomic>
#include <chrono>
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.hpp"

namespace MapleRuntime {

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::atomic<ZRelocateQueue::WaitEnterHook> g_waitEnterHook{ nullptr };
}
void ZRelocateQueue::SetWaitEnterHook(WaitEnterHook hook)
{
    g_waitEnterHook.store(hook, std::memory_order_release);
}
#endif

bool ZRelocateQueue::needs_attention() const
{
    return needsAttention.load(std::memory_order_relaxed) != 0;
}

void ZRelocateQueue::inc_needs_attention()
{
    needsAttention.fetch_add(1, std::memory_order_acq_rel);
}

void ZRelocateQueue::dec_needs_attention()
{
    needsAttention.fetch_sub(1, std::memory_order_acq_rel);
}

void ZRelocateQueue::activate(uint32_t workers)
{
    isActive.store(true, std::memory_order_release);
    join(workers);
}

void ZRelocateQueue::deactivate()
{
    isActive.store(false, std::memory_order_release);
    clear();
}

bool ZRelocateQueue::is_active() const
{
    return isActive.load(std::memory_order_acquire);
}

void ZRelocateQueue::join(uint32_t workers)
{
    CHECK_DETAIL(workers != 0 && nworkers == 0 && nsynchronized == 0,
                 "invalid relocate queue join workers=%u nworkers=%u nsync=%u",
                 workers, nworkers, nsynchronized);
    nworkers = workers;
}

void ZRelocateQueue::resize_workers(uint32_t workers)
{
    std::lock_guard<std::mutex> guard(lock);
    nworkers = workers;
}

void ZRelocateQueue::leave()
{
    std::lock_guard<std::mutex> guard(lock);
    nworkers--;
    const bool done = prune();
    const bool last = synchronizeFlag && nworkers == nsynchronized;
    if (done || last) {
        attention.notify_all();
    }
}

void ZRelocateQueue::add_and_wait(ZForwarding* forwarding)
{
    std::unique_lock<std::mutex> guard(lock);
    if (forwarding->is_done()) {
        return;
    }
    queue.append(forwarding);
    if (queue.length() == 1) {
        inc_needs_attention();
        attention.notify_all();
    }
#if defined(MRT_TESTABLE_INTERNALS)
    if (WaitEnterHook hook = g_waitEnterHook.load(std::memory_order_acquire)) {
        hook(forwarding);
    }
#endif
    while (!forwarding->is_done()) {
        attention.wait_for(guard, std::chrono::milliseconds(1));
    }
}

bool ZRelocateQueue::prune()
{
    if (queue.is_empty()) {
        return false;
    }
    bool done = false;
    for (int i = 0; i < queue.length();) {
        ZForwarding* forwarding = queue.at(i);
        if (forwarding->is_done()) {
            done = true;
            queue.delete_at(i);
            completionCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            i++;
        }
    }
    if (queue.is_empty()) {
        dec_needs_attention();
    }
    return done;
}

ZForwarding* ZRelocateQueue::prune_and_claim()
{
    if (prune()) {
        attention.notify_all();
    }
    for (int i = 0; i < queue.length(); i++) {
        ZForwarding* forwarding = queue.at(i);
        if (forwarding->claim()) {
            return forwarding;
        }
    }
    return nullptr;
}

void ZRelocateQueue::synchronize_thread()
{
    nsynchronized++;
    if (nsynchronized == nworkers) {
        attention.notify_all();
    }
}

void ZRelocateQueue::desynchronize_thread()
{
    nsynchronized--;
}

ZForwarding* ZRelocateQueue::synchronize_poll()
{
    if (!needs_attention()) {
        return nullptr;
    }
    std::unique_lock<std::mutex> guard(lock);
    if (ZForwarding* forwarding = prune_and_claim()) {
        return forwarding;
    }
    if (!synchronizeFlag) {
        return nullptr;
    }
    synchronize_thread();
    do {
        attention.wait(guard);
        if (ZForwarding* forwarding = prune_and_claim()) {
            desynchronize_thread();
            return forwarding;
        }
    } while (synchronizeFlag);
    desynchronize_thread();
    return nullptr;
}

void ZRelocateQueue::clear()
{
    if (queue.is_empty()) {
        return;
    }
    queue.clear();
    dec_needs_attention();
}

void ZRelocateQueue::synchronize()
{
    std::unique_lock<std::mutex> guard(lock);
    synchronizeFlag = true;
    inc_needs_attention();
    while (nworkers != nsynchronized) {
        attention.wait(guard);
    }
}

void ZRelocateQueue::desynchronize()
{
    std::lock_guard<std::mutex> guard(lock);
    synchronizeFlag = false;
    dec_needs_attention();
    attention.notify_all();
}

ZRelocateQueue::EnqueueResult ZRelocateQueue::Add(void* region, MAddress from)
{
    auto owner = forwarding_for_page(static_cast<ZPage*>(region));
    CHECK_DETAIL(!owner || owner->covers(from), "relocation request outside forwarding from=%#zx", from);
    return Add(owner);
}

ZRelocateQueue::EnqueueResult ZRelocateQueue::Add(ZForwarding* forwarding)
{
    if (forwarding == nullptr) {
        return { nullptr, false, false, nullptr };
    }
    std::lock_guard<std::mutex> guard(lock);
    if (forwarding->is_done()) {
        return { forwarding, false, true, forwarding };
    }
    if (!isActive.load(std::memory_order_acquire) && !forwarding->claimed().load(std::memory_order_acquire)) {
        return { nullptr, false, false, nullptr };
    }
    for (int i = 0; i < queue.length(); i++) {
        if (queue.at(i) == forwarding) {
            return { forwarding, false, true, forwarding };
        }
    }
    queue.append(forwarding);
    if (queue.length() == 1) {
        inc_needs_attention();
    }
    attention.notify_all();
    return { forwarding, true, true, forwarding };
}

void ZRelocateQueue::Wait(ZForwarding* forwarding)
{
    add_and_wait(forwarding);
}

size_t ZRelocateQueue::Complete(ZForwarding* forwarding)
{
    std::lock_guard<std::mutex> guard(lock);
    (void)forwarding;
    const bool done = prune();
    attention.notify_all();
    return done ? 1 : 0;
}

ZForwarding* ZRelocateQueue::PruneAndClaim()
{
    std::lock_guard<std::mutex> guard(lock);
    return prune_and_claim();
}

ZRelocateQueue::Selection ZRelocateQueue::SynchronizePoll()
{
    std::unique_lock<std::mutex> guard(lock);
    if (ZForwarding* forwarding = prune_and_claim()) {
        return { forwarding, nullptr, false };
    }
    CHECK_DETAIL(nworkers != 0 && nsynchronized < nworkers,
                 "invalid relocation worker synchronization workers=%u synchronized=%u",
                 nworkers, nsynchronized);
    ++nsynchronized;
    if (nsynchronized == nworkers) {
        isActive.store(false, std::memory_order_release);
        nworkers = 0;
        nsynchronized = 0;
        attention.notify_all();
        return { nullptr, nullptr, true };
    }
    for (;;) {
        attention.wait(guard);
        if (!isActive.load(std::memory_order_acquire)) {
            return { nullptr, nullptr, true };
        }
        if (ZForwarding* forwarding = prune_and_claim()) {
            --nsynchronized;
            return { forwarding, nullptr, false };
        }
    }
}

size_t ZRelocateQueue::PendingCount() const
{
    std::lock_guard<std::mutex> guard(lock);
    return static_cast<size_t>(queue.length());
}

size_t ZRelocateQueue::SynchronizedWorkerCount() const
{
    std::lock_guard<std::mutex> guard(lock);
    return nsynchronized;
}

PageAge ZRelocate::compute_to_age(PageAge fromAge)
{
    const uint32_t threshold = Heap::GetHeap().GetCollector().GetGCStats(GCCycleGeneration::YOUNG).tenuringThreshold;
    return ComputeToAge(fromAge, threshold);
}

void ZRelocate::flip_age_pages(ZWorkers& workers, const ZArray<ZPage*>* pages)
{
    class ZFlipAgePagesTask : public ZTask {
    public:
        explicit ZFlipAgePagesTask(const ZArray<ZPage*>* pages)
            : ZTask("ZFlipAgePagesTask"), iter(pages)
        {}
        void work() override
        {
            ZArray<ZPage*> promoted;
            for (ZPage* prev; iter.next(&prev);) {
                const PageAge fromAge = prev->age();
                const PageAge toAge = ZRelocate::compute_to_age(fromAge);
                const bool promotion = toAge == PageAge::old;
                ZPage* const newPage = promotion ? prev->clone_for_promotion() : prev->reset(toAge);
                newPage->reset_livemap();
                if (promotion) {
                    promoted.append(prev);
                }
            }
            Heap::GetHeap().GetCollector().GetZGeneration(GCCycleGeneration::YOUNG)
                .relocation_set().register_flip_promoted(promoted);
        }
    private:
        ZArrayParallelIterator<ZPage*> iter;
    };
    ZFlipAgePagesTask task(pages);
    workers.run(&task);
}

void ZRelocate::barrier_promoted_pages(ZWorkers& workers, const ZArray<ZPage*>* flipPromoted,
                                       const ZArray<ZPage*>* relocatePromoted)
{
    class ZPromoteBarrierTask : public ZTask {
    public:
        ZPromoteBarrierTask(const ZArray<ZPage*>* flip, const ZArray<ZPage*>* relocate)
            : ZTask("ZPromoteBarrierTask"), flipIter(flip), relocateIter(relocate)
        {}
        void work() override
        {
            auto promoteBarriers = [](ZArrayParallelIterator<ZPage*>* iter) {
                for (ZPage* page; iter->next(&page);) {
                    page->object_iterate([](BaseObject* obj) {
                        ZIterator::basic_oop_iterate_safe(obj, [](RefField<>& field) {
                            ZBarrier::promote_barrier_on_young_oop_field(
                                reinterpret_cast<volatile zpointer*>(&field));
                        });
                    });
                }
            };
            promoteBarriers(&flipIter);
            promoteBarriers(&relocateIter);
        }
    private:
        ZArrayParallelIterator<ZPage*> flipIter;
        ZArrayParallelIterator<ZPage*> relocateIter;
    };
    ZPromoteBarrierTask task(flipPromoted, relocatePromoted);
    workers.run(&task);
}

} // namespace MapleRuntime
