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
#include "Heap/Verify/VerifyOption.h"
#include "Heap/Verify/VerifyRememberedSet.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/VerifyRoots.h"
#include "Heap/Verify/Zap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/NwDropAudit.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/Stw2CurrentAudit.h"
#include "Heap/Verify/NullRouteCaller.h"
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
struct CopierRouteMint {
    static CopierRouteToken Make() { return CopierRouteToken(); }
};
namespace WCollectorInternal {
bool MinorYoungFlipOff()
{
    static const bool off = []() {
        const char* value = std::getenv("MRT_GCV2_MINOR_YOUNG_FLIP");
        return value != nullptr && std::strcmp(value, "0") == 0;
    }();
    return off;
}

} // namespace WCollectorInternal
// Frame-colour census after relocate-start flip. Compile-time off; no MRT_GCV2_ env.
//
// It answered its question and the answer is stable: across 550 flips the stack roots held
// zero old-epoch colours and zero current-epoch colours, because runtime stack roots are
// plain by ABI (BaseObject.h, "Stack/runtime roots are uncoloured RootSlots"). Leaving it
// compiled in would put a full walk of every mutator stack, plus a log line per mutator,
// inside the relocate-start pause.
//
// Kept rather than deleted because the same walk answers the follow-up question -- whether a
// plain stack root still names a from-object after relocation, which no colour bit can flag.
static constexpr bool kFrameColourCensus = false;

static void CensusFrameColoursAfterFlip(const char* site, uintptr_t prevRemap)
{
    if (!kFrameColourCensus) {
        return;
    }
    const uintptr_t loadBad = ::g_cjLoadBadMask;
    const uintptr_t curRemap = ColourPredicates::current_remapped(loadBad);
    size_t nMut = 0;
    size_t nManaged = 0;
    size_t nSlot = 0;
    size_t nOld = 0;
    size_t nNew = 0;
    size_t nPlainNull = 0;
    size_t nOther = 0;
    size_t oldD0 = 0;
    size_t oldD1 = 0;
    size_t oldD2 = 0;
    size_t oldD3p = 0;
    size_t maxOldDepth = 0;
    MutatorManager::Instance().VisitAllMutators([&](Mutator& mutator) {
        ++nMut;
        if (!mutator.IsManagedContext()) {
            return;
        }
        mutator.MutatorLock();
        StackFrameCursor cursor(mutator.GetUnwindContext());
        size_t depth = 0;
        RootVisitor visitor = [&](ObjectRef& root) {
            const uintptr_t value = raw(root.LoadPlain());
            ++nSlot;
            const uintptr_t remap = value & REMAP_COLOUR_MASK;
            if (value == 0 || remap == 0) {
                ++nPlainNull;
            } else if (ColourPredicates::is_load_good(value, loadBad)) {
                ++nNew;
            } else if (prevRemap != 0 && remap == prevRemap) {
                ++nOld;
                if (depth <= 3) {
                    ++oldD0;
                } else if (depth <= 7) {
                    ++oldD1;
                } else if (depth <= 15) {
                    ++oldD2;
                } else {
                    ++oldD3p;
                }
                if (depth > maxOldDepth) {
                    maxOldDepth = depth;
                }
            } else {
                ++nOther;
            }
        };
        while (!cursor.Done()) {
            const FrameInfo* frame = cursor.CurrentFrame();
            const bool managed = frame != nullptr && frame->GetFrameType() == FrameType::MANAGED;
            if (managed) {
                ++nManaged;
                ++depth;
            }
            cursor.ProcessOne(visitor, mutator);
        }
        mutator.MutatorUnlock();
    });
    static std::atomic<size_t> cycle{0};
    const size_t n = cycle.fetch_add(1, std::memory_order_relaxed) + 1;
    LOG(RTLOG_ERROR,
        "[FRAMECOLOUR] site=%s cycle=%zu mut=%zu managedFrames=%zu slots=%zu "
        "oldColour=%zu newColour=%zu plainNull=%zu other=%zu "
        "prevRemap=%#lx curRemap=%#lx loadBad=%#lx "
        "oldDepth[0-3]=%zu [4-7]=%zu [8-15]=%zu [16+]=%zu maxOldDepth=%zu stw=%d",
        site, n, nMut, nManaged, nSlot, nOld, nNew, nPlainNull, nOther,
        static_cast<unsigned long>(prevRemap), static_cast<unsigned long>(curRemap),
        static_cast<unsigned long>(loadBad), oldD0, oldD1, oldD2, oldD3p, maxOldDepth,
        MutatorManager::Instance().WorldStopped() ? 1 : 0);
}

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

    RegionInfo* regionInfo = nullptr;
    if (RegionInfo::InGhostFromRegion(obj)) {
        regionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<uintptr_t>(obj));
    } else {
        regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    }
    return regionInfo->IsUnmovableFromRegion();
}
template<bool forward>
bool WCollector::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                       BaseObject*& toObj) const
{
    RefField<> oldRef(field);
    if (IsLoadBad(oldRef)) {
        fromObj = to_object(oldRef.GetTargetObject());
        if (forward) {
            toObj = const_cast<WCollector*>(this)->TryForwardObject(fromObj);
        } else {
            toObj = FindToVersion(fromObj);
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
    return TryUpdateRefFieldImpl<false>(obj, field, oldRef, newRef);
}

bool WCollector::TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<true>(obj, field, oldRef, newRef);
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
        if (host != nullptr && IsGhostFromObject(host) && !IsUnmovableFromObject(host)) {
            BaseObject* toHost = TryForwardObject(host);
            if (toHost != nullptr && toHost != host) {
                BaseObject* toInterior = reinterpret_cast<BaseObject*>(
                    reinterpret_cast<uintptr_t>(toHost) +
                    (reinterpret_cast<uintptr_t>(oldObj) - reinterpret_cast<uintptr_t>(host)));
                HealRootWriteback(root, toInterior, HealSite::WCollectorForwardRawInterior);
                return toInterior;
            }
        }
        HealRootWriteback(root, oldObj, HealSite::WCollectorPreserveRawInterior);
        return oldObj;
    }
    if (IsGhostFromObject(oldObj)) {
        BaseObject* toVersion = TryForwardObject(oldObj);
        if (toVersion == nullptr) {
            return oldObj;
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

    auto remapField = [&](RefField<>& field, size_t& seen, size_t& coloured, size_t& remapped,
                          size_t& doubleBad) {
        ++seen;
        RefField<> oldField(field);
        const uintptr_t rawVal = raw(oldField.GetFieldValue());
        const auto kind = RemapYoungRootsLogic::Classify(rawVal, youngMask, oldMask);
        if (kind == RemapYoungRootsLogic::Kind::Uncoloured) {
            return;
        }
        ++coloured;
        if (kind == RemapYoungRootsLogic::Kind::DoubleBad) {
            ++doubleBad;
        }
        if (kind == RemapYoungRootsLogic::Kind::LoadGood) {
            return;
        }
        BaseObject* latest = make_load_good(oldField);
        if (!Heap::IsHeapAddress(latest)) {
            return;
        }
        if (!Collector::PlausibleManagedObjectGate("RemapYoungRoots", latest)) {
            return;
        }
        RefField<> newField = GetAndTryTagRefField(latest);
        if (oldField.GetFieldValue() != newField.GetFieldValue()) {
            if (HealSlot(field, oldField.GetFieldValue(), newField.GetFieldValue(),
                         HealSite::WCollectorRemapYoungRoots)) {
                ++remapped;
            }
        }
    };

    std::unordered_set<MAddress> remset = Heap::GetHeap().GetRememberedSet().Snapshot();
    for (MAddress slot : remset) {
        if (slot == 0) {
            continue;
        }
        remapField(*reinterpret_cast<RefField<>*>(slot), remsetSeen, remsetColoured, remsetRemapped,
                   remsetDoubleBad);
    }

    RootSlotVisitor staticVisitor = [&](RootSlot& root) {
        ++staticSeen;
        const uintptr_t rawVal = raw(root.LoadPlain());
        const auto kind = RemapYoungRootsLogic::Classify(rawVal, youngMask, oldMask);
        if (kind == RemapYoungRootsLogic::Kind::Uncoloured) {
            return;
        }
        ++staticColoured;
        if (kind == RemapYoungRootsLogic::Kind::DoubleBad) {
            ++staticDoubleBad;
        }
        if (kind == RemapYoungRootsLogic::Kind::LoadGood) {
            return;
        }
        RefField<> asField(to_zpointer(rawVal));
        BaseObject* latest = make_load_good(asField);
        if (!Heap::IsHeapAddress(latest)) {
            return;
        }
        if (!Collector::PlausibleManagedObjectGate("RemapYoungRoots.static", latest)) {
            return;
        }
        HealRootWriteback(root, latest, HealSite::WCollectorRemapYoungRoots);
        ++staticRemapped;
    };
    Heap::GetHeap().VisitStaticRoots(staticVisitor);

    MutatorManager::Instance().VisitAllMutators([&](Mutator& mutator) {
        if (!mutator.IsManagedContext()) {
            return;
        }
        mutator.MutatorLock();
        StackFrameCursor cursor(mutator.GetUnwindContext());
        RootVisitor visitor = [&](ObjectRef& root) {
            ++stackSeen;
            if ((raw(root.LoadPlain()) & REMAP_COLOUR_MASK) != 0) {
                ++stackColoured;
            }
        };
        while (!cursor.Done()) {
            cursor.ProcessOne(visitor, mutator);
        }
        mutator.MutatorUnlock();
    });

    LOG(RTLOG_ERROR,
        "[A8REMAP] remset seen=%zu coloured=%zu remapped=%zu doubleBad=%zu "
        "static seen=%zu coloured=%zu remapped=%zu doubleBad=%zu "
        "stack seen=%zu coloured=%zu flipSeq=%lu",
        remsetSeen, remsetColoured, remsetRemapped, remsetDoubleBad, staticSeen, staticColoured,
        staticRemapped, staticDoubleBad, stackSeen, stackColoured,
        static_cast<unsigned long>(FlipSeq().load(std::memory_order_relaxed)));
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
    auto it = cycleRefWorkStack.begin();
    std::unordered_map<BaseObject*, std::list<BaseObject*>> tmp;
    while (it != cycleRefWorkStack.end()) {
        BaseObject* exportObj = it->first;
        BaseObject* latest = exportObj;
        if (IsGhostFromObject(exportObj) && !IsUnmovableFromObject(exportObj)) {
            latest = ForwardObject(exportObj);
        }
        for (auto &externObj : it->second) {
            if (IsGhostFromObject(externObj) && !IsUnmovableFromObject(externObj)) {
                BaseObject* toObj = ForwardObject(externObj);
                externObj = toObj;
            }
        }
        if (latest != exportObj) {
            tmp[latest] = it->second;
            it = cycleRefWorkStack.erase(it);
        } else {
            it++;
        }
    }
    if (!tmp.empty()) {
        cycleRefWorkStack.insert(tmp.begin(), tmp.end());
    }
}

void WCollector::PreforwardAllResurrectExportFromObjects()
{
    std::unordered_set<BaseObject*> tmp;
    std::lock_guard<std::mutex> lg(resurrectExportMtx);
    auto it = resurrectedExportObjectes.begin();
    while (it != resurrectedExportObjectes.end()) {
        BaseObject* exportObj = *it;
        BaseObject* latest = exportObj;
        if (IsGhostFromObject(exportObj) && !IsUnmovableFromObject(exportObj)) {
            latest = ForwardObject(exportObj);
        }
        if (latest != exportObj) {
            tmp.insert(latest);
            it = resurrectedExportObjectes.erase(it);
        } else {
            it++;
        }
    }
    if (!tmp.empty()) {
        resurrectedExportObjectes.insert(tmp.begin(), tmp.end());
    }
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
        CensusFrameColoursAfterFlip("full",
            (ZPointerRemappedYoungMask ^ REMAP_COLOUR_MASK) &
                (ZPointerRemappedOldMask ^ REMAP_COLOUR_MASK));
    }

    GCThreadPool* threadPool = GetThreadPool();
    MRT_ASSERT(threadPool != nullptr, "thread pool is null");
    // forward and fix cj future objects
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardConcurrencyModelRoots(); }));

    // forward and fix finalizer roots.
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardFinalizerProcessorRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllExportFromRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardStaticRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardDiscoveredExternObjects(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllResurrectExportFromObjects(); }));
    threadPool->Start();
    threadPool->WaitFinish();
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
bool WCollector::CasInstallResolvedTarget(RefField<>& field, MAddress expected, BaseObject* target,
                                          HealSite site, HealNull allowNull) const
{
    zpointer desired = RootSlotWriteback(target, field).GetFieldValue();
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
    auto plannedTo = [this, stw](BaseObject* from) -> BaseObject* {
        return stw != nullptr ? PlanRouteUnderStw(from, *stw).dest : FindToVersion(from);
    };

    RefField<> value(field);
    BaseObject* object = to_object(value.GetTargetObject());
    if (!IsOldPointer(value)) {
        // zBarrier.inline.hpp:318-340 resolves through the forwarding before
        // self-healing the concrete oop slot. zRelocate.cpp:1018-1047 keeps
        // that receipt available until relocated fields have been repaired.
        // After-copy Exempt makes our page UNMOVABLE_FROM before this bulk
        // ref-fix runs, so the ghost-only arm below cannot observe a completed
        // copy. Consume its still-active receipt before PrepareForwardTable
        // retires the table.
        if (object != nullptr && Heap::IsHeapAddress(object)) {
            RegionInfo* fromRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
            ZForwarding* forwarding = fromRegion != nullptr && fromRegion->IsForwardingDone()
                ? ForwardingTable::GetEntries(reinterpret_cast<MAddress>(object))
                : nullptr;
            if (forwarding != nullptr) {
                const MAddress receipt = forwarding->find(reinterpret_cast<MAddress>(object));
                const MAddress live = receipt == 0 ? 0 : forwarding->resolve_live(receipt);
                if (live != 0) {
                    BaseObject* to = reinterpret_cast<BaseObject*>(live);
                    MAddress expected = raw(value.GetFieldValue());
                    (void)CasInstallResolvedTarget(field, expected, to,
                                                   HealSite::WCollectorMinorResolveLoadGoodForward);
                    return to;
                }
                if (receipt != 0) {
                    ZForwarding::StaleToLifeCount().fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        // hangfloor: plain stack/reg roots (and any load-good colour) make IsOldPointer
        // structurally false — that predicate needs IsLoadBad, which plain never is.
        // After young prepare, from-space still needs ghost routing; without it
        // FixMinor/VisitMinor keep the from address and young GC thrash (10/10 HANG).
        if (object != nullptr && Heap::IsHeapAddress(object) && IsGhostFromObject(object) &&
            !IsUnmovableFromObject(object)) {
            // installdomain: admit into route domain before any install/forward consumes it.
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), object);
            BaseObject* to = plannedTo(object);
            // satbfix: only install a to that is a live object tip; a RECENT_FULL hole
            // address must not be written into the slot (same invalid_object family).
            if (to != nullptr && Heap::IsHeapAddress(to)) {
                RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(to));
                if (toRegion != nullptr && !toRegion->IsFreeRegion() && !toRegion->IsGarbageRegion() &&
                    to->IsValidObject()) {
                    MAddress expected = raw(value.GetFieldValue());
                    (void)CasInstallResolvedTarget(field, expected, to,
                                                   HealSite::WCollectorMinorResolveLoadGoodForward);
                    return to;
                }
            }
        }
        return object;
    }
    // Minor path must not call FindLatestVersion: after a full GC Flip, remset/root
    // slots can still hold one-gen-stale tags whose from-copy was reclaimed (ghost
    // gone, header zeroed). F5 would abort a detector path; here we soft-resolve:
    //   routed to-version → RootSlotWriteback(to)
    //   unmoved valid from → RootSlotWriteback(from)
    //   true dead (free/garbage/null) → null the slot (caller drops the edge)
    //   active-region bad tip → leave alone / return from (never invent null)
    // N2: CAS (FYS=1 multi-writer safe; product default FYS=1).
    // hangfloor: use RootSlotWriteback so heap remset/fields keep Phase C colour.
    MAddress expected = raw(value.GetFieldValue());
    BaseObject* to = plannedTo(object);
    bool toActiveBadTip = false;
    if (to != nullptr && Heap::IsHeapAddress(to)) {
        RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(to));
        if (toRegion != nullptr && !toRegion->IsFreeRegion() && !toRegion->IsGarbageRegion()) {
            if (to->IsValidObject()) {
                (void)CasInstallResolvedTarget(field, expected, to,
                                               HealSite::WCollectorMinorResolveOldForward);
                return to;
            }
            // Active region, tip invalid — same family as F3 invalid_object (rtype=2).
            toActiveBadTip = true;
        }
    }
    if (Heap::IsHeapAddress(object)) {
        RegionInfo* fromRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (fromRegion != nullptr && !fromRegion->IsFreeRegion() && !fromRegion->IsGarbageRegion() &&
            object->IsValidObject()) {
            // installdomain: identity arm is the A-only fork (IsValidObject without liveInfo0).
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), object);
            (void)CasInstallResolvedTarget(field, expected, object,
                                           HealSite::WCollectorMinorResolveOldIdentity);
            return object;
        }
    }
    // Non-heap (static/binary constants, etc.): FindToVersion nullptr means "not a heap
    // object", not "dead residue". Return as-is; never CAS (slot may be RO static root).
    // See reports/REPORT-zcdnull.md — CAS-null on RO static SEGV (si_addr=&field).
    if (object != nullptr && !Heap::IsHeapAddress(object)) {
        return object;
    }
    // Active-region bad tip with no valid from: leave the old-tag alone (satbfix).
    // Mutator read path still routes; inventing null zeros live holder fields.
    if (toActiveBadTip) {
        static std::atomic<size_t> g_resolveBadTipSkip{ 0 };
        size_t n = g_resolveBadTipSkip.fetch_add(1, std::memory_order_relaxed);
        if (n < 16) {
            VLOG(REPORT,
                 "[GCV2][minor-badtip] field=%p from=%p to=%p — leave old-tag",
                 &field, object, to);
        }
        return object;
    }
    static std::atomic<size_t> g_staleOldTagLogged{ 0 };
    size_t n = g_staleOldTagLogged.fetch_add(1, std::memory_order_relaxed);
    if (n < 16) {
        VLOG(REPORT,
             "[GCV2][minor-stale-oldtag] field=%p raw=%#zx from=%p to=%p "
             "(drop; full-GC remset/root residue after Flip)",
             &field, static_cast<size_t>(raw(value.GetFieldValue())), object, to);
    }
    bool holderLiveBySnapshot = SlotHeldByLiveObject(&field);
    if (KeepRememberedHolder(holderLiveBySnapshot, holderIsCurrentMinorRoot)) {
        if (!holderLiveBySnapshot && holderIsCurrentMinorRoot && preservedByCurrentRoot != nullptr) {
            *preservedByCurrentRoot = true;
        }
        return object;
    }
    NoteNullslotWrite("fix_resolve_cas", nullptr, &field, object, to, &g_nullslotResolve);
    (void)CasInstallResolvedTarget(field, expected, nullptr, HealSite::WCollectorMinorResolveDead,
                                   HealNull::Allow);
    return nullptr;
}

// rootgate positive control (default off: MRT_GCV2_ROOTGATE_ACCOUNT=1 or MRT_GCV2_DIAG=rootgate).
// The liveness gate added to the load-good arm of ResolveMinorReference(RootSlot&) below is
// only allowed to claim it fixes something if it can be shown to refuse something. Counting
// every refusal here is what makes "0" reportable as "this gate never fired on this workload"
// instead of being read as "the defect is fixed". The gate itself is unconditional product
// code; only the accounting is gated, so the arm cannot be silently disabled by the env.
static std::atomic<size_t> g_rootGateRefused{ 0 };
static std::atomic<size_t> g_rootGateNoRegion{ 0 };
static std::atomic<size_t> g_rootGateFreeOrGarbage{ 0 };
static std::atomic<size_t> g_rootGateBadTip{ 0 };
static std::atomic<bool> g_rootGateAtexit{ false };

static void DumpRootGateSummary()
{
    LOG(RTLOG_ERROR,
        "[GCV2][rootgate] SUMMARY refused=%zu no_region=%zu free_or_garbage=%zu bad_tip=%zu "
        "(load-good arm of ResolveMinorReference(RootSlot&); 0 = gate never fired on this workload)",
        g_rootGateRefused.load(std::memory_order_relaxed), g_rootGateNoRegion.load(std::memory_order_relaxed),
        g_rootGateFreeOrGarbage.load(std::memory_order_relaxed), g_rootGateBadTip.load(std::memory_order_relaxed));
}

static bool RootGateAccountOn()
{
    static const bool on = DiagGate::LegacyOrToken("MRT_GCV2_ROOTGATE_ACCOUNT", "rootgate");
    return on;
}

static void NoteRootGateRefusal(const RootSlot& root, BaseObject* from, BaseObject* to, RegionInfo* toRegion)
{
    if (!RootGateAccountOn()) {
        return;
    }
    bool expected = false;
    if (g_rootGateAtexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        (void)std::atexit([]() { DumpRootGateSummary(); });
    }
    if (toRegion == nullptr) {
        g_rootGateNoRegion.fetch_add(1, std::memory_order_relaxed);
    } else if (toRegion->IsFreeRegion() || toRegion->IsGarbageRegion()) {
        g_rootGateFreeOrGarbage.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_rootGateBadTip.fetch_add(1, std::memory_order_relaxed);
    }
    size_t n = g_rootGateRefused.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16) {
        VLOG(REPORT,
             "[GCV2][rootgate] refuse root=%p from=%p to=%p toRegion=%p free=%u garbage=%u validTip=%u "
             "— would have installed a non-live to-address into a root slot",
             static_cast<const void*>(&root), from, to, toRegion,
             static_cast<unsigned>(toRegion != nullptr && toRegion->IsFreeRegion()),
             static_cast<unsigned>(toRegion != nullptr && toRegion->IsGarbageRegion()),
             static_cast<unsigned>(to != nullptr && to->IsValidObject()));
    }
    // The workloads this runs under die on SIGSEGV, which skips atexit. Emit at every power
    // of two so the count survives the crash it is meant to explain.
    if ((n & (n - 1)) == 0) {
        DumpRootGateSummary();
    }
}

BaseObject* WCollector::ResolveMinorReference(RootSlot& root, const ScopedStopTheWorld* stw) const
{
    auto plannedTo = [this, stw](BaseObject* from) -> BaseObject* {
        return stw != nullptr ? PlanRouteUnderStw(from, *stw).dest : FindToVersion(from);
    };

    zaddress_unsafe observed = root.LoadPlain();
    HeapSlot<> observedBits(to_zpointer(raw(observed)));
    BaseObject* object = to_object(observedBits.GetTargetObject());
    const bool isOld = IsOldPointer(observedBits);
    {
        size_t en = g_resolveRootEntry.fetch_add(1, std::memory_order_relaxed);
        if (isOld) {
            g_resolveRootOld.fetch_add(1, std::memory_order_relaxed);
        }
        // Entry sample: prove FixMinorRootSlots reaches this function before Mode A.
        if (NullslotProbeEnabled() && en < 32) {
            GCPhase phase = Heap::GetHeap().GetGCPhase();
            std::fprintf(stderr,
                         "[GCV2][nullslot] path=resolve_root_entry n=%zu root=%p obj=%p raw=%#zx "
                         "isOld=%u phase=%s(%u)\n",
                         en, static_cast<void*>(&root), object, static_cast<size_t>(raw(observed)),
                         static_cast<unsigned>(isOld), Collector::GetGCPhaseName(phase),
                         static_cast<unsigned>(phase));
            std::fflush(stderr);
        }
    }
    if (!isOld) {
        if (object != nullptr && Heap::IsHeapAddress(object) && IsGhostFromObject(object) &&
            !IsUnmovableFromObject(object)) {
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), object);
            BaseObject* to = plannedTo(object);
            // satbfix parity. Three arms resolve a from-object to a to-address and install it;
            // this was the only one without the liveness gate:
            //   RefField<>& overload, load-good arm  :2641-2644  — has it, and its comment
            //       names the failure: "a RECENT_FULL hole address must not be written into
            //       the slot" (same invalid_object family as F3 rtype=2).
            //   this overload, old-tag arm           :2758-2761  — has it.
            //   this overload, load-good arm         (here)      — had only IsHeapAddress.
            // Not a design trade-off: this arm writes a *root* slot, so an unchecked
            // to-address is committed before VisitMinorRoots' PlausibleManagedObjectGate
            // (:2933) ever inspects the value, and that gate is a tip check which cannot see
            // free/garbage region state anyway. Refusal falls through to `return object;`,
            // which is exactly what both sibling arms do.
            if (to != nullptr && Heap::IsHeapAddress(to)) {
                RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(to));
                if (toRegion != nullptr && !toRegion->IsFreeRegion() && !toRegion->IsGarbageRegion() &&
                    to->IsValidObject()) {
                    HealRootWriteback(root, to, HealSite::WCollectorResolveRootLoadGoodForward);
                    return to;
                }
                NoteRootGateRefusal(root, object, to, toRegion);
            }
        }
        return object;
    }

    BaseObject* to = plannedTo(object);
    if (to != nullptr && Heap::IsHeapAddress(to)) {
        RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(to));
        if (toRegion != nullptr && !toRegion->IsFreeRegion() && !toRegion->IsGarbageRegion() &&
            to->IsValidObject()) {
            HealRootWriteback(root, to, HealSite::WCollectorResolveRootOldForward);
            return to;
        }
    }
    if (Heap::IsHeapAddress(object)) {
        RegionInfo* fromRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (fromRegion != nullptr && !fromRegion->IsFreeRegion() && !fromRegion->IsGarbageRegion() &&
            object->IsValidObject()) {
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), object);
            HealRootWriteback(root, object, HealSite::WCollectorNormalizeOldRoot);
            return object;
        }
    }
    if (object != nullptr && !Heap::IsHeapAddress(object)) {
        return object;
    }
    // rootdrop probe: classify why to/from both failed live predicates before drop-null.
    // Gate = MRT_GCV2_NULLSLOT (same as nullslot); default off.
    {
        RegionInfo* toRegion =
            (to != nullptr && Heap::IsHeapAddress(to))
                ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(to))
                : nullptr;
        RegionInfo* fromRegion =
            (object != nullptr && Heap::IsHeapAddress(object))
                ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object))
                : nullptr;
        const char* toWhy = nullptr;
        if (to == nullptr) {
            toWhy = "to_null";
        } else if (!Heap::IsHeapAddress(to)) {
            toWhy = "to_not_heap";
        } else {
            toWhy = ClassifyRootLiveFail(to, toRegion);
        }
        const char* fromWhy = ClassifyRootLiveFail(object, fromRegion);
        NoteResolveRootNull(&root, object, to, fromRegion, toRegion, toWhy, fromWhy);
    }
    static std::atomic<size_t> g_staleOldRootLogged{ 0 };
    size_t n = g_staleOldRootLogged.fetch_add(1, std::memory_order_relaxed);
    if (n < 16) {
        VLOG(REPORT,
             "[GCV2][minor-stale-oldtag] root=%p raw=%#zx from=%p to=%p "
             "(drop; full-GC root residue after Flip)",
             &root, static_cast<size_t>(raw(observed)), object, to);
    }
    g_resolveRootHealNull.fetch_add(1, std::memory_order_relaxed);
    HealRoot(root, zaddress::null, HealSite::WCollectorResolveDeadRoot, HealNull::Allow);
    return nullptr;
}
bool WCollector::FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase,
                                      const ScopedStopTheWorld* stw,
                                      bool holderIsCurrentMinorRoot) const
{
    // N1: major-style CAS tolerate (TryUpdateRefFieldImpl family). Under multi-worker
    // fix, CAS fail is normal (peer already updated) — abort assertion was serial-only.
    RefField<> oldField(field);
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
    // write plain only. Storage is still HeapSlot (fields/remset) — DerivedSlot cannot CAS
    // into it; CasInstallInteriorPlain names the (host,offset) provenance (derivedtype).
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
        if (host != nullptr && IsGhostFromObject(host) && !IsUnmovableFromObject(host)) {
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), host);
            BaseObject* toHost = const_cast<WCollector*>(this)->ForwardObject(host);
            if (toHost != nullptr && toHost != host) {
                size_t offset = static_cast<size_t>(reinterpret_cast<uintptr_t>(target) -
                                                    reinterpret_cast<uintptr_t>(host));
                MAddress oldVal = raw(oldField.GetFieldValue());
                MAddress plainVal = reinterpret_cast<MAddress>(toHost) + offset;
                if (oldVal != plainVal) {
                    (void)CasInstallInteriorPlain(field, to_zpointer(oldVal), toHost, offset,
                                                  HealSite::WCollectorMinorFixInteriorForward);
                }
                return true;
            }
        }
        // Gate rejected; host unknown or not forwarded — still plain interior (03fc21ed).
        MAddress oldVal = raw(oldField.GetFieldValue());
        MAddress plainVal = reinterpret_cast<MAddress>(target);
        if (oldVal != plainVal) {
            (void)CasInstallInteriorPlain(field, to_zpointer(oldVal), target,
                                          HealSite::WCollectorMinorFixInteriorPreserve);
        }
        return false;
    }
    HeapSlot<> oldBits(oldField);
    BaseObject* oldObj = to_object(oldBits.GetTargetObject());
    // resolveto: Resolve already rewrote FROM→TO. TO sits in a Compacted ghost
    // (in-place pack). Forward/Admit indexes liveInfo0 by from-offset — feeding TO
    // misses → leave-alone. Keep the already-installed to.
    const bool alreadyTo = (target != oldObj);
    BaseObject* current = target;
    if (!alreadyTo && IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
        // installdomain: route-domain grant before ForwardObject → GetRoute.
        EnsureRouteDomainMembership(const_cast<WCollector*>(this), target);
        current = const_cast<WCollector*>(this)->ForwardObject(target);
    }
    // ForwardObject null = movable ghost with no to-version (survivor-gate miss).
    // Drop the edge; do not reinstall the from address that is about to be reclaimed.
    if (current == nullptr) {
        if (KeepRememberedHolder(SlotHeldByLiveObject(&field), holderIsCurrentMinorRoot)) {
            return false;
        }
        MAddress oldVal = raw(field.GetFieldValue());
        (void)HealSlot(field, to_zpointer(oldVal), zpointer::null,
                       HealSite::WCollectorMinorFixForwardNull, HealNull::Allow);
        return false;
    }
    // ForwardObject may return the same interior if gated; re-check before colouring.
    if (!Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot.postfwd", current)) {
        MAddress oldVal = raw(field.GetFieldValue());
        MAddress plainVal = reinterpret_cast<MAddress>(current);
        if (oldVal != plainVal) {
            (void)CasInstallInteriorPlain(field, to_zpointer(oldVal), current,
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
    BaseObject* target = ResolveMinorReference(root, stw);
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    if (!Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot", target)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(target);
        if (host != nullptr && IsGhostFromObject(host) && !IsUnmovableFromObject(host)) {
            // grant-before-route: paint only; bulk grant pass already did peers.
            (void)ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), host);
            BaseObject* toHost = const_cast<WCollector*>(this)->ForwardObject(host);
            if (toHost != nullptr && toHost != host) {
                BaseObject* toInterior = reinterpret_cast<BaseObject*>(
                    reinterpret_cast<uintptr_t>(toHost) +
                    (reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(host)));
                HealRootWriteback(root, toInterior, HealSite::WCollectorFixRootInteriorForward);
                return true;
            }
        }
        HealRootWriteback(root, target, HealSite::WCollectorPreserveRootInterior);
        return false;
    }
    HeapSlot<> oldBits(to_zpointer(oldValue));
    BaseObject* oldObj = to_object(oldBits.GetTargetObject());
    // resolveto: Resolve already remapped FROM→TO. Do not Admit the to-address
    // against the from-offset bitmap (offpast same-target probe: sameObj=0).
    const bool alreadyTo = (target != oldObj);
    BaseObject* current = target;
    if (!alreadyTo && IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
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
        BaseObject* viaTable = FindToVersion(target);
        if (viaTable != nullptr && viaTable != target && Heap::IsHeapAddress(viaTable) &&
            viaTable->IsValidObject()) {
            HealRootWriteback(root, viaTable, HealSite::WCollectorFixRootForwarded);
            return true;
        }
        static std::atomic<size_t> g_ysRootLeaveAlone{ 0 };
        size_t la = g_ysRootLeaveAlone.fetch_add(1, std::memory_order_relaxed) + 1;
        if (la <= 32) {
            LOG(RTLOG_ERROR,
                "[GCV2][youngstatic] root_fwd_null_leave_alone n=%zu root=%p target=%p "
                "(ZGC never-heal-null; table miss after force-domain)",
                la, static_cast<void*>(&root), static_cast<void*>(target));
        }
        return false;
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
    if (Heap::IsHeapAddress(knownBase) &&
        Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot.derivedBase", knownBase) &&
        IsGhostFromObject(knownBase) && !IsUnmovableFromObject(knownBase)) {
        (void)ForceRootRouteDomainWhileForwardable(const_cast<WCollector*>(this), knownBase);
        currentBase = const_cast<WCollector*>(this)->ForwardObject(knownBase);
        if (currentBase == nullptr) {

            return false;
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
        NullRouteCaller::ScopedEdge _edge("root", nullptr, reinterpret_cast<uintptr_t>(&root));
        NullRouteCaller::ScopedTag _nrTag("FixMinorEvacuatedSlot");
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
        NullRouteCaller::ScopedEdge _edge("liveobj", object, reinterpret_cast<uintptr_t>(&field));
        NullRouteCaller::ScopedTag _nrTag("FixMinorEvacuatedSlot");
        (void)FixMinorEvacuatedSlot(field, nullptr, stw);
    });

}

// R2: parallel ⑦ young.ref_fix — index-shard reachableObjects + remset slots;
// root families = 6 family-level tasks (static not split). Template = A2 stwpar2.
// Env: MRT_GCV2_REFFIX_WORKERS, MRT_GCV2_REFFIX_FORCE_SERIAL.
void WCollector::FixMinorRootSlotsParallel(GCThreadPool* threadPool, const ScopedStopTheWorld* stw)
{
    // statresid: serial grant-before-route for all root families (must complete before
    // any parallel Forward/Route freezes a shared region's geometry).
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

        } else if (!Collector::PlausibleManagedObjectGate("statresid.grant_pass.par", obj)) {
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

    // 5 root families as separate tasks (static kept whole — mutex+dedup set).
    // Order matches serial FixMinorRootSlots.
    auto rootFix = [this, stw](ObjectRef& root) {
        NullRouteCaller::ScopedEdge _edge("root", nullptr, reinterpret_cast<uintptr_t>(&root));
        NullRouteCaller::ScopedTag _nrTag("FixMinorEvacuatedSlot");
        (void)FixMinorEvacuatedSlot(root, stw);
    };
    DerivedPtrVisitor derivedFix = [this, stw](BasePtrType basePtr, DerivedSlot& derived) {
        BaseObject* knownBase = is_null(basePtr) ? nullptr :
            to_object(safe(uncolor_bits(to_zpointer(raw(basePtr)))));
        (void)FixMinorEvacuatedSlot(derived, knownBase, stw);
    };
    threadPool->AddWork(new (std::nothrow) LambdaWork([this, rootFix, derivedFix](size_t) {
        RootVisitor rawRootVisitor = rootFix;
        DerivedPtrVisitor derivedVisitor = derivedFix;
        MutatorManager::Instance().VisitAllMutators(
            [&rawRootVisitor, &derivedVisitor](Mutator& mutator) {
                mutator.VisitHeapReferences(rawRootVisitor, derivedVisitor);
            });
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this, rootFix](size_t) {
        RootVisitor rawRootVisitor = rootFix;
        Heap::GetHeap().VisitStaticRoots(rawRootVisitor);
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this, rootFix](size_t) {
        RootVisitor rawRootVisitor = rootFix;
        Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rawRootVisitor);
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this, rootFix](size_t) {
        RootVisitor rawRootVisitor = rootFix;
        collectorResources.GetFinalizerProcessor().VisitRawPointers(rawRootVisitor);
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this, rootFix](size_t) {
        RootVisitor rawRootVisitor = rootFix;
        Heap::GetHeap().VisitAllExportRoots(rawRootVisitor);
    }));
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
            VerifyHeapObjects(point, true);
            VLOG(REPORT, "[GCV2][verify][post-evac] point=%s run=%zu", point, minorTotalRuns + 1);
        }
    };
    // fixinput: grant route-domain before holder Forward (same as FixMinorEvacuatedSlot).
    // Do not rewrite holders or soft-skip Forward here — gold regressed when from_fallback
    // left unfixed from-faces. Bad to-tip is refused at FixMinorObjectSlots (reader gate).
    auto currentObject = [this](BaseObject* object) {
        if (IsGhostFromObject(object) && !IsUnmovableFromObject(object)) {
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), object);
            return ForwardObject(object);
        }
        return object;
    };

    // ZGC Phase 7/8 (zGeneration.cpp:573-580, 918-931, 850-853): pause_relocate_start
    // is flip + set_phase(Relocate) + _relocate.start(); object copy is concurrent.
    // Flip is the trap that makes mutator loads take the self-heal / relocate_object
    // path; without it, concurrent copy is the empty window concreffix measured.
    // MRT_GCV2_MINOR_YOUNG_FLIP=0 rolls back to the all-STW evacuate.
    const bool youngFlipOff = MinorYoungFlipOff();
    const bool concRelocate = stw != nullptr && *stw != nullptr && !youngFlipOff;
    // Concurrent copy is opt-in, not default. REPORT-zpublish measured it drifting the
    // survival_dense checksum 1 run in 5 (368685912892819 vs golden 368685940367600)
    // while 4/5 were golden -- a silent wrong answer, which is worse than a missing
    // optimisation. The publish-vs-compute split (this branch's main work) was necessary
    // but is demonstrably not sufficient; ops/design/ROUTE_PUBLISH_VS_COMPUTE.md.
    // MRT_GCV2_CONC_RELOCATE=1 turns it on -- that is the arm where role=mutator
    // any_copies first became non-zero (2269) and noGhost fell 99.997% -> 99.773%.
    // On. It was off because of a silent wrong answer -- survival_dense produced
    // 368685912892819 instead of the golden 368685940367600 in one run of five -- and a wrong
    // checksum is worse than a missing optimisation, so the comment above was right to keep it off.
    //
    // That measurement predates mutator-assisted relocation. Concurrent relocation is exactly the
    // arm where a mutator meets an object the collector has not moved yet; until this cycle it hit
    // WaitRoutedTipReady, which waited for a publication nobody had been asked to make. The comment
    // above even records the symptom: role=mutator any_copies first became non-zero on this arm.
    //
    // Re-measured on the workload the defect was found on, after that fix:
    //   correctness  15 of 15 runs golden with it on, 4 of 4 with it off, no wrong answer
    //   wall         11.74s off, 8.38s on -- 1.40x, five runs each
    // and natural_wave_notime is 8 of 8 with it on.
    //
    // A compile-time constant rather than the env read it replaces: this was the last live
    // MRT_GCV2_ getenv in the tree, and the campaign cut those from 190 to 3 precisely because a
    // pinned-off env read looks identical to a mechanism that never fires.
    constexpr bool concRelocateOn = true;
    const bool doConcRelocate = concRelocate && concRelocateOn;
    auto liveStw = [stw]() -> const ScopedStopTheWorld* {
        return (stw != nullptr && *stw != nullptr) ? stw->get() : nullptr;
    };
    const bool doYoungFlip = !youngFlipOff;
    GCThreadPool* threadPool = GetThreadPool();
    static const bool forceSerial = []() {
        const char* value = std::getenv("MRT_GCV2_REFFIX_FORCE_SERIAL");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    const bool useParallel = threadPool != nullptr && !forceSerial;

    // Keep opt-in (`=1`): `=0` or unset is the immediate rollback path.
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
                NullRouteCaller::ScopedEdge _edge("remset", nullptr, static_cast<uintptr_t>(slot));
                NullRouteCaller::ScopedTag _nrTag("FixMinorEvacuatedSlot");
                auto known = interiorBases.find(slot);
                BaseObject* knownBase = known != interiorBases.end() ? known->second : nullptr;
                (void)FixMinorEvacuatedSlot(HeapSlotAt<>(slot), knownBase, evacStw,
                                            holderIsCurrentRoot(slot));
            }
        }
    };

    auto fixForwardedReferencesSerial = [this, &reachableVec, &remsetVec, &interiorBases, &currentObject,
                                         &liveStw, &holderIsCurrentRoot]() {
        const ScopedStopTheWorld* evacStw = liveStw();
        FixMinorRootSlots(evacStw);
        PreforwardDiscoveredExternObjects();
        PreforwardAllResurrectExportFromObjects();
        for (BaseObject* object : reachableVec) {
            FixMinorObjectSlots(currentObject(object), evacStw);
        }
        for (MAddress slot : remsetVec) {
            if (Heap::IsHeapAddress(slot)) {
                NullRouteCaller::ScopedEdge _edge("remset", nullptr, static_cast<uintptr_t>(slot));
                NullRouteCaller::ScopedTag _nrTag("FixMinorEvacuatedSlot");
                auto known = interiorBases.find(slot);
                BaseObject* knownBase = known != interiorBases.end() ? known->second : nullptr;
                (void)FixMinorEvacuatedSlot(HeapSlotAt<>(slot), knownBase, evacStw,
                                            holderIsCurrentRoot(slot));
            }
        }
    };

    auto fixHeapParallelOnly = [this, &reachableVec, &remsetVec, &fixHeapSlice](GCThreadPool* pool) {
        // Heap+remset only (roots already fixed under STW). Used by concurrent ref_fix window.
        const size_t dispelAtEntry = RegionInfo::GetDispelGhostCount();
        const size_t nObj = reachableVec.size();
        const size_t nSlot = remsetVec.size();
        const int32_t helperNum = pool->GetMaxThreadNum();
        int32_t heapWorkers = helperNum + 1;
        {
            const char* value = std::getenv("MRT_GCV2_REFFIX_WORKERS");
            if (value != nullptr && value[0] != '\0') {
                int32_t requested = static_cast<int32_t>(std::strtol(value, nullptr, 10));
                if (requested >= 1 && requested < heapWorkers) {
                    heapWorkers = requested;
                }
            }
        }
        if (heapWorkers < 1) {
            heapWorkers = 1;
        }
        std::vector<size_t> objectsTaken(static_cast<size_t>(heapWorkers), 0);
        std::atomic<size_t> objCursor{ 0 };
        std::atomic<size_t> slotCursor{ 0 };
        const size_t objChunk = std::max<size_t>(64, (nObj + static_cast<size_t>(heapWorkers) * 4 - 1) /
                                                        (static_cast<size_t>(heapWorkers) * 4 + 1));
        const size_t slotChunk = std::max<size_t>(64, (nSlot + static_cast<size_t>(heapWorkers) * 4 - 1) /
                                                         (static_cast<size_t>(heapWorkers) * 4 + 1));
        for (int32_t w = 0; w < heapWorkers; ++w) {
            size_t* taken = &objectsTaken[static_cast<size_t>(w)];
            pool->AddWork(new (std::nothrow) LambdaWork(
                [fixHeapSlice, &objCursor, &slotCursor, nObj, nSlot, objChunk, slotChunk, taken](size_t) {
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
                }));
        }
        pool->Start();
        pool->WaitFinish();
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
             "[GCV2][reffix][conc_heap] workers_active=%zu workers_scheduled=%d objects_taken=[%s] "
             "nObj=%zu nSlot=%zu cas_ok=%zu cas_fail=%zu concurrent=1",
             active, heapWorkers, takenStr.c_str(), nObj, nSlot,
             g_minorRefCasOk.load(std::memory_order_relaxed),
             g_minorRefCasFail.load(std::memory_order_relaxed));
    };

    auto fixForwardedReferencesParallel = [this, &reachableVec, &remsetVec, &fixHeapSlice,
                                           &liveStw](GCThreadPool* pool) {
        // T-D ③: dispel must stay frozen across the parallel window.
        const size_t dispelAtEntry = RegionInfo::GetDispelGhostCount();
        const size_t nObj = reachableVec.size();
        const size_t nSlot = remsetVec.size();
        const int32_t helperNum = pool->GetMaxThreadNum();
        const int32_t poolCap = helperNum + 1;
        int32_t heapWorkers = poolCap;
        {
            const char* value = std::getenv("MRT_GCV2_REFFIX_WORKERS");
            if (value != nullptr && value[0] != '\0') {
                int32_t requested = static_cast<int32_t>(std::strtol(value, nullptr, 10));
                if (requested >= 1 && requested < heapWorkers) {
                    heapWorkers = requested;
                }
            }
        }
        // At least 1 heap worker; root families = 5 additional tasks.
        if (heapWorkers < 1) {
            heapWorkers = 1;
        }
        std::vector<size_t> objectsTaken(static_cast<size_t>(heapWorkers), 0);
        std::atomic<size_t> objCursor{ 0 };
        std::atomic<size_t> slotCursor{ 0 };
        // Chunk size: aim ~heapWorkers*4 grabs for load balance; min 64 objects.
        const size_t objChunk = std::max<size_t>(64, (nObj + static_cast<size_t>(heapWorkers) * 4 - 1) /
                                                        (static_cast<size_t>(heapWorkers) * 4 + 1));
        const size_t slotChunk = std::max<size_t>(64, (nSlot + static_cast<size_t>(heapWorkers) * 4 - 1) /
                                                         (static_cast<size_t>(heapWorkers) * 4 + 1));

        // uafclose: root Fix must finish before any heap Forward/Route. Serial FixMinorRootSlots
        // already does roots first; the old parallel path queued root tasks + heap workers then
        // Start() once — heap ForwardObject → RouteRegion → CompactRegion could memset free-tail
        // while a root still named that from (leave-alone → reclaim → GetSize UAF).
        // Phase 1: root families only (grant-before-route + parallel root Fix).
        // concreffix: split the two phases on the structured channel. ZGC keeps root fix in
        // pause_relocate_start but has no centralized field-walk at all — slots heal through the
        // load barrier. So phase 1 is work a self-healing barrier would still owe, and phase 2 is
        // the part it would remove. Timing them apart is the only way to size that trade.
        {
            MRT_PHASE_TIMER("young.ref_fix_bulk_roots");
            FixMinorRootSlotsParallel(pool, liveStw());
            pool->Start();
            pool->WaitFinish();
        }
        // Phase 2: export/extern preforward + heap/remset Fix (same pool, after roots).
        // The timer below closes right after WaitFinish; it covers the two preforward tasks as
        // well, because they are queued into the same pool run and cannot be timed apart.
        {
        MRT_PHASE_TIMER("young.ref_fix_bulk_heap");
        pool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardDiscoveredExternObjects(); }));
        pool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllResurrectExportFromObjects(); }));

        for (int32_t w = 0; w < heapWorkers; ++w) {
            size_t* taken = &objectsTaken[static_cast<size_t>(w)];
            pool->AddWork(new (std::nothrow) LambdaWork(
                [fixHeapSlice, &objCursor, &slotCursor, nObj, nSlot, objChunk, slotChunk, taken](size_t) {
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
                }));
        }

        pool->Start();
        pool->WaitFinish();
        }

        const size_t dispelAtExit = RegionInfo::GetDispelGhostCount();
        CHECK_DETAIL(dispelAtExit == dispelAtEntry,
                     "T-D ghost dispel during parallel ref_fix window entry=%zu exit=%zu "
                     "(plain InGhostFromRegion read assumes phase isolation)",
                     dispelAtEntry, dispelAtExit);
        // concreffix: population of the centralized walk — the denominator for "how much work
        // would a self-healing barrier remove". It must not use REPORT (that would perturb the
        // very timings being measured), but it must not be always-on either: unconditional
        // RTLOG_ERROR would put one ERROR line per minor on the product path. Gate it like the
        // other GC instruments (ebceaf91 precedent); nObj/nSlot/heapWorkers are pre-existing
        // loop bounds, so the disabled path costs one cached bool test.
        static const bool walkPopOn = DiagGate::LegacyOrToken("MRT_GCV2_REFFIX_WALK", "reffixwalk");
        if (walkPopOn) {
            LOG(RTLOG_ERROR, "[GCV2][reffix][walk] nObj=%zu nSlot=%zu heapWorkers=%d", nObj, nSlot,
                heapWorkers);
        }

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
             "[GCV2][reffix][parallel] workers_active=%zu workers_scheduled=%d objects_taken=[%s] "
             "nObj=%zu nSlot=%zu cas_ok=%zu cas_fail=%zu parallel=1",
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
        // enter Relocate. Product path. MRT_GCV2_MINOR_YOUNG_FLIP=0 rolls back.
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
        static constexpr bool kFlipAfterFromSpace = true;
        const auto doFlip = [this]() {
            // Arm self-check: "I edited the source" is not evidence that this arm ran.  One line,
            // once per flip, naming which side of PrepareForwardTable<Young> we are on.
            LOG(RTLOG_ERROR, "[FLIPORDER] young flip arm=%s", kFlipAfterFromSpace ? "after-fromspace" : "before");
            flip_young_relocate_start();
            CensusFrameColoursAfterFlip("young",
                (ZPointerRemappedYoungMask ^ REMAP_COLOUR_MASK) & ZPointerRemappedOldMask);
        };
        if (doYoungFlip && !kFlipAfterFromSpace) {
            doFlip();
        }

        {
            MRT_PHASE_TIMER("young.ref_fix_prepare");
            TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);

            // iorfix: PrepareForwardTable FIRST so liveInfo0 snapshots the closed mark
            // domain while every from region is still FORWARDABLE, THEN pass1 Fix/Forward.
            // Prior order let FixMinorRootSlots RouteRegion before the domain snapshot.
            TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
            fwdTable.PrepareForwardTable<Generation::Young>();
            // fliporder: from-space is now published, so flip here -- ZGC's install-then-flip order.
            if (doYoungFlip && kFlipAfterFromSpace) {
                doFlip();
            }
            TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);
            postEvacPoint("pre-fix-forwarded", false);
        }

        // pregrant: the centralized route-domain confirmation walk is deliberately empty.
        // The always-on post-mark fixpoint above is the frontier finder: it scans live
        // holders, queues only young targets missing from the mark face, and closes their
        // transitive fields before PrepareForwardTable snapshots that face into liveInfo0.
        // Root-only late membership is handled by FixMinorRootSlots' two-pass
        // grant-before-forward immediately below. Rewalking reachableVec + remset + every
        // root here only asked IsRouteSurvivedObject again for an already-closed domain.
        // ZGC likewise installs the relocation set from marking liveness and has no
        // centralized pregrant pass (zGeneration.cpp:190-250).
        {
            MRT_PHASE_TIMER("young.ref_fix_pregrant");
        }

        // pass1 root fix after the domain snapshot — serial sandwich stays;
        // only the post-map fixForwardedReferences body is parallelized (⑦ bulk).
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

        if (!doConcRelocate) {
            MRT_PHASE_TIMER("young.ref_fix_bulk");
            if (!useParallel) {
                VLOG(REPORT, "[GCV2][reffix][parallel] fallback=serial %s",
                     threadPool == nullptr ? "pool_unavailable" : "force_serial");
                fixForwardedReferencesSerial();
                VLOG(REPORT,
                     "[GCV2][reffix][parallel] workers_active=1 workers_scheduled=1 objects_taken=[%zu] "
                     "nObj=%zu nSlot=%zu cas_ok=%zu cas_fail=%zu parallel=0",
                     reachableVec.size(), reachableVec.size(), remsetVec.size(),
                     g_minorRefCasOk.load(std::memory_order_relaxed),
                     g_minorRefCasFail.load(std::memory_order_relaxed));
            } else {
                fixForwardedReferencesParallel(threadPool);
            }
        }

        if (!doConcRelocate) {
            MRT_PHASE_TIMER("young.ref_fix_tail");
            size_t rej = g_fixinputReject.load(std::memory_order_relaxed);
            size_t rec = g_fixinputRecover.load(std::memory_order_relaxed);
            size_t unr = g_fixinputUnrecoverable.load(std::memory_order_relaxed);
            if (rej != 0 || rec != 0 || unr != 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][fixinput] reject=%zu recover=%zu unrecoverable=%zu",
                    rej, rec, unr);
            }
            ValidateMinorReferences("before-return", &reachableVec);
            manager.ExpireKeptFromPreviousCycle();
            postEvacPoint("post-fix-pre-forward", true);
            if (HealCoverage::kHealCoverageCensus) {
                HealCoverage::CensusAfterPublication(
                    currentRemapColour, FlipSeq().load(std::memory_order_relaxed),
                    "young-ref-fix");
            }
        }
    }

    if (doConcRelocate) {
        TransitionToGCPhase(GCPhase::GC_PHASE_FORWARD, true);
        {
            MRT_PHASE_TIMER("young.concurrent_relocate");
            stw->reset();
            VLOG(REPORT, "[GCV2][relocate][conc] concurrent_relocate start nObj=%zu flip=1",
                 reachableVec.size());
            {
                MRT_PHASE_TIMER("young.copy");
                ForwardFromSpace();
            }
            *stw = std::make_unique<ScopedStopTheWorld>("young post-relocate", true,
                                                        GCPhase::GC_PHASE_FORWARD);
            manager.FinishIncompleteFromRegions();
        }
        VLOG(REPORT, "[GCV2][relocate][conc] concurrent_relocate done; STW re-entered");
        postEvacPoint("post-forward-pre-reclaim", true);
        if (kVerifyPostEvac) {
            ValidateMinorReferences("post-forward-pre-reclaim", &reachableVec);
        }
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
            if (useParallel) {
                fixHeapParallelOnly(threadPool);
            } else {
                size_t taken = 0;
                fixHeapSlice(0, reachableVec.size(), 0, remsetVec.size(), taken);
                VLOG(REPORT,
                     "[GCV2][relocate][conc_stw] slot_fix objects_taken=%zu nObj=%zu nSlot=%zu "
                     "cas_ok=%zu cas_fail=%zu",
                     taken, reachableVec.size(), remsetVec.size(),
                     g_minorRefCasOk.load(std::memory_order_relaxed),
                     g_minorRefCasFail.load(std::memory_order_relaxed));
            }
        }
        {
            MRT_PHASE_TIMER("young.ref_fix_tail");
            size_t rej = g_fixinputReject.load(std::memory_order_relaxed);
            size_t rec = g_fixinputRecover.load(std::memory_order_relaxed);
            size_t unr = g_fixinputUnrecoverable.load(std::memory_order_relaxed);
            if (rej != 0 || rec != 0 || unr != 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][fixinput] reject=%zu recover=%zu unrecoverable=%zu",
                    rej, rec, unr);
            }
            ValidateMinorReferences("before-return", &reachableVec);
            manager.ExpireKeptFromPreviousCycle();
            if (HealCoverage::kHealCoverageCensus) {
                HealCoverage::CensusAfterPublication(
                    currentRemapColour, FlipSeq().load(std::memory_order_relaxed),
                    "young-ref-fix-conc");
            }
        }
    } else {
        MRT_PHASE_TIMER("young.copy");
        ForwardFromSpace();
        manager.FinishIncompleteFromRegions();
        postEvacPoint("post-forward-pre-reclaim", true);
        if (kVerifyPostEvac) {
            ValidateMinorReferences("post-forward-pre-reclaim", &reachableVec);
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
                if (region->HasMarkStartAllocGap()) {
                    continue;
                }
                MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
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
        ValidateMinorReferences("after-dispel", nullptr);
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
//   ② return from while region still mid-route (from is a live object; mutator continues),
//   ③ never return a null-tip geometric address, and never CAS one into a slot.
// Distinct from 4e75f2cc: that path is RouteObject *miss* (no plan) on a ghost about to
// be reclaimed — returning from there reinstalls a dying address. Here RouteObject *hit*
// with no tip yet: while still ROUTED/ROUTING, from is not yet CollectRegion'd.
// After object/region publish (FORWARDED|COMPACTED) tip must exist if the plan was real;
// missing tip = permanent hole = invariant violation → CHECK (not hang, not geometric to).
//
// Diag: MRT_GCV2_WAITFWD=1 counts enter / tip-ready / give-up (gate before counter work).
namespace {
// permwho: what the permhole CHECK could not say.
//
// ① Two ledgers. The report printed live= from GetLiveByteCount(), but livesame moved the
//    reclaim predicate onto the mark face: IsKnownEmpty() (RegionInfo.h:1620) reads
//    liveInfo->markEpoch vs snapshotEpoch, and GetLiveByteCount() now only feeds densify
//    (RegionInfo.h:1593-1595). A break reported from one ledger cannot say whether the two
//    agree at that instant, so record both faces.
// ② Which invariant. AdmitForRoute (RegionInfo.h:940-945) admits any 8-byte offset whose bit
//    is set in liveInfo0; MarkBits paints one bit per 8 bytes across the whole object
//    (RegionInfo.h:478 → LiveInfo.h:123), so every interior word of a marked object is
//    admissible. CopyObject writes a tip only at an object *start* (WCollector.cpp:6215).
//    A size-walk of the from-region separates the two candidate breaks:
//      isObjStart=1 containerFwd=0 ⇒ a survivor *start* reached FORWARDED without a Copy
//                                    (receipt-gate / ordering break)
//      isObjStart=0 containerFwd=1 ⇒ interior admission: no path ever fills that tip, and
//                                    to == containerTo + delta proves the plan is geometric
struct PermHoleFacts {
    unsigned knownEmpty = 0;
    unsigned liveAuth = 0;
    unsigned long long faceEpoch = 0;
    unsigned long long regionEpoch = 0;
    unsigned markBmNull = 1;
    unsigned resBmNull = 1;
    unsigned ghost0Null = 1;
    unsigned ghostSurv = 0;
    unsigned curSurv = 0;
    size_t fromOffset = 0;
    size_t allocOff = 0;
    size_t ghostSize = 0;
    unsigned long long preLiveFrom = 0;
    // walk results
    unsigned walkDone = 0;
    unsigned isObjStart = 0;
    unsigned containerFound = 0;
    unsigned containerFwd = 0;
    uintptr_t containerAddr = 0;
    size_t containerSize = 0;
    size_t delta = 0;
    unsigned long long preLiveContainer = 0;
    uintptr_t containerToGuess = 0;
    unsigned containerToValid = 0;
    size_t walkSteps = 0;
    // from-region carrier state
    unsigned fromGhost = 0;
    unsigned fromFree = 0;
    unsigned fromGarbage = 0;
    unsigned liveInfoSame = 0;
    // to-region state: separates "no path ever wrote this tip" from "a tip was written and
    // the to-region has since been reclaimed/reused" — the CHECK message cannot tell them
    // apart, and they have opposite fixes.
    unsigned toFound = 0;
    unsigned toRtype = 0;
    unsigned toRoute = 0;
    unsigned toFree = 0;
    unsigned toGarbage = 0;
    unsigned toGhost = 0;
    unsigned toYoung = 0;
    uintptr_t toRegStart = 0;
    uintptr_t toRegAllocPtr = 0;
    // permhit: the recorded plan itself, and the first words of the memory it points at.
    // RouteInfo (LiveInfo.h:244-260) has no epoch, so plan and reality can only be told
    // apart by comparing them; ClearUnits (RegionInfo.h:842-851) zeroes reused memory, so
    // toWord0==0 with the address inside a live alloc prefix is the reuse signature
    // remsetlife measured on the remset face.
    uintptr_t planTo1 = 0;
    unsigned planTo1Used = 0;
    unsigned planTo2Idx = 0;
    unsigned toInAllocPrefix = 0;
    unsigned toWordsRead = 0;
    unsigned long long toWord0 = 0;
    unsigned long long toWord1 = 0;
    size_t toOffInReg = 0;
};

// Metadata only — safe even after CollectRegion turned the payload into free memory.
void CollectPermHoleMeta(RegionInfo* r, BaseObject* from, PermHoleFacts& f)
{
    if (r == nullptr) {
        return;
    }
    f.knownEmpty = static_cast<unsigned>(r->IsRouteKnownEmpty());
    f.liveAuth = static_cast<unsigned>(r->IsLiveCountAuthoritative());
    f.regionEpoch = static_cast<unsigned long long>(r->GetSnapshotEpoch());
    f.ghostSize = r->GetGhostRegionSize();
    MAddress start = r->GetRegionStart();
    MAddress allocPtr = r->GetRegionAllocPtr();
    f.allocOff = allocPtr > start ? static_cast<size_t>(allocPtr - start) : 0;
    MAddress fromAddr = reinterpret_cast<MAddress>(from);
    if (from != nullptr && fromAddr >= start) {
        f.fromOffset = static_cast<size_t>(fromAddr - start);
    }
    LiveInfo* ghost = r->GetLiveInfo0ForProbe();
    f.ghost0Null = static_cast<unsigned>(ghost == nullptr);
    if (ghost != nullptr) {
        if (r->GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = r->GetRouteMarkView<Generation::Young>();
            f.faceEpoch = static_cast<unsigned long long>(r->GetMarkEpoch(view, ghost));
        } else {
            MarkView<Generation::Old> view = r->GetRouteMarkView<Generation::Old>();
            f.faceEpoch = static_cast<unsigned long long>(r->GetMarkEpoch(view, ghost));
        }
        f.markBmNull = static_cast<unsigned>(r->GetRouteMarkBitmap(ghost) == nullptr);
        f.resBmNull = static_cast<unsigned>(ghost->resurrectBitmap == nullptr);
        f.ghostSurv = static_cast<unsigned>(r->IsRouteSurvivedObject(f.fromOffset));
        // GetPreMaskInfo divides by the ghost region size; a zero size would fault inside
        // the diagnostic rather than reporting anything.
        if (f.ghostSize > 0) {
            f.preLiveFrom =
                static_cast<unsigned long long>(r->GetPreLiveBytesInGhostRegionForProbe(fromAddr));
        }
    }
    LiveInfo* current = r->GetLiveInfo();
    if (r->GetRouteMarkGeneration() == Generation::Young) {
        MarkView<Generation::Young> view = r->GetRouteMarkView<Generation::Young>();
        f.curSurv = static_cast<unsigned>(r->IsSurvivedObject(view, current, f.fromOffset));
    } else {
        MarkView<Generation::Old> view = r->GetRouteMarkView<Generation::Old>();
        f.curSurv = static_cast<unsigned>(r->IsSurvivedObject(view, current, f.fromOffset));
    }
    f.fromGhost = static_cast<unsigned>(r->IsGhostFromRegion());
    f.fromFree = static_cast<unsigned>(r->IsFreeRegion());
    f.fromGarbage = static_cast<unsigned>(r->IsGarbageRegion());
    f.liveInfoSame = static_cast<unsigned>(r->GetLiveInfo() == ghost);
}

// The geometric to lands in some region; its carrier state says whether a tip could still
// be there at all.
void CollectPermHoleToRegion(BaseObject* geometricTo, PermHoleFacts& f)
{
    if (geometricTo == nullptr || !Heap::IsHeapAddress(geometricTo)) {
        return;
    }
    RegionInfo* tr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(geometricTo));
    if (tr == nullptr) {
        return;
    }
    f.toFound = 1;
    f.toRtype = static_cast<unsigned>(tr->GetRegionType());
    f.toRoute = static_cast<unsigned>(tr->GetRouteState());
    f.toFree = static_cast<unsigned>(tr->IsFreeRegion());
    f.toGarbage = static_cast<unsigned>(tr->IsGarbageRegion());
    f.toGhost = static_cast<unsigned>(tr->IsGhostFromRegion());
    f.toYoung = static_cast<unsigned>(tr->IsYoungRegion());
    f.toRegStart = static_cast<uintptr_t>(tr->GetRegionStart());
    f.toRegAllocPtr = static_cast<uintptr_t>(tr->GetRegionAllocPtr());
    uintptr_t toAddr = reinterpret_cast<uintptr_t>(geometricTo);
    f.toOffInReg = toAddr >= f.toRegStart ? static_cast<size_t>(toAddr - f.toRegStart) : 0;
    f.toInAllocPrefix = static_cast<unsigned>(toAddr >= f.toRegStart && toAddr < f.toRegAllocPtr);
    // Mapped heap memory: readable whatever its contents. A zero first word is the state
    // ClearUnits leaves behind, and is also what an unwritten tip looks like.
    if (f.toInAllocPrefix != 0 && toAddr + 2 * sizeof(uint64_t) <= f.toRegAllocPtr) {
        const uint64_t* w = reinterpret_cast<const uint64_t*>(toAddr);
        f.toWord0 = static_cast<unsigned long long>(w[0]);
        f.toWord1 = static_cast<unsigned long long>(w[1]);
        f.toWordsRead = 1;
    }
}

// The plan the from-region is still serving, read straight out of its RouteInfo.
void CollectPermHolePlan(RegionInfo* r, PermHoleFacts& f)
{
    if (r == nullptr) {
        return;
    }
    RouteInfo plan = r->GetRouteInfoForProbe();
    f.planTo1 = plan.toRegion1StartAddress;
    f.planTo1Used = static_cast<unsigned>(plan.GetToRegion1UsedBytes());
    f.planTo2Idx = static_cast<unsigned>(plan.GetToRegion2Idx());
}

// Payload walk — reads from-region memory, which CollectRegion may already have released.
// Called only after the metadata line is already on the record.
void CollectPermHoleWalk(RegionInfo* r, BaseObject* from, BaseObject* geometricTo, PermHoleFacts& f)
{
    if (r == nullptr || from == nullptr || !r->IsSmallRegion()) {
        return;
    }
    MAddress start = r->GetRegionStart();
    MAddress allocPtr = r->GetRegionAllocPtr();
    MAddress fromAddr = reinterpret_cast<MAddress>(from);
    if (fromAddr < start || allocPtr <= start) {
        return;
    }
    constexpr size_t kMaxWalkSteps = 1u << 20;
    MAddress position = start;
    while (position < allocPtr && f.walkSteps < kMaxWalkSteps) {
        BaseObject* o = from_region_addr(position);
        if (!Collector::PlausibleManagedObjectGate("permwho-walk", o)) {
            break;
        }
        size_t allocSize = RegionSpace::GetAllocSize(*o);
        if (allocSize == 0) {
            break;
        }
        ++f.walkSteps;
        if (position == fromAddr) {
            f.isObjStart = 1;
        }
        if (fromAddr >= position && fromAddr < position + allocSize) {
            f.containerFound = 1;
            f.containerAddr = static_cast<uintptr_t>(position);
            f.containerSize = allocSize;
            f.delta = static_cast<size_t>(fromAddr - position);
            f.containerFwd = static_cast<unsigned>(o->IsForwarded());
            LiveInfo* ghost = r->GetLiveInfo0ForProbe();
            if (ghost != nullptr && r->GetGhostRegionSize() > 0) {
                f.preLiveContainer =
                    static_cast<unsigned long long>(r->GetPreLiveBytesInGhostRegionForProbe(position));
            }
            if (geometricTo != nullptr && reinterpret_cast<uintptr_t>(geometricTo) > f.delta) {
                uintptr_t guess = reinterpret_cast<uintptr_t>(geometricTo) - f.delta;
                f.containerToGuess = guess;
                BaseObject* cto = from_region_addr(guess);
                if (Heap::IsHeapAddress(cto)) {
                    f.containerToValid = static_cast<unsigned>(cto->IsValidObject());
                }
            }
            break;
        }
        position += allocSize;
    }
    f.walkDone = 1;
}
} // namespace

BaseObject* WCollector::WaitRoutedTipReady(BaseObject* from, BaseObject* to, RegionInfo* forwarding) const
{
    static std::atomic<uint64_t> enterCount{0};
    static std::atomic<uint64_t> tipReadyCount{0};
    static std::atomic<uint64_t> giveUpCount{0};
    constexpr int diagOn = 0;
    if (diagOn) {
        // permwho: the three counters were incremented but never read anywhere, so
        // MRT_GCV2_WAITFWD=1 produced no output at all and "enter != 0" was unanswerable.
        // Capture-less lambda may odr-use these function-local statics without capturing.
        static std::atomic<bool> waitfwdAtexitInstalled{ false };
        bool expected = false;
        if (waitfwdAtexitInstalled.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            std::atexit([]() {
                std::fprintf(stderr, "[GCV2][waitfwd] atexit enter=%llu tipReady=%llu giveUp=%llu\n",
                             static_cast<unsigned long long>(enterCount.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(tipReadyCount.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(giveUpCount.load(std::memory_order_relaxed)));
                std::fflush(stderr);
            });
        }
        enterCount.fetch_add(1, std::memory_order_relaxed);
    }

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    // Bound mid-copy waits only while route is still in flight. Permanent publish-without-tip
    // is an invariant break (CHECK below), not a longer spin.
    auto permanentHole = [&](const char* reason, int spins, BaseObject* geometricTo) -> BaseObject* {
        if (diagOn) {
            giveUpCount.fetch_add(1, std::memory_order_relaxed);
        }
        // portmutreloc: the 4096-spin FATAL leg. Counted, never removed -- the port adds a
        // first choice ahead of this wait, it does not take the fallback away. If mutator
        // relocation is worth anything, this is the number that drops.
        if (MutatorRelocate::StatsOn()) {
            MutatorRelocate::NoteWaitFatal();
        }
        RegionInfo::RouteState rs =
            forwarding != nullptr ? forwarding->GetRouteState() : RegionInfo::RouteState::NORMAL;
        GCPhase phase = GetGCPhase();
        MAddress regStart = forwarding != nullptr ? forwarding->GetRegionStart() : 0;
        MAddress regEnd = forwarding != nullptr ? forwarding->GetRegionEnd() : 0;
        unsigned rtype = forwarding != nullptr ? static_cast<unsigned>(forwarding->GetRegionType()) : 0;
        unsigned young = forwarding != nullptr ? static_cast<unsigned>(forwarding->IsYoungRegion()) : 0;
        size_t live = forwarding != nullptr ? forwarding->GetLiveByteCount() : 0;
        bool fromFwd = from != nullptr && from->IsForwarded();
        // permwho: both ledgers, then the size-walk that names which invariant broke.
        // Metadata line first: the walk touches from-region payload that CollectRegion may
        // already have released, so the cheap facts must be on the record before it runs.
        PermHoleFacts f;
        CollectPermHoleMeta(forwarding, from, f);
        CollectPermHoleToRegion(geometricTo, f);
        CollectPermHolePlan(forwarding, f);
        LOG(RTLOG_ERROR,
            "[GCV2][permhit] plan region=%p planTo1=%#zx planTo1Used=%u planTo2Idx=%u "
            "to=%p toOffInReg=%zu toInAllocPrefix=%u toWordsRead=%u toWord0=%#llx toWord1=%#llx "
            "preLiveFrom=%llu fromOff=%zu",
            forwarding, static_cast<size_t>(f.planTo1), f.planTo1Used, f.planTo2Idx, geometricTo,
            f.toOffInReg, f.toInAllocPrefix, f.toWordsRead, f.toWord0, f.toWord1, f.preLiveFrom,
            f.fromOffset);
        LOG(RTLOG_ERROR,
            "[GCV2][permwho] toregion to=%p toFound=%u toRtype=%u toRoute=%u toFree=%u toGarbage=%u "
            "toGhost=%u toYoung=%u toRegStart=%#zx toRegAlloc=%#zx fromGhost=%u fromFree=%u "
            "fromGarbage=%u liveInfoSame=%u",
            geometricTo, f.toFound, f.toRtype, f.toRoute, f.toFree, f.toGarbage, f.toGhost, f.toYoung,
            static_cast<size_t>(f.toRegStart), static_cast<size_t>(f.toRegAllocPtr), f.fromGhost,
            f.fromFree, f.fromGarbage, f.liveInfoSame);
        LOG(RTLOG_ERROR,
            "[GCV2][permwho] books region=%p live=%zu liveAuth=%u knownEmpty=%u faceEpoch=%llu "
            "regionEpoch=%llu ghost0Null=%u markBmNull=%u resBmNull=%u ghostSurv=%u curSurv=%u "
            "fromOff=%zu allocOff=%zu ghostSize=%zu preLiveFrom=%llu "
            "enter=%llu tipReady=%llu giveUp=%llu",
            forwarding, live, f.liveAuth, f.knownEmpty, f.faceEpoch, f.regionEpoch, f.ghost0Null,
            f.markBmNull, f.resBmNull, f.ghostSurv, f.curSurv, f.fromOffset, f.allocOff, f.ghostSize,
            f.preLiveFrom, static_cast<unsigned long long>(enterCount.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(tipReadyCount.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(giveUpCount.load(std::memory_order_relaxed)));
        CollectPermHoleWalk(forwarding, from, geometricTo, f);
        LOG(RTLOG_ERROR,
            "[GCV2][permwho] walk region=%p walkDone=%u steps=%zu isObjStart=%u containerFound=%u "
            "container=%#zx containerSize=%zu containerFwd=%u delta=%zu preLiveContainer=%llu "
            "containerToGuess=%#zx containerToValid=%u",
            forwarding, f.walkDone, f.walkSteps, f.isObjStart, f.containerFound,
            static_cast<size_t>(f.containerAddr), f.containerSize, f.containerFwd, f.delta,
            f.preLiveContainer, static_cast<size_t>(f.containerToGuess), f.containerToValid);
        CHECK_DETAIL(false,
                     "[GCV2][permhole] WaitRoutedTipReady %s spins=%d phase=%d routeState=%u "
                     "region=%p range=[%#zx,%#zx) rtype=%u young=%u live=%zu knownEmpty=%u "
                     "ghostSurv=%u isObjStart=%u containerFwd=%u delta=%zu containerToValid=%u "
                     "toRtype=%u toFree=%u toGarbage=%u "
                     "from=%p fromFwd=%u to=%p tipValid=0 — publish without receipt",
                     reason, spins, static_cast<int>(phase), static_cast<unsigned>(rs), forwarding,
                     static_cast<size_t>(regStart), static_cast<size_t>(regEnd), rtype, young, live,
                     f.knownEmpty, f.ghostSurv, f.isObjStart, f.containerFwd, f.delta, f.containerToValid,
                     f.toRtype, f.toFree, f.toGarbage,
                     from, static_cast<unsigned>(fromFwd), geometricTo);
        // Unreachable after FATAL; keep from (never geometric null-tip) if CHECK is non-abort builds.
        return from;
    };

    auto lookupTo = [&]() -> BaseObject* {
        if constexpr (ForwardingTable::kEntriesSoleWhenArmed) {
            if (ForwardingTable::EntriesArmed(reinterpret_cast<MAddress>(from))) {
                const MAddress stored = ForwardingTable::FindTo(reinterpret_cast<MAddress>(from));
                if (stored != 0) {
                    return reinterpret_cast<BaseObject*>(stored);
                }
                if (from->IsForwarded()) {
                    BaseObject* geometric = space.GetRegionManager().FindPublishedRoute(from, forwarding).dest;
                    if (geometric != nullptr &&
                        ZForwarding::DestUsable(reinterpret_cast<MAddress>(geometric))) {
                        const MAddress receipt = ForwardingTable::InsertMapping(
                            reinterpret_cast<MAddress>(from), reinterpret_cast<MAddress>(geometric));
                        (void)space.GetRegionManager().GetRelocationRequestQueue().Publish(
                            reinterpret_cast<MAddress>(from), receipt);
                        return reinterpret_cast<BaseObject*>(receipt);
                    }
                }
                return nullptr;
            }
        }
        return space.GetRegionManager().FindPublishedRoute(from, forwarding).dest;
    };
    BaseObject* again = lookupTo();
    if (again != nullptr && Heap::IsHeapAddress(again) && again->IsValidObject()) {
        if (diagOn) {
            tipReadyCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (MutatorRelocate::StatsOn()) {
            MutatorRelocate::NoteWaitReceipt();
        }
        return again;
    }
    const bool tableHit = again != nullptr;
    const RegionInfo::RouteState rs = forwarding->GetRouteState();
    const bool regionPublished =
        rs == RegionInfo::RouteState::FORWARDED || rs == RegionInfo::RouteState::COMPACTED ||
        forwarding->IsForwardingDone();
    // LEAD 12:2x: retain refused = worker holds the page (retain_page n<0 / n==0).
    // oraclecut §4: unpublished (any reason) waits for the region-level publish;
    // keep-from only after publish + table miss (VisitLive hole).
    bool retainRefused = false;
    if (!regionPublished && !forwarding->IsForwardingDone()) {
        if (forwarding->TryLockReadFromRegion()) {
            forwarding->UnlockReadFromRegion();
        } else {
            retainRefused = true;
        }
    }
    const MutatorRelocate::UnpublishedAnswer ans =
        MutatorRelocate::AnswerUnpublished(tableHit, regionPublished, retainRefused);
    if (ans == MutatorRelocate::UnpublishedAnswer::UseTo && again != nullptr) {
        return again;
    }
    // Wait for the region-level publish (FORWARDED / COMPACTED / kept), not
    // an object-level empty spin (47595a33). ExemptFromRegion publishes kept
    // immediately so this wait is bounded every cycle.
    //
    // oracle r5: store-side waits after FORWARD (reclaim/idle) on ROUTED
    // unpublished pages are structurally never-true (527/527 timeout).
    // Only wait while a publisher still exists this cycle.
    const GCPhase waitPhase = GetGCPhase();
    // POST_TRACE already RouteRegion's (PrepareForwardTable); copy is still
    // ahead. Skipping the wait there keep-froms a page ForwardFromSpace is
    // about to empty (r5 n=64 phase=12 route=3). IDLE/FINISH/RECLAIM have
    // no publisher — those are the structurally-false waits (oracle r5).
    const bool waitEligible = (waitPhase == GCPhase::GC_PHASE_POST_TRACE ||
                               waitPhase == GCPhase::GC_PHASE_PREFORWARD ||
                               waitPhase == GCPhase::GC_PHASE_FORWARD) &&
        forwarding != nullptr && !forwarding->IsFreeRegion() && !forwarding->IsGarbageRegion();
    if (ans == MutatorRelocate::UnpublishedAnswer::Wait && !waitEligible) {
        static std::atomic<size_t> g_waitIneligible{ 0 };
        const size_t n = g_waitIneligible.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n - 1)) == 0) {
            LOG(RTLOG_ERROR,
                "[GCV2][wait-ineligible] n=%zu from=%p region=%p phase=%d route=%u done=%u "
                "free=%u garbage=%u — FindTo/FindRetiredTo/keep-from",
                n, from, forwarding, static_cast<int>(waitPhase), static_cast<unsigned>(rs),
                static_cast<unsigned>(forwarding != nullptr && forwarding->IsForwardingDone()),
                static_cast<unsigned>(forwarding != nullptr && forwarding->IsFreeRegion()),
                static_cast<unsigned>(forwarding != nullptr && forwarding->IsGarbageRegion()));
        }
        BaseObject* retired = lookupTo();
        if (retired != nullptr && Heap::IsHeapAddress(retired) && retired->IsValidObject()) {
            if (MutatorRelocate::StatsOn()) {
                MutatorRelocate::NoteWaitReceipt();
            }
            return retired;
        }
        if (MutatorRelocate::StatsOn()) {
            MutatorRelocate::NoteWaitGiveUp();
        }
        return from;
    }
    if (ans == MutatorRelocate::UnpublishedAnswer::Wait) {
        static std::atomic<size_t> g_regionWait{ 0 };
        static std::atomic<size_t> g_regionGot{ 0 };
        const size_t wn = g_regionWait.fetch_add(1, std::memory_order_relaxed) + 1;
        if (MutatorRelocate::StatsOn()) {
            MutatorRelocate::NoteRegionWaitEnter();
        }
        if (wn <= 8 || (wn & (wn - 1)) == 0) {
            LOG(RTLOG_ERROR,
                "[GCV2][region-wait] n=%zu from=%p region=%p route=%u done=%u — queue receipt request",
                wn, from, forwarding, static_cast<unsigned>(rs),
                static_cast<unsigned>(forwarding->IsForwardingDone()));
        }
        auto regionIsPublished = [forwarding]() -> bool {
            const RegionInfo::RouteState now = forwarding->GetRouteState();
            return now == RegionInfo::RouteState::FORWARDED ||
                now == RegionInfo::RouteState::COMPACTED || forwarding->IsForwardingDone();
        };
        // zRelocate.cpp:382-406 enters add_and_wait only after retain_page
        // succeeded. RetainForwarding may itself observe a concurrent page
        // completion and refuse; keep-from is then the legal late answer.
        if (!forwarding->TryLockReadFromRegion()) {
            if (MutatorRelocate::StatsOn()) {
                MutatorRelocate::NoteWaitGiveUp();
            }
            return from;
        }
        forwarding->UnlockReadFromRegion();

        // A mutator-discovered object must be in the worker's relocation domain
        // before the request is visible. The request is an attention signal;
        // page publication, not this object's receipt, is the wait predicate.
        EnsureRouteDomainMembership(const_cast<WCollector*>(this), from);
        RelocationRequestQueue& requests = space.GetRegionManager().GetRelocationRequestQueue();
        RelocationRequestQueue::EnqueueResult queued =
            requests.Add(forwarding, reinterpret_cast<MAddress>(from));

        // Close publish-before-enqueue: installation may have won between the
        // first lookup and Add(). Publishing an already-existing receipt is the
        // only alternate completion path.
        BaseObject* raced = lookupTo();
        if (raced != nullptr && Heap::IsHeapAddress(raced) && raced->IsValidObject()) {
            (void)requests.Publish(reinterpret_cast<MAddress>(from), reinterpret_cast<MAddress>(raced));
        }

        requests.WaitUntil(queued.request, regionIsPublished);

        // zRelocate.cpp:408-415 asks the object question only after the page is
        // done. A miss is the VisitLive hole recorded in MutatorRelocate.h:68-80
        // and legally keeps the still-live from address.
        BaseObject* ready = lookupTo();
        if (ready != nullptr && Heap::IsHeapAddress(ready) && ready->IsValidObject()) {
            g_regionGot.fetch_add(1, std::memory_order_relaxed);
            if (MutatorRelocate::StatsOn()) {
                MutatorRelocate::NoteRegionWaitGot();
                MutatorRelocate::NoteWaitReceipt();
            }
            return ready;
        }
        if (MutatorRelocate::StatsOn()) {
            MutatorRelocate::NoteRegionWaitPublishedMiss();
            MutatorRelocate::NoteWaitGiveUp();
        }
        return from;
    }
    if constexpr (MutatorRelocate::kUnpublishedMeansKeepFrom) {
        if (MutatorRelocate::StatsOn()) {
            MutatorRelocate::NoteWaitGiveUp();
        }
        return from;
    }
    (void)permanentHole;
    return from;
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
// nullptr means "fall through to the legs that were already there". Never a hard failure:
// every refusal here is a state the old code handled anyway.
BaseObject* WCollector::TryMutatorRelocate(BaseObject* obj, RegionInfo* forwarding) const
{
    if (!MutatorRelocate::Enabled()) {
        return nullptr;
    }
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
    if (!forwarding->TryLockReadFromRegion()) {
        MutatorRelocate::NoteFallback(MutatorRelocate::Fallback::RETAIN_FAILED);
        return nullptr;
    }
    // zRelocate.cpp:393-395: retain_page then assert is_phase_relocate.
    // SetGCPhase publishes before handshake, so a mutator that retained across
    // FORWARD→IDLE must not enter ForwardObjectImpl's CHECK. Release and let
    // the existing FindToVersion / wait legs consume the published table.
    phase = GetGCPhase();
    if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
        forwarding->UnlockReadFromRegion();
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
    BaseObject* toVersion = const_cast<WCollector*>(this)->ForwardObjectImpl(obj, forwarding);
    MutatorRelocate::LeaveScope();
    forwarding->UnlockReadFromRegion(); // release_page
    if (wasForwarded) {
        MutatorRelocate::NoteAlreadyForwarded();
    }
    if (toVersion == nullptr) {
        MutatorRelocate::NoteFallback(MutatorRelocate::Fallback::COPY_FAILED);
        return nullptr;
    }
    if (toVersion == obj) {
        // ForwardObjectImpl resolved to the from address: not a relocation. Let the old legs
        // decide what to hand back rather than short-circuiting them with an unmoved pointer.
        MutatorRelocate::NoteFallback(MutatorRelocate::Fallback::COPY_FAILED);
        return nullptr;
    }
    return toVersion;
}

BaseObject* WCollector::ResolveStoreValue(BaseObject* ref) const
{
    // zBarrier.inline.hpp:695-716 store_barrier_on_heap_oop_field:
    // color_store_good includes remap. A movable ghost-from value must go
    // through the same relocate_or_remap funnel as the load barrier
    // (zRelocate.cpp:382-416) before it is painted store-good.
    // Flip false for the perturbation SO (W1 returns, crash returns).
    static constexpr bool kResolveStoreValue = true;
    if constexpr (!kResolveStoreValue) {
        return ref;
    }
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        return ref;
    }
    const MAddress fromAddr = reinterpret_cast<MAddress>(ref);
    RegionInfo* ghost = RegionInfo::GetGhostFromRegionAt(fromAddr);
    if (ghost == nullptr || ghost->IsUnmovableFromRegion()) {
        // oracle Q3 ④: resolve-time FREE is a lost reference. FindRetiredTo
        // first (FindToVersion already walks the retired generation); miss
        // keeps the value for ColourStaleLoadBad — never null, never store-good.
        RegionInfo* live = RegionInfo::TryGetRegionInfoAt(fromAddr);
        if (ghost == nullptr && (live == nullptr || live->IsFreeRegion())) {
            BaseObject* retired = FindToVersion(ref);
            if (retired != nullptr) {
                return retired;
            }
            static std::atomic<size_t> g_lostRef{ 0 };
            const size_t n = g_lostRef.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8 || (n & (n - 1)) == 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][lostref] n=%zu ref=%p region=%p free=%u — retired miss, keep load-bad",
                    n, ref, live, static_cast<unsigned>(live != nullptr && live->IsFreeRegion()));
            }
        }
        return ref;
    }
    BaseObject* resolved = relocate_or_remap_object(ref, ghost->generation_id());
    if (resolved != nullptr) {
        return resolved;
    }
    return ref;
}

BaseObject* WCollector::ForwardObject(BaseObject* obj)
{
    // markfloor: stack/reg roots may hold RawArray+8 interiors (tip=length). Do not
    // GetSize/CopyObject them; leave the slot unchanged (caller keeps obj).
    if (!Collector::PlausibleManagedObjectGate("WCollector::ForwardObject", obj)) {
        // tipnull: uncopied movable ghost is not VisitLive success.
        if (IsGhostFromObject(obj) && !IsUnmovableFromObject(obj)) {
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
        return nullptr;
    }
    RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
        return nullptr;
    }

    if (fwdTable.RouteRegion(region)) {
        // portmutreloc positive control (MRT_GCV2_MUTRELOC_INJECT=1). The loop below already
        // is retain / copy / release -- it is where the ported leg's three pieces came from.
        // Routing it through TryMutatorRelocate once makes those pieces execute on a path this
        // workload actually reaches, so retain, the scoped copy and the attribution in
        // ForwardObjectExclusive are all exercised and self_copies must come out non-zero.
        // On refusal it falls straight into the unchanged loop, so behaviour is unchanged
        // apart from which frame ran the copy.
        if (MutatorRelocate::InjectOn()) {
            BaseObject* injected = TryMutatorRelocate(obj, region);
            if (injected != nullptr) {
                return injected;
            }
        }
        // secondclass ①: GetRoute is geometric plan; retain before copying or
        // consuming from-side state (else null-tip → HasRefField SEGV si_addr=0x8).
        if (region->TryLockReadFromRegion()) {
            // zRelocate.cpp:393-395: retain_page then assert is_phase_relocate.
            // SetGCPhase can publish IDLE while this thread holds the retain.
            // Release and consume only the forwarding-table answer in that case.
            const GCPhase retainedPhase = GetGCPhase();
            if (retainedPhase != GCPhase::GC_PHASE_PREFORWARD &&
                retainedPhase != GCPhase::GC_PHASE_FORWARD) {
                region->UnlockReadFromRegion();
                return FindToVersion(obj);
            }
            BaseObject* toVersion = ForwardObjectImpl(obj, region);
            region->UnlockReadFromRegion();
            return toVersion;
        }
        // ZGC's relocate_object (zRelocate.cpp:362-393) calls forward_object
        // after retain_page refuses; it never retries the retain with sched_yield.
        // Our n<0 refusal is immediate (the caller may already hold an outer pin),
        // so a table miss is allowed here. Returning null makes ForwardRegion's
        // receipt audit keep the page instead of spinning outside a safepoint.
        return FindToVersion(obj);
    } else if (region->IsCompacted()) {
        // Compact copies under region write-lock before COMPACTED is published.
        return FindToVersion(obj);
    }
    return nullptr;
}

BaseObject* WCollector::ForwardObjectImpl(BaseObject* obj, RegionInfo* ghostFromRegion)
{
    CHECK(GetGCPhase() == GCPhase::GC_PHASE_PREFORWARD || GetGCPhase() == GCPhase::GC_PHASE_FORWARD);
    // Plan the dest *before* TryLockObject. Holding LOCKED across RouteRegion /
    // TakeRegion is the object-lock face of REPORT-routespin: a waiter in
    // IsLockedWord yield can never help, and the copier can park in a safepoint
    // or ROUTING wait that only GC can finish. ZGC relocate_object_inner
    // (zRelocate.cpp:354-372) does alloc+copy+insert with no safepoint; 乙1 is
    // the same rule for the object lock that routefix already applied to ROUTING.
    ForwardingTable::EnsureEntries(ghostFromRegion);
    BaseObject* planned = fwdTable.PlanRoute(obj, CopierRouteMint::Make()).dest;
    do {
        StateWord oldWord = obj->GetStateWord();

        // 1. object has already been forwarded. Table hit is the publish
        // (zRelocate.cpp:371, MutatorRelocate.h:124). A FORWARDED header with no
        // entry is last cycle's residual after the table was retired
        // (zRelocationSet.cpp:91-96); PlanRoute's dest is uncopied — do not
        // return it. Fall through and recopy this cycle.
        if (obj->IsForwarded()) {
            auto toObj = GetForwardPointer(obj, ghostFromRegion);
            if (toObj != nullptr) {
                DLOG(FORWARD, "skip forwarded obj %p -> %p<%p>(%zu)", obj, toObj, toObj->GetTypeInfo(),
                     toObj->GetSize());
                return toObj;
            }
        }

        // 2. object is being forwarded. zRelocate.cpp:386-389 find() hit → already
        // relocated; insert (the publish) happens before UnlockObject(FORWARDED), so
        // waiters must not require the lock to drop. A yield-only loop here is what
        // hung gc-main at WCollector.cpp:9570 while the mutator sat in SuspendForSync
        // (REPORT-llstore hang_live).
        if (oldWord.IsLockedWord()) {
            auto toObj = GetForwardPointer(obj, ghostFromRegion);
            const bool tableHit = toObj != nullptr;
            const bool pagePublished = ghostFromRegion != nullptr &&
                (ghostFromRegion->IsForwardingDone() ||
                 ghostFromRegion->GetRouteState() == RegionInfo::RouteState::FORWARDED ||
                 ghostFromRegion->GetRouteState() == RegionInfo::RouteState::COMPACTED);
            const MutatorRelocate::LockedWaiterAnswer ans =
                MutatorRelocate::AnswerLockedWaiter(tableHit, pagePublished);
            if (ans == MutatorRelocate::LockedWaiterAnswer::UseTo) {
                return toObj;
            }
            if (ans == MutatorRelocate::LockedWaiterAnswer::UsePlanned) {
                // Page done + leftover LOCKED is not a live copier
                // (zForwarding.cpp:138-151). PlanRoute dest is uncopied after
                // the table was retired (zRelocationSet.cpp:91-96). Keep from
                // (same as AnswerUnpublished KeepFrom) — do not return planned,
                // do not yield-wait (REPORT-llstore hang_live).
                return obj;
            }
            sched_yield();
            continue;
        }

        // 3. hope we can forward this object
        if (obj->TryLockObject(oldWord)) {
            // zForwarding.cpp:86-108: retain at the moment this thread copies,
            // not later in Exclusive — Exempt WaitCopied must observe the token
            // before MarkForwardingDone (REPORT-lockdrain A 1→6 on Exclusive-only +1).
            RegionInfo* page = ghostFromRegion;
            if (page == nullptr) {
                page = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
                if (page == nullptr) {
                    page = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
                }
            }
            if (page != nullptr) {
                page->NoteCopyInflight();
            }
            return ForwardObjectExclusive(obj, planned, page);
        }
    } while (true);
    LOG(RTLOG_FATAL, "forwardObject exit in wrong path");
    return nullptr;
}

BaseObject* WCollector::ForwardObjectExclusive(BaseObject* obj)
{
    // Vtable entry: caller already TryLock'd. Count on the from-page here so
    // the token is live before copy (same as ForwardObjectImpl's TryLock arm).
    RegionInfo* page = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (page == nullptr) {
        page = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    }
    if (page != nullptr) {
        page->NoteCopyInflight();
    }
    return ForwardObjectExclusive(obj, fwdTable.PlanRoute(obj, CopierRouteMint::Make()).dest, page);
}

BaseObject* WCollector::ForwardObjectExclusive(BaseObject* obj, BaseObject* toObj, RegionInfo* copyPage)
{
    // EndCopy on the same page NoteCopy ran on (zForwarding.cpp:134-169).
    // find() hits never enter (zRelocate.cpp:382-410).
    struct EndCopyInflight {
        RegionInfo* region;
        ~EndCopyInflight()
        {
            if (region != nullptr) {
                region->EndCopyInflight();
            }
        }
    } endCopy{ copyPage };

    if (!Collector::PlausibleManagedObjectGate("WCollector::ForwardObjectExclusive", obj)) {
        // Caller locked for a real object; unlock without claiming FORWARDED.
        obj->UnlockObject(ObjectState::NORMAL);
        return nullptr;
    }
    if (toObj == nullptr) {
        obj->UnlockObject(ObjectState::NORMAL);
        return nullptr;
    }
    size_t size = RegionSpace::GetAllocSize(*obj);
    DLOG(FORWARD, "forward obj %p<%p>(%zu) to %p", obj, obj->GetTypeInfo(), size, toObj);
    CopyObject(*obj, *toObj, size);
    // Publish a fully-initialized to-object. ZGC insert (zRelocate.cpp:368-372) is the
    // publish of a completed copy; SetStateCode must precede InsertMapping so a
    // find() hit never observes the from-copy's LOCKED header bits on to.
    // In-place (GetRoute keep-from: to==from) the header is still LOCKED —
    // painting NORMAL here makes UnlockObject CHECK fail (StateWord.h:183).
    // That is the A_locked abort after exempt-kept (REPORT-lockdrain /
    // REPORT-exemptlife §4).
    if (toObj != obj) {
        toObj->SetStateCode(ObjectState::NORMAL);
    }
    std::atomic_thread_fence(std::memory_order_release);
    if (toObj != obj && !ToHeaderCovered(toObj)) {
        obj->UnlockObject(ObjectState::NORMAL);
        return nullptr;
    }
    const ZForwarding::Receipt receipt =
        ForwardingTable::InstallMapping(reinterpret_cast<MAddress>(obj), reinterpret_cast<MAddress>(toObj));
    const MAddress mapped = receipt.address;
    if (!ForwardingTable::ReceiptAllowsForwarded(mapped)) {
        // FORWARDED is a publication of the receipt, not merely of CopyObject.
        // If the table itself could not be installed, restore a retryable header;
        // never expose a forwarded object whose answer is the retiring from slot.
        obj->UnlockObject(ObjectState::NORMAL);
        return nullptr;
    }
    RegionSpace& regionSpace = reinterpret_cast<RegionSpace&>(theAllocator);
    (void)regionSpace.GetRegionManager().GetRelocationRequestQueue().Publish(
        reinterpret_cast<MAddress>(obj), mapped);
    // portmutreloc: the copy just happened on this thread. InScope() is set only by
    // TryMutatorRelocate, so this counts objects a mutator relocated itself and nothing else --
    // the one number that distinguishes "the ported leg ran" from "the leg exists". GC workers
    // reach the same CopyObject with the flag clear and are not counted.
    if (MutatorRelocate::StatsOn()) {
        // ThreadLocal.h:20 ThreadType {CJ_PROCESSOR, GC_THREAD, FP_THREAD, HOT_UPDATE_THREAD};
        // IsRuntimeThread() is >= GC_THREAD, so !IsRuntimeThread() is exactly CJ_PROCESSOR --
        // a mutator. Both predicates are thread-local reads (MutatorManager.cpp:70-84).
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
    obj->UnlockObject(ObjectState::FORWARDED);
    return reinterpret_cast<BaseObject*>(mapped);
}
} // namespace MapleRuntime
