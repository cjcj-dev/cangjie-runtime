// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/WCollector/WCollector.h"

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
struct Y2yHandoffReceiptState {
    std::atomic<uint64_t> phase0 { 0 };
    std::atomic<uint64_t> phase1 { 0 };
    std::atomic<uint64_t> phase2 { 0 };
    std::atomic<uint64_t> beforeRelease { 0 };
    std::atomic<uint64_t> afterRoot { 0 };
    std::atomic<uint64_t> afterStw2 { 0 };
};
Y2yHandoffReceiptState g_y2yHandoffReceipt;
std::atomic<BaseObject*> g_y2yAfterReleaseHolder { nullptr };
std::atomic<uint64_t> g_y2yAfterReleasePublications { 0 };
std::atomic<Mutator*> g_satbBeforeMarkEndProducer { nullptr };
std::atomic<BaseObject*> g_satbBeforeMarkEndFirst { nullptr };
std::atomic<BaseObject*> g_satbBeforeMarkEndSecond { nullptr };
std::atomic<uint64_t> g_satbBeforeMarkEndPublications { 0 };
std::atomic<Mutator*> g_exportRootAfterT1Producer { nullptr };
std::atomic<BaseObject*> g_exportRootAfterT1Holder { nullptr };
std::atomic<BaseObject*> g_exportRootAfterT1Child { nullptr };
std::atomic<uint64_t> g_exportRootAfterT1Armed { 0 };
std::atomic<uint64_t> g_exportRootRegistrationsAfterT1 { 0 };
std::atomic<uint64_t> g_exportRootProducerFlushes { 0 };
std::atomic<uint64_t> g_exportRootObservedAtT2 { 0 };
std::atomic<U64> g_exportRootHandle { std::numeric_limits<U64>::max() };
std::atomic<bool> g_exportRootHolderMarked { false };
std::atomic<bool> g_exportRootChildMarked { false };
}

void ResetY2yHandoffTestReceipt()
{
    g_y2yHandoffReceipt.phase0.store(0, std::memory_order_relaxed);
    g_y2yHandoffReceipt.phase1.store(0, std::memory_order_relaxed);
    g_y2yHandoffReceipt.phase2.store(0, std::memory_order_relaxed);
    g_y2yHandoffReceipt.beforeRelease.store(0, std::memory_order_relaxed);
    g_y2yHandoffReceipt.afterRoot.store(0, std::memory_order_relaxed);
    g_y2yHandoffReceipt.afterStw2.store(0, std::memory_order_relaxed);
    g_y2yAfterReleaseHolder.store(nullptr, std::memory_order_relaxed);
    g_y2yAfterReleasePublications.store(0, std::memory_order_relaxed);
}

Y2yHandoffTestReceipt ReadY2yHandoffTestReceipt()
{
    return { g_y2yHandoffReceipt.phase0.load(std::memory_order_relaxed),
             g_y2yHandoffReceipt.phase1.load(std::memory_order_relaxed),
             g_y2yHandoffReceipt.phase2.load(std::memory_order_relaxed),
             g_y2yHandoffReceipt.beforeRelease.load(std::memory_order_relaxed),
             g_y2yHandoffReceipt.afterRoot.load(std::memory_order_relaxed),
             g_y2yHandoffReceipt.afterStw2.load(std::memory_order_relaxed) };
}

void NoteY2yBeforeReleaseTestReceipt(uint64_t pending)
{
    g_y2yHandoffReceipt.phase0.fetch_add(1, std::memory_order_relaxed);
    g_y2yHandoffReceipt.beforeRelease.store(pending, std::memory_order_relaxed);
}

void NoteY2yAfterRootTestReceipt(uint64_t pending)
{
    g_y2yHandoffReceipt.phase1.fetch_add(1, std::memory_order_relaxed);
    g_y2yHandoffReceipt.afterRoot.store(pending, std::memory_order_relaxed);
}

void NoteY2yAfterStw2TestReceipt(uint64_t pending)
{
    g_y2yHandoffReceipt.phase2.fetch_add(1, std::memory_order_relaxed);
    g_y2yHandoffReceipt.afterStw2.fetch_add(pending, std::memory_order_relaxed);
}

void ArmY2yAfterReleaseTestReceipt(BaseObject* holder, uint64_t publications)
{
    g_y2yAfterReleaseHolder.store(holder, std::memory_order_release);
    g_y2yAfterReleasePublications.store(publications, std::memory_order_release);
}

void PublishY2yAfterReleaseTestReceipt()
{
    uint64_t remaining = g_y2yAfterReleasePublications.load(std::memory_order_acquire);
    while (remaining != 0 &&
           !g_y2yAfterReleasePublications.compare_exchange_weak(remaining, remaining - 1,
                                                                 std::memory_order_acq_rel,
                                                                 std::memory_order_acquire)) {}
    if (remaining == 0) {
        return;
    }
    BaseObject* holder = g_y2yAfterReleaseHolder.load(std::memory_order_acquire);
    CHECK_DETAIL(holder != nullptr, "armed y2y after-release receipt without holder");
    AllocBuffer::GetOrCreateAllocBuffer()->PushY2yDirtyHolder(holder);
}

void ArmSatbBeforeMarkEndTestReceipt(Mutator* producer, BaseObject* first, BaseObject* second)
{
    g_satbBeforeMarkEndProducer.store(producer, std::memory_order_release);
    g_satbBeforeMarkEndFirst.store(first, std::memory_order_release);
    g_satbBeforeMarkEndSecond.store(second, std::memory_order_release);
    g_satbBeforeMarkEndPublications.store(2, std::memory_order_release);
}

void PublishSatbBeforeMarkEndTestReceipt()
{
    uint64_t remaining = g_satbBeforeMarkEndPublications.load(std::memory_order_acquire);
    while (remaining != 0 &&
           !g_satbBeforeMarkEndPublications.compare_exchange_weak(remaining, remaining - 1,
                                                                  std::memory_order_acq_rel,
                                                                  std::memory_order_acquire)) {}
    if (remaining == 0) {
        return;
    }
    Mutator* producer = g_satbBeforeMarkEndProducer.load(std::memory_order_acquire);
    BaseObject* object = remaining == 2 ? g_satbBeforeMarkEndFirst.load(std::memory_order_acquire)
                                       : g_satbBeforeMarkEndSecond.load(std::memory_order_acquire);
    CHECK_DETAIL(producer != nullptr && object != nullptr, "armed SATB mark-end receipt without producer/object");
    producer->RememberObjectInSatbBuffer(object);
    producer->FlushSatbBuffer();
}

void ResetExportRootPublicationTestReceipt()
{
    g_exportRootAfterT1Producer.store(nullptr, std::memory_order_relaxed);
    g_exportRootAfterT1Holder.store(nullptr, std::memory_order_relaxed);
    g_exportRootAfterT1Child.store(nullptr, std::memory_order_relaxed);
    g_exportRootAfterT1Armed.store(0, std::memory_order_relaxed);
    g_exportRootRegistrationsAfterT1.store(0, std::memory_order_relaxed);
    g_exportRootProducerFlushes.store(0, std::memory_order_relaxed);
    g_exportRootObservedAtT2.store(0, std::memory_order_relaxed);
    g_exportRootHandle.store(std::numeric_limits<U64>::max(), std::memory_order_relaxed);
    g_exportRootHolderMarked.store(false, std::memory_order_relaxed);
    g_exportRootChildMarked.store(false, std::memory_order_relaxed);
}

void ArmExportRootAfterT1TestReceipt(Mutator* producer, BaseObject* holder, BaseObject* child)
{
    CHECK_DETAIL(producer != nullptr && holder != nullptr && child != nullptr,
                 "export-root T1 receipt requires producer, holder, and child");
    g_exportRootAfterT1Producer.store(producer, std::memory_order_release);
    g_exportRootAfterT1Holder.store(holder, std::memory_order_release);
    g_exportRootAfterT1Child.store(child, std::memory_order_release);
    g_exportRootAfterT1Armed.store(1, std::memory_order_release);
}

void PublishExportRootAfterT1TestReceipt()
{
    if (g_exportRootAfterT1Armed.exchange(0, std::memory_order_acq_rel) == 0) {
        return;
    }
    Mutator* producer = g_exportRootAfterT1Producer.load(std::memory_order_acquire);
    BaseObject* holder = g_exportRootAfterT1Holder.load(std::memory_order_acquire);
    CHECK_DETAIL(producer != nullptr && holder != nullptr, "armed export-root T1 receipt is incomplete");
    Mutator* previous = ThreadLocal::GetMutator();
    ThreadLocal::SetMutator(producer);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(holder);
    ThreadLocal::SetMutator(previous);
    g_exportRootHandle.store(handle, std::memory_order_release);
    g_exportRootRegistrationsAfterT1.fetch_add(1, std::memory_order_relaxed);
}

void FlushExportRootAfterT1TestReceipt()
{
    if (g_exportRootRegistrationsAfterT1.load(std::memory_order_acquire) == 0 ||
        g_exportRootProducerFlushes.exchange(1, std::memory_order_acq_rel) != 0) {
        return;
    }
    Mutator* producer = g_exportRootAfterT1Producer.load(std::memory_order_acquire);
    CHECK_DETAIL(producer != nullptr, "registered export-root T1 receipt has no producer");
    producer->FlushSatbBuffer();
}

void NoteExportRootPublicationAtT2TestReceipt()
{
    if (g_exportRootRegistrationsAfterT1.load(std::memory_order_acquire) == 0) {
        return;
    }
    BaseObject* holder = g_exportRootAfterT1Holder.load(std::memory_order_acquire);
    BaseObject* child = g_exportRootAfterT1Child.load(std::memory_order_acquire);
    auto isMarked = [](BaseObject* object) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        return region != nullptr &&
            region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object);
    };
    g_exportRootHolderMarked.store(isMarked(holder), std::memory_order_relaxed);
    g_exportRootChildMarked.store(isMarked(child), std::memory_order_relaxed);
    g_exportRootObservedAtT2.fetch_add(1, std::memory_order_relaxed);
}

ExportRootPublicationTestReceipt ReadExportRootPublicationTestReceipt()
{
    return { g_exportRootRegistrationsAfterT1.load(std::memory_order_relaxed),
             g_exportRootProducerFlushes.load(std::memory_order_relaxed),
             g_exportRootObservedAtT2.load(std::memory_order_relaxed),
             g_exportRootHandle.load(std::memory_order_relaxed),
             g_exportRootHolderMarked.load(std::memory_order_relaxed),
             g_exportRootChildMarked.load(std::memory_order_relaxed) };
}

#endif
namespace {
bool VerifyStackRootPostconditionEnabled()
{
    static const bool on = []() {
        const char* value = std::getenv("MRT_GCV2_VERIFY_STACK_ROOTS_COMPLETE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return on;
}
} // namespace

namespace WCollectorInternal {
void VerifyStackRootPostcondition(uint64_t stackScanEpoch, const char* source)
{
    if (!VerifyStackRootPostconditionEnabled()) {
        return;
    }

    size_t checked = 0;
    size_t incomplete = 0;
    MutatorManager::Instance().VisitAllMutators([&](Mutator& mutator) {
        ++checked;
        StackWatermark& watermark = mutator.GetStackWatermark();
        if (watermark.IsDone(stackScanEpoch)) {
            return;
        }
        ++incomplete;
        LOG(RTLOG_ERROR,
            "[GCV2][verify][stack-roots-complete] INCOMPLETE source=%s epoch=%llu mutator=%p tid=%u cjthread=%p "
            "managed=%d saferegion=%d wm_epoch=%llu phase=%u owner=%u cursor=%zu frames=%zu "
            "env=MRT_GCV2_VERIFY_STACK_ROOTS_COMPLETE=1",
            source, static_cast<unsigned long long>(stackScanEpoch), &mutator, mutator.GetTid(),
            mutator.GetCjthreadPtr(), mutator.IsManagedContext() ? 1 : 0, mutator.InSaferegion() ? 1 : 0,
            static_cast<unsigned long long>(watermark.GetEpoch()), static_cast<unsigned>(watermark.GetPhase()),
            static_cast<unsigned>(watermark.GetOwner()), watermark.GetCursorIndex(), watermark.GetFrameCount());
    });
    LOG(RTLOG_ERROR,
        "[GCV2][verify][stack-roots-complete] SUMMARY source=%s epoch=%llu checked=%zu incomplete=%zu "
        "env=MRT_GCV2_VERIFY_STACK_ROOTS_COMPLETE=1",
        source, static_cast<unsigned long long>(stackScanEpoch), checked, incomplete);
}
} // namespace WCollectorInternal

void WCollector::ValidateMinorReferences(const char* point, const std::vector<BaseObject*>* reachableVec)
{
    (void)point;
    (void)reachableVec;
}

void WCollector::VerifyRegionSets(const char* point)
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    size_t youngRunIndex = minorTotalRuns + 1;
    if (std::strcmp(point, "after-young-mark") == 0) {
        VerifyRegions::VerifyAfterYoungMark(manager, minorCandidateRegions, youngRunIndex, point);
    } else {
        VerifyRegions::VerifyAfterPrepareYoung(manager, minorCandidateRegions, youngRunIndex, point);
    }
}

void WCollector::ProbeUnmarkedLive(const MinorObjectSet& allocationRoots, const MinorSlotSet& rememberedSlots)
{
    // Default off. When on: independent full-heap retrace from roots (no remset filter),
    // collect young objs reachable that way, compare to region mark bitmap after young-only mark.
    // For each unmarked-but-full-reachable young object, scan non-young holders for incoming
    // old→young edges and report whether that field is in the minor-acquired remset.
    // This probe is what located the mark gap: with MRT_GCV2_MINOR_GC_ALOT forcing young
    // collections, run 69 reported UNMARKED_LIVE=832 with edgeInRemset=0 edgeNotInRemset=28 --
    // every unmarked-live object that had an incoming old->young edge had that edge missing from
    // the remembered set, which is what sent the fix to RecordCrossGenEdge's target-generation
    // test rather than to the consumption side.
    // Off by default -- it retraces the whole heap from roots on every minor, so it is a
    // verification instrument, not something to ship on.  But "off" here used to mean *unreachable*:
    // the env read was pinned to nullptr, so nothing could turn it on without editing this file, and
    // the one measurement that decides whether an old->young edge is being lost simply could not be
    // taken.  A compile-time constant says the same thing while leaving the switch where a reader
    // can find it.  (No new MRT_GCV2_ env var: those were cut 190 -> 3 on purpose.)
    //
    // Flip to true, rebuild, and read [GCV2][markgap] UNMARKED_LIVE / edgeInRemset /
    // edgeNotInRemset.  That is the acceptance criterion for anything touching remembered-set
    // recording -- not remembered-set size, which is a side effect and has already sent this
    // campaign down one wrong path.
    constexpr bool kMarkGapProbe = false;
    if (!kMarkGapProbe) {
        return;
    }

    MinorObjectSet fullReachable;
    MinorObjectSet fullYoung;
    WorkStack pending = NewWorkStack();
    VisitMinorRoots([&pending](BaseObject* object) {
        if (Heap::IsHeapAddress(object)) {
            pending.push_back(object);
        }
    }, [&pending](BaseObject* object) {
        if (Heap::IsHeapAddress(object)) {
            pending.push_back(MarkStackEntry::MarkOnly(object));
        }
    });
    for (BaseObject* object : allocationRoots) {
        pending.push_back(object);
    }
    auto pushField = [this, &pending](RefField<>& field) {
        BaseObject* target = ResolveMinorReference(field);
        if (Heap::IsHeapAddress(target)) {
            pending.push_back(target);
        }
    };
    while (!pending.empty()) {
        const MarkStackEntry entry = pending.back();
        BaseObject* object = entry.object();
        pending.pop_back();
        if (!Heap::IsHeapAddress(object) || !fullReachable.insert(object).second) {
            continue;
        }
        if (!object->IsValidObject()) {
            continue;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region == nullptr) {
            continue;
        }
        if (region->IsYoungRegion()) {
            fullYoung.insert(object);
        }
        if (!entry.follow() || !object->HasRefField()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            HeapSlot<>& referentField =
                HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            BaseObject* referent = ResolveMinorReference(referentField);
            if (Heap::IsHeapAddress(referent)) {
                referent->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
            }
            continue;
        }
        object->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
    }

    size_t unmarkedLive = 0;
    size_t markedYoung = 0;
    size_t missingEdgeHolders = 0;
    size_t edgeInRemset = 0;
    size_t edgeNotInRemset = 0;
    size_t noIncomingOldFound = 0;
    size_t sampleLimit = 8;
    size_t samples = 0;

    for (BaseObject* object : fullYoung) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region == nullptr) {
            continue;
        }
        if (region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object)) {
            ++markedYoung;
            continue;
        }
        ++unmarkedLive;

        // Find old→young incoming edges by independent non-young holder walk.
        size_t incomingOld = 0;
        size_t incomingMissing = 0;
        MAddress sampleField = 0;
        BaseObject* sampleHolder = nullptr;
        Heap::GetHeap().ForEachObj(
            [&](BaseObject* holder) {
                if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                    return;
                }
                RegionInfo* hReg = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
                if (hReg == nullptr || hReg->IsYoungRegion() || hReg->IsGarbageRegion() || hReg->IsFreeRegion()) {
                    return;
                }
                holder->ForEachRefField([&](RefField<>& field) {
                    BaseObject* target = to_object(field.GetTargetObject());
                    if (target != object) {
                        return;
                    }
                    ++incomingOld;
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    bool inRemset = rememberedSlots.count(slot) != 0;
                    if (inRemset) {
                        ++edgeInRemset;
                    } else {
                        ++edgeNotInRemset;
                        ++incomingMissing;
                        if (sampleField == 0) {
                            sampleField = slot;
                            sampleHolder = holder;
                        }
                    }
                });
            },
            false);

        if (incomingMissing > 0) {
            ++missingEdgeHolders;
        }
        if (incomingOld == 0) {
            ++noIncomingOldFound;
        }

        if (samples < sampleLimit) {
            ++samples;
            TypeInfo* ti = object->IsValidObject() ? object->GetTypeInfo() : nullptr;
            TypeInfo* hti = (sampleHolder != nullptr && sampleHolder->IsValidObject()) ? sampleHolder->GetTypeInfo()
                                                                                      : nullptr;
            VLOG(REPORT,
                 "[GCMARKGAP][unmarked-live] run=%zu obj=%p region=%p start=%#zx marked=0 "
                 "fullReachable=1 incomingOld=%zu incomingMissing=%zu sampleField=%p sampleHolder=%p "
                 "objTi=%p holderTi=%p inRemsetSample=%u",
                 minorTotalRuns + 1, object, region, region->GetRegionStart(), incomingOld, incomingMissing,
                 reinterpret_cast<void*>(sampleField), sampleHolder, ti, hti,
                 static_cast<unsigned>(sampleField != 0 && rememberedSlots.count(sampleField) != 0));
        }
    }

    // Also count residual unmarked valid objs on candidates (may be truly dead).
    size_t residualUnmarkedValid = 0;
    size_t residualUnmarkedAndFullReachable = 0;
    size_t neverExaminedCandidates = 0;
    for (RegionInfo* region : minorCandidateRegions) {
        if (region->GetMarkBitmap(region->GetMarkView<Generation::Young>()) == nullptr &&
            region->GetRegionAllocPtr() > region->GetRegionStart()) {
            ++neverExaminedCandidates;
        }
        region->VisitAllObjects([&](BaseObject* object) {
            if (region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object)) {
                return;
            }
            if (!object->IsValidObject()) {
                return;
            }
            ++residualUnmarkedValid;
            if (fullYoung.count(object) != 0) {
                ++residualUnmarkedAndFullReachable;
            }
        });
    }

    VLOG(REPORT,
         "[GCMARKGAP][summary] run=%zu fullYoung=%zu markedYoung=%zu UNMARKED_LIVE=%zu "
         "missingEdgeHolders=%zu edgeInRemset=%zu edgeNotInRemset=%zu noIncomingOld=%zu "
         "residualUnmarkedValid=%zu residualUnmarkedAndFullReachable=%zu neverExaminedCandidates=%zu "
         "remsetSize=%zu env=MRT_GCMARKGAP_PROBE=1",
         minorTotalRuns + 1, fullYoung.size(), markedYoung, unmarkedLive, missingEdgeHolders, edgeInRemset,
         edgeNotInRemset, noIncomingOldFound, residualUnmarkedValid, residualUnmarkedAndFullReachable,
         neverExaminedCandidates, rememberedSlots.size());
}

void WCollector::ValidateYoungMarking(const std::vector<BaseObject*>& reachableVec,
                                      const MinorObjectSet& allocationRoots)
{
    // Gate mirrors ValidateMinorReferences. Default OFF — product path must not abort.
    // Flip kVerifyYoungMarking / kVerifyMarkSource in VerifyOption.h and rebuild.
    // IndependentVsBitmap does NOT require MinorClosure membership, so fullYoungScan
    // is not tautological (gcvheap / HotSpot inventory #22).
    if (!kVerifyYoungMarking) {
        return;
    }

    VerifyMarkSource markSource = ParseVerifyMarkSource();
    const bool useIndependent = markSource == VerifyMarkSource::IndependentVsBitmap ||
                                markSource == VerifyMarkSource::IndependentRetrace ||
                                markSource == VerifyMarkSource::MinorClosure;
    const bool useBitmap = markSource == VerifyMarkSource::IndependentVsBitmap ||
                           markSource == VerifyMarkSource::RegionMarkBitmap ||
                           markSource == VerifyMarkSource::MinorClosure;
    const bool requireMinorClosure = markSource == VerifyMarkSource::MinorClosure;

    MinorObjectSet reachable;
    MinorObjectSet expectedYoung;
    MinorObjectSet minorClosureSet;
    if (requireMinorClosure || (useIndependent && useBitmap)) {
        for (BaseObject* object : reachableVec) {
            minorClosureSet.insert(object);
        }
    }
    if (useIndependent) {
        WorkStack pending = NewWorkStack();
        VisitMinorRoots([&pending](BaseObject* object) {
            if (Heap::IsHeapAddress(object)) {
                pending.push_back(object);
            }
        }, [&pending](BaseObject* object) {
            if (Heap::IsHeapAddress(object)) {
                pending.push_back(MarkStackEntry::MarkOnly(object));
            }
        });
        for (BaseObject* object : allocationRoots) {
            pending.push_back(object);
        }
        auto pushField = [this, &pending](RefField<>& field) {
            BaseObject* target = ResolveMinorReference(field);
            if (Heap::IsHeapAddress(target)) {
                pending.push_back(target);
            }
        };
        while (!pending.empty()) {
            const MarkStackEntry entry = pending.back();
            BaseObject* object = entry.object();
            pending.pop_back();
            if (!reachable.insert(object).second) {
                continue;
            }
            CHECK_DETAIL(object->IsValidObject(), "minor marking validator reached invalid object %p", object);
            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
            if (region->IsYoungRegion()) {
                expectedYoung.insert(object);
            }
            if (!entry.follow() || !object->HasRefField()) {
                continue;
            }
            if (UNLIKELY(object->IsWeakRef())) {
                HeapSlot<>& referentField = HeapSlotAt<>(
                    reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
                BaseObject* referent = ResolveMinorReference(referentField);
                if (Heap::IsHeapAddress(referent)) {
                    referent->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
                }
                continue;
            }
            object->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
        }
    }

    size_t actualYoung = 0;
    size_t unexpectedYoung = 0;
    if (useBitmap) {
        for (RegionInfo* region : minorCandidateRegions) {
            region->VisitAllObjects([&](BaseObject* object) {
                if (!region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object)) {
                    return;
                }
                ++actualYoung;
                bool bad = false;
                if (useIndependent && expectedYoung.count(object) == 0) {
                    bad = true;
                }
                if (requireMinorClosure && minorClosureSet.count(object) == 0) {
                    bad = true;
                }
                if (bad) {
                    ++unexpectedYoung;
                }
            });
        }
    }

    size_t missingYoung = 0;
    size_t expectedCandidateYoung = 0;
    size_t expectedOffCandidateYoung = 0;
    size_t offCandidateMarked = 0;
    size_t offCandidateMinorClosure = 0;
    size_t offCandidateAllocationRoot = 0;
    size_t offCandidateRouteDestHeld = 0;
    constexpr size_t diffSampleLimitPerClass = 4;
    size_t diffSamples = 0;
    size_t diffRouteHeldSamples = 0;
    size_t diffOtherSamples = 0;
    if (useIndependent) {
        for (BaseObject* object : expectedYoung) {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
            const bool inCandidate = minorCandidateRegions.count(region) != 0;
            if (inCandidate) {
                ++expectedCandidateYoung;
            } else {
                ++expectedOffCandidateYoung;
                const bool marked = region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object);
                const bool inMinorClosure = minorClosureSet.count(object) != 0;
                const bool allocationRoot = allocationRoots.count(object) != 0;
                const bool routeDestHeld = region->IsRouteDestHeld();
                offCandidateMarked += static_cast<size_t>(marked);
                offCandidateMinorClosure += static_cast<size_t>(inMinorClosure);
                offCandidateAllocationRoot += static_cast<size_t>(allocationRoot);
                offCandidateRouteDestHeld += static_cast<size_t>(routeDestHeld);
                size_t& classSamples = routeDestHeld ? diffRouteHeldSamples : diffOtherSamples;
                if (classSamples < diffSampleLimitPerClass) {
                    TypeInfo* ti = object->GetTypeInfo();
                    VLOG(REPORT,
                         "[GCV2][verify][young-marking-diff-sample] run=%zu obj=%p typeInfo=%p typeName=%s "
                         "region=%p regionStart=%#zx regionType=%u reachable=1 young=%u marked=%u candidate=0 "
                         "inMinorClosure=%u allocationRoot=%u routeDestHeld=%u markOrigin=%s",
                         minorTotalRuns + 1, object, ti, ti == nullptr ? "<null>" : ti->GetName(), region,
                         region->GetRegionStart(), static_cast<unsigned>(region->GetRegionType()),
                         static_cast<unsigned>(region->IsYoungRegion()), static_cast<unsigned>(marked),
                         static_cast<unsigned>(inMinorClosure), static_cast<unsigned>(allocationRoot),
                         static_cast<unsigned>(routeDestHeld),
                         inMinorClosure ? "minor-closure" : (marked ? "bitmap-preexisting-or-nonclosure" : "none"));
                    ++diffSamples;
                    ++classSamples;
                }
            }
            bool missing = false;
            if (useBitmap &&
                !region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object)) {
                missing = true;
            }
            if (requireMinorClosure && minorClosureSet.count(object) == 0) {
                missing = true;
            }
            if (missing) {
                ++missingYoung;
            }
        }
    }

    if (useIndependent && useBitmap) {
        VLOG(REPORT,
             "[GCV2][verify][young-marking-domain] run=%zu expectedAll=%zu expectedCandidate=%zu "
             "expectedOffCandidate=%zu offCandidateMarked=%zu offCandidateMinorClosure=%zu "
             "offCandidateAllocationRoot=%zu offCandidateRouteDestHeld=%zu samples=%zu",
             minorTotalRuns + 1, expectedYoung.size(), expectedCandidateYoung, expectedOffCandidateYoung,
             offCandidateMarked, offCandidateMinorClosure, offCandidateAllocationRoot,
             offCandidateRouteDestHeld, diffSamples);
    }

    size_t matchCount = (actualYoung >= unexpectedYoung) ? (actualYoung - unexpectedYoung) : 0;
    size_t expectedSize = useIndependent ? (useBitmap ? expectedCandidateYoung : expectedYoung.size()) : actualYoung;
    VLOG(REPORT,
         "[GCV2][verify][young-marking] run=%zu phase=post-trace kVerifyYoungMarking=1 "
         "markSource=%s mark-equivalence=%zu/%zu missing=%zu unexpected=%zu "
         "expectedAll=%zu expectedOffCandidate=%zu requireMinorClosure=%u",
         minorTotalRuns + 1, VerifyMarkSourceName(markSource), matchCount, expectedSize, missingYoung,
         unexpectedYoung, expectedYoung.size(), expectedOffCandidateYoung,
         static_cast<unsigned>(requireMinorClosure));
    if (markSource == VerifyMarkSource::IndependentRetrace || markSource == VerifyMarkSource::RegionMarkBitmap) {
        // Single-source modes only report; cross-check needs two sides.
        return;
    }
    CHECK_DETAIL(missingYoung == 0 && unexpectedYoung == 0 &&
                     (!useIndependent || !useBitmap || actualYoung == expectedCandidateYoung),
                 "minor marking differs from full marking: actualCandidate=%zu expectedCandidate=%zu "
                 "expectedAll=%zu expectedOffCandidate=%zu missing=%zu unexpected=%zu markSource=%s",
                 actualYoung, expectedCandidateYoung, expectedYoung.size(), expectedOffCandidateYoung,
                 missingYoung, unexpectedYoung, VerifyMarkSourceName(markSource));
}

void WCollector::FlushAllocationRegions()
{
    theAllocator.VisitAllocBuffers([](AllocBuffer& buffer) { buffer.FlushRegion(); });
}

void WCollector::DoYoungGarbageCollection()
{
    uint64_t start = TimeUtil::NanoSeconds();
    std::unique_ptr<ScopedStopTheWorld> stw =
        std::make_unique<ScopedStopTheWorld>("young prepare", false);
    // Full-colour gate: reject any plain HeapSlot before young mark mutates colours.
    // This STW entry is the young-only mark start; old marking does not participate in a minor.
    flip_young_mark_start();

    // minortime: STW rendezvous cost is already logged by ScopedStopTheWorld dtor
    // ("young collection stw time N us"). Body timers below exclude that wait.
    // Timeline probe (gcdirty): earliest STW point = mutator just handed control.
    // force via POST_EVAC so we do not need global VERIFY_HEAP (avoids pre-evac side effects).
    if (kVerifyPostEvac) {
        VLOG(REPORT, "[GCV2][verify][post-evac] enter point=stw-enter run=%zu priorMinors=%zu",
             minorTotalRuns + 1, minorTotalRuns);
        VerifyHeapObjects("stw-enter");
        VLOG(REPORT, "[GCV2][verify][post-evac] point=stw-enter run=%zu", minorTotalRuns + 1);
    }
    {
        // minortime: ① FlushAllocationRegions
        MRT_PHASE_TIMER("young.flush_alloc");
        FlushAllocationRegions();
    }

    if (minorTotalRuns != 0) {
        ValidateMinorReferences("round2-start", nullptr);
    }

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    minorCandidateRegions.clear();
    YoungCollectionStats stats;
    {
        // minortime: ② PrepareYoungGarbageCandidates
        MRT_PHASE_TIMER("young.prepare_candidates");
        stats = manager.PrepareYoungGarbageCandidates(
            [this](RegionInfo* region) { minorCandidateRegions.insert(region); });
    }
    VLOG(REPORT,
         "[GCV2][candfix] prepare_candidates candidate_regions=%zu candidate_bytes=%zu "
         "from_visited=%zu from_units=%zu unmovable_visited=%zu unmovable_units=%zu "
         "unmovable_young=%zu unmovable_held=%zu recent_visited=%zu recent_units=%zu "
         "recent_young=%zu recent_held=%zu clear_live_regions=%zu clear_live_units=%zu "
         "objects_visited=%zu slots_visited=%zu repark_ns=%llu unmovable_ns=%llu recent_ns=%llu "
         "hold_ns=%llu clear_live_ns=%llu visitor_ns=%llu list_move_ns=%llu",
         stats.candidateRegions, stats.candidateBytes, stats.fromVisited, stats.fromVisitedUnits,
         stats.unmovableVisited, stats.unmovableVisitedUnits, stats.unmovableYoung, stats.unmovableHeld,
         stats.recentFullVisited, stats.recentFullVisitedUnits, stats.recentFullYoung, stats.recentFullHeld,
         stats.clearLiveRegions, stats.clearLiveUnits, stats.objectVisits, stats.slotVisits,
         static_cast<unsigned long long>(stats.reparkNs), static_cast<unsigned long long>(stats.unmovableNs),
         static_cast<unsigned long long>(stats.recentFullNs), static_cast<unsigned long long>(stats.holdCheckNs),
         static_cast<unsigned long long>(stats.clearLiveNs), static_cast<unsigned long long>(stats.visitorNs),
         static_cast<unsigned long long>(stats.listMoveNs));
    // HotSpot g1HeapVerifier.cpp:424 verify_region_sets placement: after region accounting is stable.
    VerifyRegionSets("after-prepare-young");
    // Region-set verify after candidate construction (HotSpot verify_region_sets placement intent).
    if (kVerifyPostEvac) {
        VLOG(REPORT, "[GCV2][verify][post-evac] enter point=post-prepare-young run=%zu",
             minorTotalRuns + 1);
        VerifyHeapObjects("post-prepare-young");
        VLOG(REPORT, "[GCV2][verify][post-evac] point=post-prepare-young run=%zu", minorTotalRuns + 1);
    }
    if (stats.candidateRegions == 0) {
        manager.ReassembleFromSpace();
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
        ++minorTotalRuns;
        VLOG(REPORT, "[GCV2Minor] run=%zu candidates=0 candidateBytes=0 live=0 reclaimedBytes=0",
             minorTotalRuns);
        return;
    }

    // Pinned holders (Future/Mutex/Monitor): AllocPinned never sets young; IDLE write
    // fast-path (phase < ENUM) is a bare store — old→young edges never hit remset.
    // Stamp them before Acquire so pre-evacuate verify and young mark both see them.
    // idleedge: census remset-miss old→young BEFORE pinned stamp fills those gaps.

    // fysaudit: full non-young O→Y vs mutator remset (D1/D2/D3). Observe only.




    // promodomain: reset last cycle's flip-promoted table (CHECK registered==discharged).
    // Corresponds to ZGC reset_relocation_set before the new young collection.
    PromotedRegionDomain::ResetForNextMinor(minorTotalRuns + 1);
    // flippromo: open broad-vs-product window for regions demoted last minor.

    // ZGC zGeneration.cpp:541-555 has one young sequence: pause-mark-start,
    // concurrent mark/follow, then pause-mark-end.  Flip the previous remset
    // face here; there is no STW young-mark configuration or rollback branch.
    MinorSlotSet rememberedSlots;
    {
        // minortime: ④ remset / cross-gen edge consume (drain + pinned stamp; rescan below)
        MRT_PHASE_TIMER("young.remset_drain");
        RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
        size_t pinnedRemsetRecords = manager.RecordPinnedCrossGenEdges();
        if (pinnedRemsetRecords != 0) {
            VLOG(REPORT, "[GCV2Minor] pinnedCrossGenEdges=%zu", pinnedRemsetRecords);
        }
        // d1producer: D1 counts misses against the *mutator* remset at :5204, but the pinned walk
        // above drains into this same minor. Ask here, before the drain, how many D1 edges the
        // walk put back — the residual is what FYS=0 really loses. Observe only, default off.

        StoreBarrierBuffer::FlushAll(rememberedSet);
        // S5 flip only (YOUNG_CONCURRENT.md). ScanPreviousForMinor runs after
        // world-release with mark_follow (zRemembered.cpp:561-576).
        rememberedSet.FlipForMinor();

    }

    uint64_t stackScanEpoch = 0;
    {
        // Publish S1/S3/S5 while every mutator is stopped. SetGCPhase is the
        // release publication point; AcknowledgeEpochHandshake asserts ENUM
        // before it is allowed to snapshot a single frame.
        Heap::GetHeap().InstallBarrier(GCPhase::GC_PHASE_ENUM);
        Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
        stw.reset();


        EpochHandshakeStats handshake = MutatorManager::Instance().RunEpochHandshake("pre-minor-stack");
        stackScanEpoch = handshake.epoch;
        CHECK_DETAIL(stackScanEpoch != 0 && handshake.stackScanned + handshake.stackFallback == handshake.requested,
                     "minor concurrent stack scan accounting failed: epoch=%llu requested=%zu scanned=%zu "
                     "fallback=%zu",
                     static_cast<unsigned long long>(stackScanEpoch), handshake.requested, handshake.stackScanned,
                     handshake.stackFallback);

        // CLEAR is the closing edge for ENUM writes: it flushes every mutator's
        // SATB node before the root pass consumes retired objects below.
        stw = std::make_unique<ScopedStopTheWorld>("young collection", false);
        TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);

        // finish_processing semantics: under the closing STW first ask the GC
        // owner to complete every unfinished epoch cursor. If a cursor still
        // cannot establish DONE (for example, missing managed bounds), preserve
        // the exact legacy phase-enum fallback before roots are merged.
        MutatorManager::Instance().VisitAllMutators([stackScanEpoch](Mutator& mutator) {
            if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                (void)mutator.GcPhaseEnum(GCPhase::GC_PHASE_ENUM, stackScanEpoch, false);
            }
            if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                (void)mutator.GcPhaseEnum(GCPhase::GC_PHASE_ENUM);
            }
        });
        VerifyStackRootPostcondition(stackScanEpoch, "minor");

    }

    constexpr bool fullYoungScan = false;
    // remsetdrain: hash-work reduction defaults on; `=0` is the immediate rollback.
    // The drain side uses the bitmap's exact distinct count to reserve its destination.
    // The FYS-only consumed-ledger elision is fixed by the sole concurrent path.
    static const bool remsetHashOptRequested = []() {
        const char* value = std::getenv("MRT_GCV2_REMSET_HASH_OPT");
        return value == nullptr || std::strcmp(value, "1") == 0;
    }();
    // setbitmap O1③: default ON (bitmap claim + vector). MRT_GCV2_SETBITMAP=0 → legacy set path.
    static const bool useBitmapLedger = []() {
        const char* v = std::getenv("MRT_GCV2_SETBITMAP");
        if (v != nullptr && std::strcmp(v, "0") == 0) {
            return false;
        }
        return true;
    }();
    WorkStack workStack = NewWorkStack();
    MinorObjectSet reachableObjects; // legacy set path + FYS non-young holders under bitmap
    std::vector<BaseObject*> reachableVec;
    reachableVec.reserve(1 << 17); // ~128k; real_load ~155k reachable
    MinorObjectSet allocationRoots;
    MinorObjectSet currentMinorRoots;
    MinorSlotSet reachableSlots;
    MinorSlotSet weakSlots;
    if (remsetHashOptRequested && fullYoungScan) {
        // Runtime lower bound only: if holder closure covers the remset, this
        // avoids growth rehashes; if it does not, unordered_set still grows
        // normally.  Capacity does not admit or discard a slot.
        reachableSlots.reserve(rememberedSlots.size());
    }
    auto mergeY2yDirtyHolders = [&](WorkStack& destination) {
        theAllocator.VisitAllocBuffers([&destination](AllocBuffer& buffer) {
            buffer.MergeY2yDirtyHolders(destination);
        });
    };
#if defined(MRT_TESTABLE_INTERNALS)
    auto pendingY2yDirtyHolderCount = [&]() {
        size_t pending = 0;
        theAllocator.VisitAllocBuffers([&pending](AllocBuffer& buffer) {
            pending += buffer.Y2yDirtyHolderCount();
        });
        return pending;
    };
#endif
    // ZGC zGeneration.cpp:665-669: root production belongs to concurrent_mark.
    // Keep one producer (the existing owner-specific VisitMinorRoots/epoch path),
    // selecting only its phase boundary. MARK-only remains a diagnostic arm and
    // therefore keeps the producer under its pause; MARK+FOLLOW invokes it after
    // the world-release publication below.
    auto produceYoungRoots = [&]() {
        // minortime: ③ root enum (alloc buffers + VisitMinorRoots)
        MRT_PHASE_TIMER("young.root_enum");
        WorkStack enumRoots = NewWorkStack();
        theAllocator.VisitAllocBuffers([&enumRoots](AllocBuffer& buffer) { buffer.MergeRoots(enumRoots); });
        if (stackScanEpoch != 0) {
            SatbBuffer::Instance().GetRetiredObjects(enumRoots);
        }
        while (!enumRoots.empty()) {
            const MarkStackEntry entry = enumRoots.back();
            BaseObject* object = entry.object();
            enumRoots.pop_back();
            if (Heap::IsHeapAddress(object)) {
                allocationRoots.insert(object);
            }
            if (entry.follow()) {
                PushYoungObject(object, workStack, "alloc_buffer");
            } else if (Heap::IsHeapAddress(object)) {
                workStack.push_back(MarkStackEntry::MarkOnly(object));
            }
        }
        VisitMinorRoots([this, &workStack, &currentMinorRoots](BaseObject* object) {
            if (Heap::IsHeapAddress(object)) {
                RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
                if (region != nullptr && !region->IsYoungRegion()) {
                    currentMinorRoots.insert(object);
                }
            }
            PushYoungObject(object, workStack, "minor_root");
        }, [&workStack, &currentMinorRoots](BaseObject* object) {
            if (!Heap::IsHeapAddress(object)) {
                return;
            }
            RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
            if (region != nullptr && !region->IsYoungRegion()) {
                currentMinorRoots.insert(object);
            }
            workStack.push_back(MarkStackEntry::MarkOnly(object));
        }, stackScanEpoch);
#if defined(MRT_TESTABLE_INTERNALS)
        NoteY2yAfterRootTestReceipt(enumRoots.size());
#endif
    };
    // ZGC zGeneration.cpp:665-692: roots and follow are the single concurrent
    // young-mark path.  Mark-end convergence is owned by MarkYoungSatbBuffer's
    // termination protocol, not by a pause-local discovery loop.
    YoungConcWindowStats concWindow;
    uint64_t concWindowStartNs = 0;
    // markstw: reachableSlots is queried only with members of rememberedSlots in the
    // non-concurrent FYS path.  Keep the exact intersection instead of materialising
    // every reachable heap field.  Concurrent young marking is deliberately excluded:
    // its STW2 admits slots recorded after this initial remset snapshot.
    const MinorSlotSet* reachableSlotDomain = nullptr;
    // portyoungconc L2: this is ZGC's boundary. Everything above is pause_mark_start
    // (colour flip, retire, remset flip) only; roots and follow are concurrent_mark().
    // Release here before invoking the existing root producer so mark_follow runs
    // with mutators alive.
    {
        CHECK_DETAIL(stw != nullptr, "young concurrent mark start without pause owner");
        CHECK_DETAIL(stackScanEpoch != 0,
                     "young FOLLOW requires an epoch-backed concurrent stack-root receipt");
        concWindow.markedAtEntry = reachableVec.size();
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        // wave8 y2y handoff (8d4253522 content): consume the pre-window batch
        // before reset releases mutators. New stores after reset remain owned
        // by the STW2 consumer and cannot race this allocator-buffer merge.
        mergeY2yDirtyHolders(workStack);
#if defined(MRT_TESTABLE_INTERNALS)
        NoteY2yBeforeReleaseTestReceipt(pendingY2yDirtyHolderCount());
#endif
        stw.reset();
#if defined(MRT_TESTABLE_INTERNALS)
        PublishY2yAfterReleaseTestReceipt();
#endif
        concWindowStartNs = TimeUtil::NanoSeconds();
        produceYoungRoots();
        VLOG(REPORT,
             "[GCV2][youngconc] concurrent young mark start (FOLLOW: roots+closure concurrent) "
             "roots_marked=%zu",
             concWindow.markedAtEntry);
    }
    {
        // minortime: ⑤ mark closure pass-1 (from roots)
        // The release above makes this ZGC mark_roots()+mark_follow work concurrent.
        MRT_PHASE_TIMER("young.mark_closure");
        ++concWindow.closureCalls;
        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                          useBitmapLedger, reachableSlotDomain);
    }
    const bool remsetConsumedLedgerElideActive = false;

    if (rememberedSlots.empty()) {
        // scan_and_follow (zRemembered.cpp:561-576): previous face as grey
        // roots, mutators alive. Flip already happened under STW1.
        concWindow.remsetSlots =
            Heap::GetHeap().GetRememberedSet().ScanPreviousForMinor(rememberedSlots);
    }

    MinorSlotSet liveRememberedSlots;
    if (remsetHashOptRequested && !remsetConsumedLedgerElideActive) {
        liveRememberedSlots.reserve(rememberedSlots.size());
    }
    size_t liveRememberedCount = 0;
    for (MAddress slot : rememberedSlots) {
        if (LedgerCount(weakSlots, slot) == 0 &&
            (!fullYoungScan ||
             LedgerCount(reachableSlots, slot) != 0)) {
            ++liveRememberedCount;
            if (!remsetConsumedLedgerElideActive) {
                liveRememberedSlots.insert(slot);
            }
        }
    }
    RemsetScanStats remsetStats;
    remsetStats.recorded = rememberedSlots.size();
    remsetStats.live = liveRememberedCount;
    MinorSlotSet consumedSlots;
    if (remsetHashOptRequested && !remsetConsumedLedgerElideActive) {
        consumedSlots.reserve(rememberedSlots.size());
    }
    MinorInteriorBaseMap remsetInteriorBases;
    {
        // minortime: ④ remset rescan + ⑤ mark closure pass-2 (from remset edges)
        MRT_PHASE_TIMER("young.remset_rescan");
        RescanRememberedSet(workStack, rememberedSlots, reachableSlots, weakSlots, currentMinorRoots,
                            fullYoungScan,
                            remsetConsumedLedgerElideActive ? nullptr : &consumedSlots, &remsetStats,
                            &remsetInteriorBases, stw.get());
    }
    if (remsetHashOptRequested) {
        VLOG(REPORT,
             "[GCV2][remsetdrain][hash-opt] requested=1 active=%u recorded=%zu live=%zu consumed=%zu "
             "consumedLedger=%zu interiors=%zu fys=%u youngConc=%u",
              static_cast<unsigned>(remsetConsumedLedgerElideActive), rememberedSlots.size(), remsetStats.live,
              remsetStats.consumed, consumedSlots.size(), remsetInteriorBases.size(),
              static_cast<unsigned>(fullYoungScan), 1U);
    }
    // fysaudit: D2 retained-drop + D4 live-not-consumed (product path already FYS=0 under audit).

    {
        MRT_PHASE_TIMER("young.mark_from_remset");
        ++concWindow.closureCalls;
        concWindow.remsetSlots = remsetStats.consumed;
        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                          useBitmapLedger, reachableSlotDomain);
    }
#if defined(MRT_TESTABLE_INTERNALS)
    // Deterministic T1->T2 export-root window: root enumeration has returned,
    // and the first shared-domain SATB consumer has not started yet.
    PublishExportRootAfterT1TestReceipt();
#endif
    for (;;) {
        // Concurrent mark-follow drains work published by the previous pause.
        // Its worker completion is coordinated by YoungMarkTerminate (the
        // ZMarkTerminate worker-count/wakeup state machine), not pool polling.
        const bool workersTerminated =
            MarkYoungSatbBuffer(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                weakSlots, useBitmapLedger, &concWindow);
        CHECK_DETAIL(workersTerminated, "young concurrent mark workers did not terminate");
#if defined(MRT_TESTABLE_INTERNALS)
        FlushExportRootAfterT1TestReceipt();
        // Adversarial mutator publication point: workers have terminated, but
        // the pause has not started. The pause must flush once and return
        // failure; it must not consume closure in an in-pause loop.
        PublishSatbBeforeMarkEndTestReceipt();
#endif

        // ZGenerationYoung::pause_mark_end() does one flush. Work found here is
        // not processed in the pause: releasing this owner and restoring TRACE
        // is the existing concurrent_mark_continue edge.
#if defined(MRT_TESTABLE_INTERNALS)
        const uint64_t markEndPauseStartNs = TimeUtil::NanoSeconds();
#endif
        stw = std::make_unique<ScopedStopTheWorld>("young mark terminate", true,
                                                   GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
#if defined(MRT_TESTABLE_INTERNALS)
        const size_t y2yBatchAtMarkEnd = pendingY2yDirtyHolderCount();
#endif
        theAllocator.VisitAllocBuffers([&workStack](AllocBuffer& buffer) {
            // The CLEAR_SATB transition also closes each mutator's epoch stack
            // publication. Late stack roots are mark work, just like ZGC's
            // thread-local mark stacks flushed by ZMark::try_end().
            buffer.MergeRoots(workStack);
            buffer.MergeYoungAllocBlack(workStack);
            buffer.MergeY2yDirtyHolders(workStack);
        });
#if defined(MRT_TESTABLE_INTERNALS)
        NoteY2yAfterStw2TestReceipt(y2yBatchAtMarkEnd);
#endif
        const bool markEndSucceeded = TryEndYoungMark(workStack, &concWindow);
#if defined(MRT_TESTABLE_INTERNALS)
        NoteMarkTerminatePauseDuration(TimeUtil::NanoSeconds() - markEndPauseStartNs);
#endif
        if (workersTerminated && markEndSucceeded) {
#if defined(MRT_TESTABLE_INTERNALS)
            NoteExportRootPublicationAtT2TestReceipt();
#endif
            break;
        }
        NoteMarkTerminateContinue(workStack.size());
        ++concWindow.reenters;
        stw.reset();
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
#if defined(MRT_TESTABLE_INTERNALS)
        PublishY2yAfterReleaseTestReceipt();
#endif
    }
    ReportMarkTerminateContinue();
    {
        // Window closes here: the next statement asks every mutator to stop. Read the pair
        // (windowNs, MarkedInWindow) together -- duration alone proves nothing.
        concWindow.markedAtExit = reachableVec.size();
        if (concWindowStartNs != 0) {
            concWindow.windowNs = TimeUtil::NanoSeconds() - concWindowStartNs;
        }
        // The successful mark-end owner is retained for evacuation handoff.
        // Every allocator/y2y batch was either empty at this pause or forced a
        // failed mark-end and was processed by concurrent_mark_continue.        // Rebuild liveRememberedSlots after concurrent remset merge (stats/audit only;
        // EvacuateYoungRegions remset authority is consumedSlots — fysfixa 3f27f0c4).
        liveRememberedSlots.clear();
        liveRememberedCount = 0;
        for (MAddress slot : rememberedSlots) {
            if (LedgerCount(weakSlots, slot) == 0 &&
                (!fullYoungScan ||
                 LedgerCount(reachableSlots, slot) != 0)) {
                liveRememberedSlots.insert(slot);
                ++liveRememberedCount;
            }
        }
        remsetStats.live = liveRememberedCount;
        VLOG(REPORT, "[GCV2][youngconc] concurrent young mark done; STW2 evacuation handoff reachable=%zu",
             reachableVec.size());
    }
    // portyoungconc positive control. Emitted on EVERY minor, including the closed arm, so
    // "no line" and "a line of zeros" are distinguishable. window_ns is the only field that
    // a merely-existing window can raise; marked_in_window / satb_objects / closure_calls
    // are GC work, and it is the work fields that decide whether the window is real.
    VLOG(REPORT,
         "[GCV2][youngconc][concwork] run=%zu conc=%d follow=%d window_ns=%llu marked_in_window=%zu "
         "satb_objects=%zu satb_iters=%zu closure_calls=%zu remset_slots=%zu reenters=%zu "
         "marked_at_entry=%zu reachable_total=%zu",
         minorTotalRuns + 1, 1, 1,
         static_cast<unsigned long long>(concWindow.windowNs), concWindow.MarkedInWindow(),
         concWindow.satbObjects, concWindow.satbIters, concWindow.closureCalls, concWindow.remsetSlots,
         concWindow.reenters, concWindow.markedAtEntry, reachableVec.size());
    VLOG(REPORT, "[GCV2][setbitmap] use=%d reachable_n=%zu set_n=%zu fullYoung=%d youngConc=%d",
         static_cast<int>(useBitmapLedger), reachableVec.size(), reachableObjects.size(),
         static_cast<int>(fullYoungScan), 1);
    // No independent full-root closure is available after deleting the empty
    // explainer. nullptr means "not measured"; an empty set must mean a closure
    // actually ran and found no holders.
    VerifyRememberedSetInvariant("pre-evacuate", rememberedSlots, nullptr);

    // Full-heap object invariant H (HotSpot G1HeapVerifier::verify inventory #10).
    // Independent ForEachObj walk; gated by MRT_GCV2_VERIFY_HEAP (default off).
    // Timeline (gcdirty): also force as post-mark under POST_EVAC so first-dirty bracketing
    // does not require global VERIFY_HEAP.
    if (kVerifyPostEvac) {
        VLOG(REPORT, "[GCV2][verify][post-evac] enter point=post-mark run=%zu", minorTotalRuns + 1);
        VerifyHeapObjects("post-mark", nullptr);
        VLOG(REPORT, "[GCV2][verify][post-evac] point=post-mark run=%zu", minorTotalRuns + 1);
    } else {
        VerifyHeapObjects("pre-evacuate", nullptr);
    }

    size_t liveBytes = 0;
    TenuringInputs tenuringIn;
    tenuringIn.softMaxCapacity = Heap::GetHeap().GetMaxCapacity();
    tenuringIn.youngAllocated = stats.candidateBytes;
    for (RegionInfo* region : minorCandidateRegions) {
        const size_t live = region->GetLiveByteCount();
        liveBytes += live;
        uint32_t age = region->GetYoungAge();
        if (age >= kPageAgeCount) {
            age = untype(PageAge::survivor14);
        }
        tenuringIn.liveByAge[age] += live;
    }
    tenuringIn.youngGarbage = stats.candidateBytes > liveBytes ? (stats.candidateBytes - liveBytes) : 0;
    GCStats& gcStats = GetGCStats();
    gcStats.youngCandidateBytes = stats.candidateBytes;
    gcStats.youngPromotedBytes = liveBytes;
    for (uint32_t i = 0; i < kPageAgeCount; ++i) {
        gcStats.liveByAge[i] = tenuringIn.liveByAge[i];
    }
    gcStats.tenuringThreshold = ComputeTenuringThreshold(tenuringIn);
    if (fullYoungScan) {
        // Run structural verify before mark-equivalence CHECK (may abort).
        VerifyRegionSets("after-young-mark");
    }
    // Self-gated by kVerifyYoungMarking. Do not hide it behind fullYoungScan:
    // FYS is compile-time false, and IndependentVsBitmap does not need FYS.
    ValidateYoungMarking(reachableVec, allocationRoots);
    // Always-available (gated) probe: full-heap independent reachability vs young-only bitmap.
    // Runs with FULL_YOUNG_SCAN=0 so B2 path is exercised. Default off.
    ProbeUnmarkedLive(allocationRoots, rememberedSlots);

    {
        // minortime: ⑧ pre-evac finish (phase + weak/satb clear)
        MRT_PHASE_TIMER("young.pre_evac_clear");
        TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
        // tracecache: PrepareTrace above switched the TRACE-phase region caches on
        // (RegionManager.h:726-727), and this is the young mark's post-trace point -- the
        // same place WCollector::PostTrace drains them for a major (RelocationSet.cpp:73-78).
        // Without this call the minor leaves the cache active forever, so every region a
        // mutator fills afterwards is diverted off recentFullRegionList and is invisible to
        // both collection-set builders (PrepareYoungGarbageCandidates and
        // AssembleSmallGarbageCandidates) until the next major's PostTrace.  Measured on
        // NW256: 3744 regions / 245 MB parked in the cache at the end of the first minor,
        // so the first major's collection set was 23 MB of a 256 MB full heap.
        // ZGC keeps every page in the page table and lets ZGeneration::select_relocation_set
        // walk all of them, skipping only pages allocated during this very cycle
        // (zGeneration.cpp:206-218; ZPage::is_relocatable, zPage.inline.hpp:184-186).  A page
        // filled during marking is an ordinary candidate next cycle; it is never removed
        // from the structure the selector iterates.
        space.GetRegionManager().HandleTraceRegions();
        SatbBuffer::Instance().ClearBuffer();
    }

    size_t allocatedBefore = space.AllocatedBytes();
    // ⑥⑦⑧ inside EvacuateYoungRegions: pause relocate_start / concurrent copy / evac_finish
    // Pass STW so Phase 8 can release the world for concurrent_relocate.
    //
    // fysfixa / fysaudit D4: slot authority for remset fix = Rescan-admitted
    // consumedSlots, not the pre-rescan liveRememberedSlots ledger.
    // liveRememberedSlots under FYS=0 = all non-weak recorded (WCollector.cpp
    // live-build above); Rescan may drop retained-dead / free-holder / bad_target
    // without consuming, yet old Evacuate still Fixed those slots → from-object
    // not in liveInfo0 → AdmitForRoute miss → ForwardObjectExclusive
    // "invalid object route" (fysfloor B10). FYS=1 masked via reachableSlots
    // filtering both live-build and Rescan. Unifying on consumed restores
    // fix-domain ⊆ mark/route-domain without widening AdmitForRoute.
    if (remsetStats.live != remsetStats.consumed) {
        VLOG(REPORT,
             "[GCV2][fysfixa] remset_slot_authority live=%zu consumed=%zu gap=%zu "
             "(evac uses consumed)",
             remsetStats.live, remsetStats.consumed,
             remsetStats.live > remsetStats.consumed ? remsetStats.live - remsetStats.consumed : 0);
    }
    // In non-concurrent FYS, RescanRememberedSet only consumes slots in reachableSlots;
    // their holders are in reachableVec and will be scanned by FixMinorObjectSlots.
    // Concurrent mark force-admits slots without that proof.
    const bool refFixSlotsCoveredByReachable = false;
    EvacuateYoungRegions(reachableVec, consumedSlots, currentMinorRoots, refFixSlotsCoveredByReachable,
                         remsetInteriorBases, &stw);
    size_t allocatedAfter = space.AllocatedBytes();
    stats.reclaimedBytes = allocatedBefore > allocatedAfter ? allocatedBefore - allocatedAfter : 0;
    GetGCStats().collectedBytes = stats.reclaimedBytes;

    // Post-evacuate invariant P (HotSpot VerifyAfterGC analog for young): after
    // fix+forward+remset rebuild inside EvacuateYoungRegions, every live ref must
    // still be a legal object (VerifyHeap H) and remset must cover old→young (R).
    // Gate default off: kVerifyPostEvac. force=true so this does not
    // require MRT_GCV2_VERIFY_HEAP/REMSET (avoids pre-evacuate side effects).
    if (kVerifyPostEvac) {
        VerifyHeapObjects("post-evacuate");
        std::unordered_set<MAddress> remsetSnap = Heap::GetHeap().GetRememberedSet().Snapshot();
        VerifyRememberedSetInvariant("post-evacuate", remsetSnap);
        ValidateMinorReferences("post-evacuate", nullptr);
        VLOG(REPORT,
             "[GCV2][verify][post-evac] point=post-evacuate run=%zu "
             "kVerifyPostEvac=1 remsetSnap=%zu",
             minorTotalRuns + 1, remsetSnap.size());
    }

    // Residual Register and the remset walk now both complete in STW3, before
    // EvacuateYoungRegions retires the forwarding receipts. Then enter IDLE.
    if (stw != nullptr) {
        stw.reset();
    }

    {
        // minortime: ⑧ post-evac finish
        MRT_PHASE_TIMER("young.post_evac_finish");
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
        MergeResurrectExportObjects();
    }
    ++minorTotalRuns;
    uint64_t pauseUs = (TimeUtil::NanoSeconds() - start) / NS_PER_US;
    VLOG(REPORT,
         "[GCV2Minor] run=%zu fallbackFullScan=%u candidates=%zu candidateBytes=%zu liveBytes=%zu "
         "remembered=%zu reclaimedBytes=%zu pause=%zu us",
         minorTotalRuns, static_cast<unsigned>(fullYoungScan), stats.candidateRegions, stats.candidateBytes,
         liveBytes, liveRememberedCount, stats.reclaimedBytes, pauseUs);
    // csetalloc: surface cumulative "would allocate into CSet" count (always-on counter,
    // zero-cost when no hits; LOG only if non-zero so default noise stays quiet).
    {
        size_t into = RegionSpace::AllocIntoCSetCount();
        size_t retired = RegionSpace::AllocIntoCSetRetiredCount();
        if (into != 0 || retired != 0) {
            VLOG(REPORT, "[GCV2][csetalloc] cumulative intoCSet=%zu retired=%zu (post-minor run=%zu)",
                 into, retired, minorTotalRuns);
        }
    }
    // STEER4: DumpScrubCostAndReset is a no-op unless MRT_GCV2_SCRUB_COST=1.
    RegionManager::DumpScrubCostAndReset("post-minor");



}
} // namespace MapleRuntime
