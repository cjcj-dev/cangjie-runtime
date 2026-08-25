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
    const bool concurrentStackScan = MutatorManager::ConcurrentStackScanEnabled();
    if (UNLIKELY(!concurrentStackScan && MutatorManager::EpochHandshakeEnabled())) {
        (void)MutatorManager::Instance().RunEpochHandshake("pre-minor");
    }
    std::unique_ptr<ScopedStopTheWorld> stw;
    if (concurrentStackScan) {
        stw = std::make_unique<ScopedStopTheWorld>("young prepare", false);
    } else {
        stw = std::make_unique<ScopedStopTheWorld>("young collection", true, GCPhase::GC_PHASE_ENUM);
    }
    // plaincensus Phase 1a: measure plain HeapSlots before young mark mutates colours.
    // This STW entry is the young-only mark start; old marking does not participate in a minor.
    flip_young_mark_start();

    // minortime: STW rendezvous cost is already logged by ScopedStopTheWorld dtor
    // ("young collection stw time N us"). Body timers below exclude that wait.
    // Timeline probe (gcdirty): earliest STW point = mutator just handed control.
    // force via POST_EVAC so we do not need global VERIFY_HEAP (avoids pre-evac side effects).
    if (kVerifyPostEvac) {
        VLOG(REPORT, "[GCV2][verify][post-evac] enter point=stw-enter run=%zu priorMinors=%zu",
             minorTotalRuns + 1, minorTotalRuns);
        VerifyHeapObjects("stw-enter", true);
        VLOG(REPORT, "[GCV2][verify][post-evac] point=stw-enter run=%zu", minorTotalRuns + 1);
    }
    if (!concurrentStackScan) {
        TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
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
        VerifyHeapObjects("post-prepare-young", true);
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

    // Flags must be known before remset drain: FOLLOW STW1 only flips
    // (zRememberedSet.cpp:36), scan is concurrent (zRemembered.cpp:561-576).
    static const bool youngConcMark = []() {
        const char* v = std::getenv("MRT_GCV2_YOUNG_CONC_MARK");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    static const bool youngConcFollowRequested = []() {
        const char* v = std::getenv("MRT_GCV2_YOUNG_CONC_FOLLOW");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    const bool youngConcFollow = youngConcFollowRequested && youngConcMark;
    // FOLLOW cannot publish stack roots without an epoch receipt.  Reuse the
    // existing handshake even when only the explicit epoch knob is enabled;
    // otherwise fail closed before releasing the world.
    const bool concurrentYoungRoots = concurrentStackScan ||
        (youngConcFollow && MutatorManager::EpochHandshakeEnabled());
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
        if (youngConcFollow) {
            // S5 flip only (YOUNG_CONCURRENT.md). ScanPreviousForMinor runs after
            // world-release with mark_follow (zRemembered.cpp:561-576).
            rememberedSet.FlipForMinor();
        } else {
            rememberedSet.DrainForMinor(rememberedSlots);
        }

    }

    uint64_t stackScanEpoch = 0;
    if (concurrentYoungRoots) {
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
    // The FYS-only consumed-ledger elision is decided later, after youngConcMark is known.
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
        // h3seed2 甲: merge y2y dirty holders (object seeds) before VisitMinorRoots.
        size_t y2yDirtyN = 0;
        theAllocator.VisitAllocBuffers([&enumRoots, &y2yDirtyN](AllocBuffer& buffer) {
            y2yDirtyN += buffer.Y2yDirtyHolderCount();
            buffer.MergeY2yDirtyHolders(enumRoots);
        });
        if (y2yDirtyN != 0) {
            VLOG(REPORT, "[GCV2Minor] y2yDirtyHolders=%zu (h3seed2 object seeds, not field remset)", y2yDirtyN);
        }
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
    };
    if (!youngConcFollow) {
        produceYoungRoots();
    }
    // youngconc: concurrent young mark (mutator-concurrent, not only STW-parallel).
    // Default OFF until STW2 remset/root fixpoint is checksum-clean (see REPORT-youngconc).
    // MRT_GCV2_YOUNG_CONC_MARK=1 enables; reuses major TRACE barrier + SATB (no second family).
    // MARK-only diagnostic arm keeps root enum/closure in STW1;
    // MARK+FOLLOW publishes only the start state in STW1 and runs roots/follow concurrently.
    // STW2 = concurrent remset drain + re-enum + evacuate.
    // youngConcMark / youngConcFollow computed above (before remset drain).
    // portyoungconc L1 (ZGC zGeneration.cpp:550-555): mark_end() returning false leaves the
    // safepoint and runs concurrent_mark_continue() = mark_follow() before re-entering.
    // A non-converged mark end must re-enter; FORCE=<n> makes exactly the first n mark-ends
    // report "not converged" so the edge has a positive control. The product loop has no
    // re-entry budget, matching zGeneration.cpp:550-555; the diagnostic request remains
    // self-limiting through concWindow.reenters < youngMarkEndForceReenter.
    static const size_t youngMarkEndForceReenter = []() -> size_t {
        const char* v = std::getenv("MRT_GCV2_YOUNG_MARK_END_FORCE_REENTER");
        if (v == nullptr) {
            return 0;
        }
        long parsed = std::strtol(v, nullptr, 10);
        return parsed > 0 ? static_cast<size_t>(parsed) : 0;
    }();
    YoungConcWindowStats concWindow;
    uint64_t concWindowStartNs = 0;
    // markstw: reachableSlots is queried only with members of rememberedSlots in the
    // non-concurrent FYS path.  Keep the exact intersection instead of materialising
    // every reachable heap field.  Concurrent young marking is deliberately excluded:
    // its STW2 admits slots recorded after this initial remset snapshot.
    // Keep opt-in (`=1`): `=0` or unset preserves the unrestricted-ledger path.
    const MinorSlotSet* reachableSlotDomain = nullptr;
    // portyoungconc L2: this is ZGC's boundary. Everything above is pause_mark_start
    // (colour flip, retire, remset flip) only; roots and follow are concurrent_mark().
    // Release here before invoking the existing root producer so mark_follow runs
    // with mutators alive.
    if (youngConcFollow && stw != nullptr) {
        CHECK_DETAIL(stackScanEpoch != 0,
                     "young FOLLOW requires an epoch-backed concurrent stack-root receipt");
        concWindow.markedAtEntry = reachableVec.size();
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        stw.reset();
        concWindowStartNs = TimeUtil::NanoSeconds();
        produceYoungRoots();
        VLOG(REPORT,
             "[GCV2][youngconc] concurrent young mark start (FOLLOW: roots+closure concurrent) "
             "roots_marked=%zu",
             concWindow.markedAtEntry);
    }
    {
        // minortime: ⑤ mark closure pass-1 (from roots)
        // MARK-only runs this closure under STW; MARK+FOLLOW reaches this block after
        // the release above, matching ZGC mark_roots()+mark_follow in concurrent_mark.
        MRT_PHASE_TIMER("young.mark_closure");
        if (youngConcFollow) {
            ++concWindow.closureCalls;
        }
        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                          useBitmapLedger, reachableSlotDomain);
    }
    const bool remsetConsumedLedgerElideActive = false;

    if (youngConcFollow && rememberedSlots.empty()) {
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
              static_cast<unsigned>(fullYoungScan), static_cast<unsigned>(youngConcMark));
    }
    // fysaudit: D2 retained-drop + D4 live-not-consumed (product path already FYS=0 under audit).

    size_t reachableBeforeRemsetClosure = reachableVec.size();
    {
        MRT_PHASE_TIMER("young.mark_from_remset");
        if (youngConcFollow) {
            ++concWindow.closureCalls;
            concWindow.remsetSlots = remsetStats.consumed;
        }
        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                          useBitmapLedger, reachableSlotDomain);
    }
    if (youngConcMark && stw != nullptr) {
        // concwin: release only after the STW1 snapshot (roots + remset drain) is marked.
        // Mutators then run under TraceBarrier/SATB; STW2 still reseals + evacuates.
        // Not reached under YOUNG_CONC_FOLLOW: that arm already released above.
        concWindow.markedAtEntry = reachableVec.size();
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        stw.reset();
        concWindowStartNs = TimeUtil::NanoSeconds();
        VLOG(REPORT, "[GCV2][youngconc] concurrent young mark start (mutators running; STW1 snapshot marked)");
    }
    if (youngConcMark) {
        // SATB termination while concurrent (major MarkSatbBuffer shape). Ends in CLEAR_SATB.
        CHECK_DETAIL(MarkYoungSatbBuffer(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                         weakSlots, useBitmapLedger, &concWindow),
                     "young concurrent mark SATB not cleared");
        // Window closes here: the next statement asks every mutator to stop. Read the pair
        // (windowNs, MarkedInWindow) together -- duration alone proves nothing.
        concWindow.markedAtExit = reachableVec.size();
        if (concWindowStartNs != 0) {
            concWindow.windowNs = TimeUtil::NanoSeconds() - concWindowStartNs;
        }
        // STW2: freeze world for post-mark verify + evacuate (still STW today).
        // youngmiss2 §1①: sync CLEAR_SATB so every mutator HandleGCPhase flushes its current
        // satbNode into retiredNodes (GetRetiredObjects only pops retired — in-flight node is
        // otherwise invisible). Same shape as MarkSatbBuffer timeout STW.
        // portyoungconc L1 (ZGC zGeneration.cpp:550-555):
        //     while (!pause_mark_end()) { concurrent_mark_continue(); }
        // pause_mark_end() is a safepoint that answers "did marking terminate"; false means
        // leave the safepoint, run mark_follow() concurrently, and come back. Our STW2
        // fixpoint is that safepoint body -- but today it logs NON_CONVERGED and evacuates
        // anyway, i.e. it has no false branch. This loop supplies it.
        bool markEndDone = false;
        while (!markEndDone) {
        stw = std::make_unique<ScopedStopTheWorld>("young post-mark", true,
                                                   GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
        // youngmiss / youngconc M1: the write barrier marks concurrent-window targets before it
        // records their slots. Keep those slots on current, as ZGC does, instead of consuming the
        // same edge into this collection's marking work a second time at mark end.
        //
        // ZGC shape (zGeneration.cpp:542-558 mark_end re-enter): under STW2 mutators are frozen,
        // so roots+SATB+field-rescan loop to a quiet fixpoint; there is no current-face drain.
        // The all-STW relocation rollback is the exception because it has no later active-face
        // Snapshot for same-cycle reference fixing.
        //
        // (1) flush current; defer it, or drain only for the all-STW rollback
        // (2) fixpoint: roots + retired SATB + reachableVec field rescan (young→young)
        // (3) quiet = no new greys this iter (work empty, no field extra, reachableVec stable)
        {
            MRT_PHASE_TIMER("young.stw2_fixpoint");
            // STW2 slimming: the STW1 snapshot has already had every reachable field
            // scanned by TraceYoungClosure.  Keep a cursor at that sealed prefix and
            // only revisit holders added after the concurrent window.  Mutator y2y
            // writes are covered by the existing per-AllocBuffer dirty-holder face;
            // merging it under STW2 is the incremental replacement for rescanning the
            // whole reachableVec on every fixpoint iteration.
            size_t fieldScanCursor = reachableVec.size();
            size_t totalConcRemset = 0;
            size_t deferredCurrentRemset = 0;
            {
                MinorSlotSet concurrentRemset;
                RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
                StoreBarrierBuffer::FlushAll(rememberedSet);
                if (MinorYoungFlipOff()) {
                    // The all-STW rollback has no post-relocate active-face Snapshot(), so it
                    // still needs current slots in this collection's consumedSlots/ref-fix set.
                    totalConcRemset = rememberedSet.DrainForMinor(concurrentRemset);
                    // Observe-only: classify current-face targets against water/mark/SATB/alloc-black
                    // BEFORE MergeYoungAllocBlack / GetRetiredObjects consume those ledgers.
                    // Does not push workStack (zGeneration.cpp:897-916 pause_mark_end has no drain).
                    Stw2CurrentAudit::Census(concurrentRemset, &theAllocator);
                    if (totalConcRemset != 0) {
                        rememberedSlots.insert(concurrentRemset.begin(), concurrentRemset.end());
                        remsetStats.recorded = rememberedSlots.size();
                        // Do NOT pass product fullYoungScan: that path drops slots missing from
                        // reachableSlots. Concurrent edges are the authority for new greys.
                        RescanRememberedSet(workStack, concurrentRemset, reachableSlots, weakSlots,
                                            currentMinorRoots,
                                            /*fullYoungScan=*/false, &consumedSlots, &remsetStats,
                                            &remsetInteriorBases, stw.get());
                        for (MAddress slot : concurrentRemset) {
                            if (!Heap::IsHeapAddress(slot)) {
                                continue;
                            }
                            (void)LedgerInsert(reachableSlots, slot);
                        }
                        if (!workStack.empty()) {
                            TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec,
                                              reachableSlots, weakSlots, useBitmapLedger);
                        }
                    }
                } else {
                    // Keep current for the next minor, but scan it now as ZGC does
                    // for an entry that crossed the young-mark flip
                    // (zStoreBarrierBuffer.cpp:170-187). Snapshot is deliberately
                    // non-destructive: this cycle scans it and the next flip retains it.
                    concurrentRemset = rememberedSet.Snapshot();
                    deferredCurrentRemset = concurrentRemset.size();
                    Stw2CurrentAudit::Census(concurrentRemset, &theAllocator);
                    if (!concurrentRemset.empty()) {
                        rememberedSlots.insert(concurrentRemset.begin(), concurrentRemset.end());
                        remsetStats.recorded = rememberedSlots.size();
                        RescanRememberedSet(workStack, concurrentRemset, reachableSlots, weakSlots,
                                            currentMinorRoots,
                                            /*fullYoungScan=*/false, &consumedSlots, &remsetStats,
                                            &remsetInteriorBases, stw.get());
                        for (MAddress slot : concurrentRemset) {
                            if (Heap::IsHeapAddress(slot)) {
                                (void)LedgerInsert(reachableSlots, slot);
                            }
                        }
                        if (!workStack.empty()) {
                            TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec,
                                              reachableSlots, weakSlots, useBitmapLedger);
                        }
                    }
                }
            }
            // youngconc Ⅱ: TRACE-window allocate-black greys (painted at alloc). Claim skip in
            // TraceYoungClosure would drop reachableVec/fields — force ledger + child greys.
            // Skip incomplete headers (STW mid-construct); paint still covers GetRoute face.
            size_t allocBlackN = 0;
            {
                WorkStack allocBlack = NewWorkStack();
                theAllocator.VisitAllocBuffers(
                    [&allocBlack](AllocBuffer& buffer) { buffer.MergeYoungAllocBlack(allocBlack); });
                allocBlackN = allocBlack.size();
                while (!allocBlack.empty()) {
                    BaseObject* object = allocBlack.back().object();
                    allocBlack.pop_back();
                    if (object == nullptr || !Heap::IsHeapAddress(object)) {
                        continue;
                    }
                    if (!Collector::PlausibleManagedObjectGate("youngconc.alloc_black", object)) {
                        continue;
                    }
                    if (!object->IsValidObject()) {
                        continue;
                    }
                    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
                    if (region == nullptr || !region->IsYoungRegion()) {
                        continue;
                    }
                    (void)MarkObject(object);
                    reachableVec.push_back(object);
                    if (!object->HasRefField() || object->IsWeakRef()) {
                        continue;
                    }
                    if (useBitmapLedger && fullYoungScan) {
                        object->ForEachRefField([this, &reachableSlots](RefField<>& field) {
                            MAddress slot = reinterpret_cast<MAddress>(&field);
                            if (Heap::IsHeapAddress(slot)) {
                                (void)LedgerInsert(reachableSlots, slot);
                            }
                        });
                    }
                    object->ForEachRefField([this, &workStack, object](RefField<>& field) {
                        BaseObject* target = ResolveMinorReference(field);
                        if (target == nullptr || !Heap::IsHeapAddress(target)) {
                            return;
                        }
                        if (fullYoungScan) {
                            PushAdmittedYoung(target, workStack, "young_alloc_black.fys", &field, object);
                        } else {
                            PushYoungObject(target, workStack, "young_alloc_black");
                        }
                    });
                }
                if (!workStack.empty()) {
                    TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                      weakSlots, useBitmapLedger);
                }
            }
            constexpr size_t kMaxStw2Iters = 16;
            size_t totalFieldExtra = 0;
            size_t iters = 0;
            bool converged = false;
            for (; iters < kMaxStw2Iters; ++iters) {
                const size_t reachableBefore = reachableVec.size();
                size_t rootExtraN = 0;
                size_t satbExtraN = 0;
                {
                    WorkStack finalRoots = NewWorkStack();
                    theAllocator.VisitAllocBuffers(
                        [&finalRoots](AllocBuffer& buffer) { buffer.MergeRoots(finalRoots); });
                    SatbBuffer::Instance().GetRetiredObjects(finalRoots);
                    while (!finalRoots.empty()) {
                        const MarkStackEntry entry = finalRoots.back();
                        BaseObject* object = entry.object();
                        finalRoots.pop_back();
                        if (Heap::IsHeapAddress(object)) {
                            allocationRoots.insert(object);
                        }
                        if (!entry.follow()) {
                            if (Heap::IsHeapAddress(object)) {
                                workStack.push_back(MarkStackEntry::MarkOnly(object));
                                ++rootExtraN;
                            }
                        } else if (fullYoungScan) {
                            size_t before = workStack.size();
                            PushAdmittedYoung(object, workStack, "alloc_buffer_final.fys");
                            if (workStack.size() > before) {
                                ++rootExtraN;
                            }
                        } else {
                            size_t before = workStack.size();
                            PushYoungObject(object, workStack, "alloc_buffer_final");
                            if (workStack.size() > before) {
                                ++rootExtraN;
                            }
                        }
                    }
                    VisitMinorRoots([this, &workStack, &rootExtraN, &currentMinorRoots](BaseObject* object) {
                        if (Heap::IsHeapAddress(object)) {
                            RegionInfo* region =
                                RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
                            if (region != nullptr && !region->IsYoungRegion()) {
                                currentMinorRoots.insert(object);
                            }
                        }
                        if (fullYoungScan) {
                            size_t before = workStack.size();
                            PushAdmittedYoung(object, workStack, "minor_root_final.fys");
                            if (workStack.size() > before) {
                                ++rootExtraN;
                            }
                        } else {
                            size_t before = workStack.size();
                            PushYoungObject(object, workStack, "minor_root_final");
                            if (workStack.size() > before) {
                                ++rootExtraN;
                            }
                        }
                    }, [&workStack, &rootExtraN, &currentMinorRoots](BaseObject* object) {
                        if (!Heap::IsHeapAddress(object)) {
                            return;
                        }
                        RegionInfo* region =
                            RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
                        if (region != nullptr && !region->IsYoungRegion()) {
                            currentMinorRoots.insert(object);
                        }
                        workStack.push_back(MarkStackEntry::MarkOnly(object));
                        ++rootExtraN;
                    });
                    if (!workStack.empty()) {
                        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                          weakSlots, useBitmapLedger);
                    }
                }
                {
                    WorkStack finalSatb = NewWorkStack();
                    SatbBuffer::Instance().GetRetiredObjects(finalSatb);
                    while (!finalSatb.empty()) {
                        BaseObject* obj = finalSatb.back().object();
                        finalSatb.pop_back();
                        if (!Heap::IsHeapAddress(obj)) {
                            continue;
                        }
                        if (fullYoungScan) {
                            size_t before = workStack.size();
                            PushAdmittedYoung(obj, workStack, "young_satb_final.fys");
                            if (workStack.size() > before) {
                                ++satbExtraN;
                            }
                        } else {
                            size_t before = workStack.size();
                            PushYoungObject(obj, workStack, "young_satb_final");
                            if (workStack.size() > before) {
                                ++satbExtraN;
                            }
                        }
                    }
                    if (!workStack.empty()) {
                        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                          weakSlots, useBitmapLedger);
                    }
                }
                // youngmiss2 §1③: old→young remset does not contain concurrent y2y
                // stores.  The write barrier already records their holder objects in
                // y2yDirtyHolders.  Scan those holders plus the reachableVec suffix
                // added after STW1, rather than the sealed STW1 prefix.
                size_t fieldExtraN = 0;
                {
                    WorkStack fieldHolders = NewWorkStack();
                    theAllocator.VisitAllocBuffers(
                        [&fieldHolders](AllocBuffer& buffer) { buffer.MergeY2yDirtyHolders(fieldHolders); });
                    const size_t incrementalEnd = reachableVec.size();
                    for (size_t i = fieldScanCursor; i < incrementalEnd; ++i) {
                        fieldHolders.push_back(reachableVec[i]);
                    }
                    fieldScanCursor = incrementalEnd;

                    WorkStack fieldExtra = NewWorkStack();
                    while (!fieldHolders.empty()) {
                        BaseObject* object = fieldHolders.back().object();
                        fieldHolders.pop_back();
                        if (object == nullptr || !Heap::IsHeapAddress(object)) {
                            continue;
                        }
                        if (!Collector::PlausibleManagedObjectGate("youngconc.field_rescan.holder", object)) {
                            continue;
                        }
                        if (!object->HasRefField()) {
                            continue;
                        }
                        object->ForEachRefField([this, &fieldExtra, &reachableSlots,
                                                object](RefField<>& field) {
                            MAddress slot = reinterpret_cast<MAddress>(&field);
                            if (fullYoungScan && Heap::IsHeapAddress(slot)) {
                                (void)LedgerInsert(reachableSlots, slot);
                            }
                            BaseObject* target = ResolveMinorReference(field);
                            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                                return;
                            }
                            if (!Collector::PlausibleManagedObjectGate("youngconc.field_rescan.target", target)) {
                                BaseObject* host = Collector::TryRecoverInteriorBase(target);
                                if (host != nullptr && host != target) {
                                    target = host;
                                } else {
                                    return;
                                }
                            }
                            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                            if (region == nullptr || !region->IsYoungRegion()) {
                                return;
                            }
                            if (region->IsMarkedObject(
                                    region->GetMarkView<Generation::Young>(), target)) {
                                return;
                            }

                            PushAdmittedYoung(target, fieldExtra, "youngconc.field_rescan", &field, object);
                        });
                    }
                    fieldExtraN = fieldExtra.size();
                    totalFieldExtra += fieldExtraN;
                    if (!fieldExtra.empty()) {
                        TraceYoungClosure(fieldExtra, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                          weakSlots, useBitmapLedger);
                    }
                }
                // Quiet: no new grey work this iteration. rootExtraN may re-push already-marked
                // roots under FYS (PushYoungObject skips marked); only field/satb/vec growth count.
                if (satbExtraN == 0 && fieldExtraN == 0 && workStack.empty() &&
                    reachableVec.size() == reachableBefore) {
                    converged = true;
                    break;
                }
                (void)rootExtraN;
            }
            VLOG(REPORT,
                 "[GCV2][youngconc] stw2_fixpoint iters=%zu conc_remset_total=%zu "
                 "deferred_current=%zu field_extra_total=%zu alloc_black=%zu reachable=%zu converged=%d",
                 iters + 1, totalConcRemset, deferredCurrentRemset, totalFieldExtra, allocBlackN,
                 reachableVec.size(), static_cast<int>(converged));
            // ZGC mark_end(): true = terminated, fall through to relocate; false = go back to
            // concurrent mark. FORCE_REENTER makes the first n ends answer false, so the edge
            // has a positive control on a workload that converges in one pass every time --
            // an untaken branch cannot be shown to work.
            const bool forcedNotDone = concWindow.reenters < youngMarkEndForceReenter;
            const bool needsReenter = !converged || forcedNotDone;
            const bool wantReenter = needsReenter;
            markEndDone = !wantReenter;
            if (!converged) {
                VLOG(REPORT,
                     "[GCV2][youngconc] stw2_fixpoint NON_CONVERGED max_iters=%zu reachable=%zu "
                     "conc_remset_total=%zu field_extra_total=%zu alloc_black=%zu reenter=%d",
                     kMaxStw2Iters, reachableVec.size(), totalConcRemset, totalFieldExtra, allocBlackN,
                     static_cast<int>(wantReenter));
            }
            if (wantReenter) {
                VLOG(REPORT,
                     "[GCV2][youngconc] mark_end=false -> concurrent_mark_continue reenter=%zu forced=%d "
                     "converged=%d reachable=%zu",
                     concWindow.reenters + 1, static_cast<int>(forcedNotDone), static_cast<int>(converged),
                     reachableVec.size());
            }
        }
        if (!markEndDone) {
            // concurrent_mark_continue() == mark_follow() only (zGeneration.cpp:689-692).
            // Deliberately no PrepareTrace here: that is a mark-start action (it clears
            // notRelocatableThisCycle), and re-running it would drop stamps the window set.
            ++concWindow.reenters;
            TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
            stw.reset();
            const uint64_t reenterStartNs = TimeUtil::NanoSeconds();
            CHECK_DETAIL(MarkYoungSatbBuffer(workStack, fullYoungScan, reachableObjects, reachableVec,
                                             reachableSlots, weakSlots, useBitmapLedger, &concWindow),
                         "young concurrent mark continue SATB not cleared");
            concWindow.windowNs += TimeUtil::NanoSeconds() - reenterStartNs;
            concWindow.markedAtExit = reachableVec.size();
        }
        }
        // Rebuild liveRememberedSlots after concurrent remset merge (stats/audit only;
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
        VLOG(REPORT, "[GCV2][youngconc] concurrent young mark done; STW2 post-mark+evac reachable=%zu",
             reachableVec.size());
        // youngstatic: seal static-root young targets into mark face under STW2.
        // Probe showed VisitStaticRoots enumerates (staticYoung≫0) but PushYoungObject is
        // bypassed under FYS (direct workStack). Residual unmarked static young still reach
        // FixMinor as ghost-from with live0Surv=0. Force MarkObject here (not only push).
        {
            size_t sealed = 0;
            size_t already = 0;
            size_t staticYoung = 0;
            size_t staticOld = 0;
            size_t gateSkip = 0;
            Heap::GetHeap().VisitStaticRoots([this, &sealed, &already, &staticYoung, &staticOld, &gateSkip,
                                             &workStack](RootSlot& root) {
                zaddress_unsafe observed = root.LoadPlain();
                HeapSlot<> bits(to_zpointer(raw(observed)));
                BaseObject* obj = to_object(bits.GetTargetObject());
                if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
                    return;
                }
                if (!Collector::PlausibleManagedObjectGate("youngstatic.seal", obj)) {
                    BaseObject* host = Collector::TryRecoverInteriorBase(obj);
                    if (host == nullptr || host == obj) {
                        ++gateSkip;
                        return;
                    }
                    obj = host;
                }
                if (!obj->IsValidObject()) {
                    ++gateSkip;
                    return;
                }
                RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
                if (region == nullptr) {
                    ++gateSkip;
                    return;
                }
                if (!region->IsYoungRegion()) {
                    ++staticOld;
                    return;
                }
                ++staticYoung;
                if (region->IsMarkedObject(region->GetMarkView<Generation::Young>(), obj)) {
                    ++already;
                    return;
                }
                // Direct paint (do not rely solely on workStack drain under concurrent residue).

                (void)MarkObject(obj);
                PushAdmittedYoung(MarkStackEntry::FollowOnly(obj), workStack, "youngstatic.seal");
                ++sealed;
            });
            if (!workStack.empty()) {
                TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                  weakSlots, useBitmapLedger);
            }
            LOG(RTLOG_ERROR,
                "[GCV2][youngstatic] stw2_static_seal sealed=%zu already=%zu staticYoung=%zu "
                "staticOld=%zu gateSkip=%zu reachable=%zu",
                sealed, already, staticYoung, staticOld, gateSkip, reachableVec.size());
        }
    }
    // portyoungconc positive control. Emitted on EVERY minor, including the closed arm, so
    // "no line" and "a line of zeros" are distinguishable. window_ns is the only field that
    // a merely-existing window can raise; marked_in_window / satb_objects / closure_calls
    // are GC work, and it is the work fields that decide whether the window is real.
    VLOG(REPORT,
         "[GCV2][youngconc][concwork] run=%zu conc=%d follow=%d window_ns=%llu marked_in_window=%zu "
         "satb_objects=%zu satb_iters=%zu closure_calls=%zu remset_slots=%zu reenters=%zu "
         "marked_at_entry=%zu reachable_total=%zu",
         minorTotalRuns + 1, static_cast<int>(youngConcMark), static_cast<int>(youngConcFollow),
         static_cast<unsigned long long>(concWindow.windowNs), concWindow.MarkedInWindow(),
         concWindow.satbObjects, concWindow.satbIters, concWindow.closureCalls, concWindow.remsetSlots,
         concWindow.reenters, concWindow.markedAtEntry, reachableVec.size());
    VLOG(REPORT, "[GCV2][setbitmap] use=%d reachable_n=%zu set_n=%zu fullYoung=%d youngConc=%d",
         static_cast<int>(useBitmapLedger), reachableVec.size(), reachableObjects.size(),
         static_cast<int>(fullYoungScan), static_cast<int>(youngConcMark));
    // iorfix / blackmark: product post-mark fixpoint.
    // PrepareYoung ClearLiveInfo drops pre-mark allocation-black bits; live holders in
    // reachableVec used to hold fields to unmarked young (live0Surv=0 at GetRoute). The
    // wasMarked residual walk (e533489f7) now closes that root frontier in-place. A later
    // remset/SATB closure can still expand the reachable set and expose the old residual
    // shape, so re-walk only when work arrived after the root closure. This preserves the
    // correctness fallback without rescanning a closed root-only graph every minor.
    // Orthogonal to allocate-black paint (youngconc-only after §5.2 delete of MRT_GCV2_ALLOC_BLACK);
    // IOR samples are FixMinorEvacuatedSlot×liveobj with ROUTED+surv0 (REPORT-iorsource).
    size_t fixpointTotalExtra = 0;
    size_t fixpointRoundScans = 0;
    size_t fixpointClosureRounds = 0;
    size_t fixpointHolderVisits = 0;
    size_t fixpointHolderNonHeap = 0;
    size_t fixpointHolderGateSkip = 0;
    size_t fixpointRefHolders = 0;
    size_t fixpointRefFields = 0;
    size_t fixpointTargetNull = 0;
    size_t fixpointTargetNonHeap = 0;
    size_t fixpointTargetRecovered = 0;
    size_t fixpointTargetGateSkip = 0;
    size_t fixpointTargetNotYoung = 0;
    size_t fixpointTargetAlreadyMarked = 0;
    size_t fixpointAdmitted = 0;
    size_t fixpointReachableAdded = 0;
    uint64_t fixpointScanNs = 0;
    uint64_t fixpointClosureNs = 0;
    const size_t lateReachableAdded = reachableVec.size() - reachableBeforeRemsetClosure;
    const bool fixpointTriggered = lateReachableAdded != 0;
    {
        MRT_PHASE_TIMER("young.postmark_fixpoint");
        size_t rounds = 0;
        constexpr size_t kMaxFixpointRounds = 8;
        for (; fixpointTriggered && rounds < kMaxFixpointRounds; ++rounds) {
            WorkStack blackmarkExtra = NewWorkStack();
            const size_t nHolders = reachableVec.size();
            ++fixpointRoundScans;
            const uint64_t scanStart = TimeUtil::NanoSeconds();
            for (size_t i = 0; i < nHolders; ++i) {
                ++fixpointHolderVisits;
                BaseObject* object = reachableVec[i];
                if (object == nullptr || !Heap::IsHeapAddress(object)) {
                    ++fixpointHolderNonHeap;
                    continue;
                }
                if (!Collector::PlausibleManagedObjectGate("iorfix.fixpoint.holder", object)) {
                    ++fixpointHolderGateSkip;
                    continue;
                }
                if (!object->HasRefField()) {
                    continue;
                }
                ++fixpointRefHolders;
                object->ForEachRefField([this, &blackmarkExtra, object, &fixpointRefFields,
                                         &fixpointTargetNull, &fixpointTargetNonHeap,
                                         &fixpointTargetRecovered, &fixpointTargetGateSkip,
                                         &fixpointTargetNotYoung, &fixpointTargetAlreadyMarked,
                                         &fixpointAdmitted](RefField<>& field) {
                    ++fixpointRefFields;
                    BaseObject* target = ResolveMinorReference(field);
                    if (target == nullptr || !Heap::IsHeapAddress(target)) {
                        if (target == nullptr) {
                            ++fixpointTargetNull;
                        } else {
                            ++fixpointTargetNonHeap;
                        }

                        return;
                    }
                    if (!Collector::PlausibleManagedObjectGate("iorfix.fixpoint.target", target)) {
                        BaseObject* host = Collector::TryRecoverInteriorBase(target);
                        if (host != nullptr && host != target) {
                            target = host;
                            ++fixpointTargetRecovered;
                        } else {
                            ++fixpointTargetGateSkip;

                            return;
                        }
                    }
                    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (region == nullptr || !region->IsYoungRegion()) {
                        ++fixpointTargetNotYoung;

                        return;
                    }
                    if (region->IsMarkedObject(region->GetMarkView<Generation::Young>(), target)) {
                        ++fixpointTargetAlreadyMarked;

                        return;
                    }
                    ++fixpointAdmitted;


                    blackmarkExtra.push_back(target);
                });
            }
            fixpointScanNs += TimeUtil::NanoSeconds() - scanStart;
            if (blackmarkExtra.empty()) {
                break;
            }
            size_t before = reachableVec.size();
            size_t extraN = blackmarkExtra.size();
            const uint64_t closureStart = TimeUtil::NanoSeconds();
            TraceYoungClosure(blackmarkExtra, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                              weakSlots, useBitmapLedger);
            fixpointClosureNs += TimeUtil::NanoSeconds() - closureStart;
            ++fixpointClosureRounds;
            fixpointTotalExtra += extraN;
            fixpointReachableAdded += reachableVec.size() - before;
            if (reachableVec.size() == before) {
                break;
            }
        }
    }
    VLOG(REPORT,
         "[GCV2][candfix] postmark_fixpoint trigger=%u root_reachable=%zu late_added=%zu "
         "round_scans=%zu closure_rounds=%zu holder_visits=%zu "
         "holder_nonheap=%zu holder_gate_skip=%zu ref_holders=%zu ref_fields=%zu target_null=%zu "
         "target_nonheap=%zu target_recovered=%zu target_gate_skip=%zu target_not_young=%zu "
         "target_already_marked=%zu admitted=%zu closure_inputs=%zu reachable_added=%zu "
         "reachable_after=%zu scan_ns=%llu closure_ns=%llu",
         static_cast<unsigned>(fixpointTriggered), reachableBeforeRemsetClosure, lateReachableAdded,
         fixpointRoundScans, fixpointClosureRounds, fixpointHolderVisits, fixpointHolderNonHeap,
         fixpointHolderGateSkip, fixpointRefHolders, fixpointRefFields, fixpointTargetNull,
         fixpointTargetNonHeap, fixpointTargetRecovered, fixpointTargetGateSkip, fixpointTargetNotYoung,
         fixpointTargetAlreadyMarked, fixpointAdmitted, fixpointTotalExtra, fixpointReachableAdded,
         reachableVec.size(), static_cast<unsigned long long>(fixpointScanNs),
         static_cast<unsigned long long>(fixpointClosureNs));

    // No independent full-root closure is available after deleting the empty
    // explainer. nullptr means "not measured"; an empty set must mean a closure
    // actually ran and found no holders.
    VerifyRememberedSetInvariant("pre-evacuate", rememberedSlots, false, nullptr);

    // Full-heap object invariant H (HotSpot G1HeapVerifier::verify inventory #10).
    // Independent ForEachObj walk; gated by MRT_GCV2_VERIFY_HEAP (default off).
    // Timeline (gcdirty): also force as post-mark under POST_EVAC so first-dirty bracketing
    // does not require global VERIFY_HEAP.
    if (kVerifyPostEvac) {
        VLOG(REPORT, "[GCV2][verify][post-evac] enter point=post-mark run=%zu", minorTotalRuns + 1);
        VerifyHeapObjects("post-mark", true, nullptr);
        VLOG(REPORT, "[GCV2][verify][post-evac] point=post-mark run=%zu", minorTotalRuns + 1);
    } else {
        VerifyHeapObjects("pre-evacuate", false, nullptr);
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
    VLOG(REPORT, "[GCV2][pageage] threshold=%u live0=%zu live1=%zu garbage=%zu allocated=%zu",
         gcStats.tenuringThreshold, tenuringIn.liveByAge[0], tenuringIn.liveByAge[1],
         tenuringIn.youngGarbage, tenuringIn.youngAllocated);
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
    const bool refFixSlotsCoveredByReachable = fullYoungScan && !youngConcMark;
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
        VerifyHeapObjects("post-evacuate", true);
        std::unordered_set<MAddress> remsetSnap = Heap::GetHeap().GetRememberedSet().Snapshot();
        VerifyRememberedSetInvariant("post-evacuate", remsetSnap, true);
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
