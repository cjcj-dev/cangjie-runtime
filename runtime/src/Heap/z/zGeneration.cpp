// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zBreakpoint.hpp"
#include "Heap/z/zVerify.hpp"
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
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "Mutator/Handshake.h"
#include "TypeInfoManager.h"
#include "Heap/WCollector/WCollectorInternal.h"

#include "Heap/z/z_globals.hpp"
namespace MapleRuntime {
GenerationCycle::GenerationCycle(GCCycleGeneration generation)
    : mark(std::make_unique<ZMark>(ZMarkStripesMax,
          generation == GCCycleGeneration::YOUNG ? MarkingStacks::MarkingGeneration::YOUNG
                                                 : MarkingStacks::MarkingGeneration::MAJOR)),
      generation(generation),
      _relocation_set(this)
{}

GenerationCycle::~GenerationCycle() = default;

// ZGC zGeneration.cpp:197-207: select policy at the generation boundary.
static double fragmentation_limit(GCCycleGeneration generation)
{
    if (generation == GCCycleGeneration::OLD) {
        return ZFragmentationLimit;
    } else {
        return ZYoungCompactionLimit;
    }
}
double GenerationCycle::FragmentationLimit() const
{
    return fragmentation_limit(generation);
}

void ResetSkippedStackMapCounts();
void ReportSkippedStackMapCounts();
// ZGenerationYoung::mark_start (zGeneration.cpp:855-880). The collector
// supplies the existing allocator/mark domain; this cycle owns phase and seq.
YoungCollectionStats GenerationCycle::StartYoungMark(WCollector& collector)
{
    CHECK(generation == GCCycleGeneration::YOUNG);
    CHECK(Snapshot().active);
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::Begin, mark.get());
    }
#endif
    ZGlobalsPointers::flip_young_mark_start();
    ZVerify::OnColorFlip();
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::BeforeRetire, mark.get());
    }
#endif

    auto& space = static_cast<RegionSpace&>(collector.GetAllocator());
    auto& manager = space.GetRegionManager();
    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungFlushAlloc);
        manager.ResetTLABUsage();
        manager.RetireSharedPages(kPageAgeRangeYoung);
        collector.FlushAllocationRegions();
    }
    // Cangjie keeps allocation lists and candidate statistics in RegionManager.
    // Preparing those lists retires the young shared/pinned allocation pages.
    collector.minorCandidateRegions.clear();
    YoungCollectionStats stats;
    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungPrepareCandidates);
        stats = manager.PrepareYoungGarbageCandidates(
            [&collector](ZPage* region) { collector.minorCandidateRegions.insert(region); });
    }
    // Flush pre-flip producers before invalidating their generation sequence.
    (void)ZMark::FlushAllGenerations();
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::BeforeSequence, mark.get());
    }
#endif
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(sequence != UINT64_MAX);
        ++sequence;
    }
    PublishPhase(GC_PHASE_ENUM);
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::BeforeDomain, mark.get());
    }
#endif
    collector.StartYoungMarkWork();
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::BeforeRemembered, mark.get());
    }
#endif
    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungRemsetDrain);
        Heap::GetHeap().GetRememberedSet().FlipForMinor();
    }
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::Complete, mark.get());
    }
#endif
    return stats;
}

// ZGenerationOld::mark_start (zGeneration.cpp:1212-1237).
void GenerationCycle::StartOldMark(WCollector& collector)
{
    CHECK(generation == GCCycleGeneration::OLD);
    CHECK(Snapshot().active);
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::Begin, mark.get());
    }
#endif
    ZGlobalsPointers::flip_old_mark_start();
    ZVerify::OnColorFlip();
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::BeforeRetire, mark.get());
    }
#endif
    auto& space = static_cast<RegionSpace&>(collector.GetAllocator());
    // ZGC holds the VM mark-start pause across retirement and seqnum advance
    // (zGeneration.cpp:1213-1231). Serialize the pinned publication adapter
    // explicitly because our handshake pause permits safe native threads.
    std::unique_lock<std::mutex> pinnedLock(space.GetRegionManager().PinnedAllocationMutex());
    space.GetRegionManager().RetireSharedPages(kPageAgeRangeOld);
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::BeforeSequence, mark.get());
    }
#endif
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(sequence != UINT64_MAX);
        ++sequence;
    }
    pinnedLock.unlock();
    PublishPhase(GC_PHASE_ENUM);
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::BeforeDomain, mark.get());
    }
#endif
    collector.StartOldMarkWork();
#if defined(MRT_TESTABLE_INTERNALS)
    if (CopyCollector::testMarkStartState) {
        CopyCollector::testMarkStartState(generation, MarkStartPoint::Complete, mark.get());
    }
#endif
}

// ZGenerationOld::relocate_start (zGeneration.cpp:1379-1397) captures the
// young sequence once for the whole old relocation, not once per forwarding.
void GenerationCycle::RecordYoungSequenceAtRelocateStart(uint64_t youngSequence)
{
    CHECK(generation == GCCycleGeneration::OLD);
    youngSequenceAtRelocateStart.store(youngSequence, std::memory_order_release);
}

bool GenerationCycle::ActiveRemsetIsCurrent(uint64_t youngSequence) const
{
    CHECK(generation == GCCycleGeneration::OLD);
    // zGeneration.inline.hpp:174-182: each young mark start flips the faces.
    return ((youngSequence - youngSequenceAtRelocateStart.load(std::memory_order_acquire)) & 1U) == 0;
}

void Collector::PublishGenerationPhase(GCCycleGeneration generation, GCPhase value)
{
    GenerationCycle& cycle = generation == GCCycleGeneration::YOUNG ? youngCycle : oldCycle;
    const GCPhase before = cycle.Phase();
    if (generation == GCCycleGeneration::OLD &&
        (value == GCPhase::GC_PHASE_PREFORWARD || value == GCPhase::GC_PHASE_FORWARD) &&
        before != GCPhase::GC_PHASE_PREFORWARD && before != GCPhase::GC_PHASE_FORWARD) {
        oldCycle.RecordYoungSequenceAtRelocateStart(youngCycle.Sequence());
    }
    cycle.PublishPhase(value);
}


// ZGeneration::mark_object, zGeneration.inline.hpp:119-123.
void WCollector::MarkYoungRootObject(BaseObject* object) const
{
    // #596's barrier already established current and selected young. Keep the
    // generation mark-phase assertion at ZGeneration::mark_object's entry.
    auto& cycle = const_cast<GenerationCycle&>(GetGenerationCycle(GCCycleGeneration::YOUNG));
    cycle.MarkObjectIfActive<false, true, true, false>(from_object(object));
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
    ZVerify::BeforeZOperation();
    // Full-colour gate: reject any plain HeapSlot before young mark mutates colours.
    // VM_ZMarkStartYoungAndOld / VM_ZMarkStartYoung (zGeneration.cpp:583-659).
    // A major starts old exactly once in this young pause. An independent
    // minor leaves the old cycle identity and mark color untouched.
    collectorResources.NoteYoungMarkStart(youngCycle.YoungType());
    // VM_ZMarkStartYoungAndOld starts the complete young event before old
    // (zGeneration.cpp:601-602); a minor only enters the young event.
    YoungCollectionStats stats = youngCycle.StartYoungMark(*this);
    if (youngCycle.IsMajorRoots()) {
        oldCycle.Begin(oldCycle.Snapshot().requestIndex);
        oldCycle.StartOldMark(*this);
    }
    RegionSpace& space = static_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    MinorSlotSet rememberedSlots;
#if defined(MRT_TESTABLE_INTERNALS)
    if (testYoungMarkStarted) {
        testYoungMarkStarted();
    }
#endif

    VLOG(REPORT,
         "[GCV2][candfix] prepare_candidates candidate_regions=%zu candidate_bytes=%zu "
         "from_visited=%zu from_units=%zu unmovable_visited=%zu unmovable_units=%zu "
         "unmovable_young=%zu recent_visited=%zu recent_units=%zu "
         "recent_young=%zu "
         "objects_visited=%zu slots_visited=%zu repark_ns=%llu unmovable_ns=%llu recent_ns=%llu "
         "visitor_ns=%llu list_move_ns=%llu",
         stats.candidateRegions, stats.candidateBytes, stats.fromVisited, stats.fromVisitedUnits,
         stats.unmovableVisited, stats.unmovableVisitedUnits, stats.unmovableYoung,
         stats.recentFullVisited, stats.recentFullVisitedUnits, stats.recentFullYoung,
         stats.objectVisits, stats.slotVisits,
         static_cast<unsigned long long>(stats.reparkNs), static_cast<unsigned long long>(stats.unmovableNs),
         static_cast<unsigned long long>(stats.recentFullNs),
         static_cast<unsigned long long>(stats.visitorNs),
         static_cast<unsigned long long>(stats.listMoveNs));
    // Even an empty candidate set completes remembered scanning and clearing.
    // Otherwise the mark-start flip would leave previous unconsumed when the
    // next young collection reuses that bitmap (zRemembered.cpp:561-576).

    // Pinned holders (Future/Mutex/Monitor): AllocPinned never sets young; IDLE write
    // fast-path (phase < ENUM) is a bare store — old→young edges never hit remset.
    // Stamp them before Acquire so pre-evacuate verify and young mark both see them.
    // idleedge: census remset-miss old→young BEFORE pinned stamp fills those gaps.

    // fysaudit: full non-young O→Y vs mutator remset (D1/D2/D3). Observe only.




    // promodomain: reset last cycle's flip-promoted table (CHECK registered==discharged).
    // Corresponds to ZGC reset_relocation_set before the new young collection.
    // flippromo: open broad-vs-product window for regions demoted last minor.

    uint64_t stackScanEpoch = 0;
    {
        // Publish S1/S3/S5 while every mutator is stopped. SetGCPhase is the
        // release publication point before stack-watermark processing.
        Heap::GetHeap().SetGCPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_ENUM);
        stw.reset();


        stackScanEpoch = StackWatermark::epoch_id();
        stw = std::make_unique<ScopedStopTheWorld>("young collection", false);
        ZVerify::BeforeZOperation();
        TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true, true);
        MutatorManager::Instance().VisitAllMutators([stackScanEpoch](Mutator& mutator) {
            if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                (void)mutator.GcPhaseEnum(GCPhase::GC_PHASE_ENUM, true, stackScanEpoch, false);
            }
        });

    }

    constexpr bool fullYoungScan = false;
    WorkStack workStack = NewWorkStack();
    MarkingStacks::VerifyEmpty(workStack.size());
    std::vector<BaseObject*> reachableVec;
    reachableVec.reserve(1 << 17); // ~128k; real_load ~155k reachable
    MinorObjectSet allocationRoots;
    MinorObjectSet currentMinorRoots;
    MinorSlotSet reachableSlots;
    MinorSlotSet weakSlots;
    if (fullYoungScan) {
        reachableSlots.reserve(rememberedSlots.size());
    }
    auto mergeY2yDirtyWork = [&](WorkStack& destination) {
        theAllocator.VisitAllocBuffers([this, &destination](AllocBuffer& buffer) {
            buffer.MergeY2yDirtyHolders(destination);
            buffer.MergeY2yDirtySlots([this, &destination](MAddress slot) {
                RefField<>& field = HeapSlotAt<>(slot);
                BaseObject* target = ResolveMinorReference(field);
                PushYoungObject(target, destination, "y2y_slot");
            });
        });
    };
#if defined(MRT_TESTABLE_INTERNALS)
    auto pendingY2yDirtyWorkCount = [&]() {
        size_t pending = 0;
        theAllocator.VisitAllocBuffers([&pending](AllocBuffer& buffer) {
            pending += buffer.Y2yDirtyHolderCount() + buffer.Y2yDirtySlotCount();
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
        MRT_PHASE_TIMER(ZStatPhases::PYoungRootEnum);
        (void)youngCycle.Mark().Flush();
        VisitMinorRoots([this, &workStack, &currentMinorRoots](BaseObject* object) {
            if (Heap::IsHeapAddress(object)) {
                ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
                if (region != nullptr && !region->IsYoungRegion()) {
                    currentMinorRoots.insert(object);
                }
            }
            PushYoungObject(object, workStack, "minor_root");
        }, [this, &workStack, &currentMinorRoots](BaseObject* object) {
            if (!Heap::IsHeapAddress(object)) {
                return;
            }
            ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
            if (region != nullptr && !region->IsYoungRegion()) {
                currentMinorRoots.insert(object);
            }
            PushYoungObject(object, workStack, "minor_root");
        }, stackScanEpoch);
        // ZMarkYoungRootsTask::work publishes its own root stacks before follow.
        (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), youngCycle.Mark());
#if defined(MRT_TESTABLE_INTERNALS)
        NoteY2yAfterRootTestReceipt(youngCycle.Mark().Stacks().Population());
#endif
    };
    // ZGC zGeneration.cpp:665-692: roots and follow are the single concurrent
    // young-mark path.  Mark-end convergence is owned by FollowYoungMark's
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
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true, true);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        // wave8 y2y handoff (8d4253522 content): consume the pre-window batch
        // before reset releases mutators. New stores after reset remain owned
        // by the STW2 consumer and cannot race this allocator-buffer merge.
        mergeY2yDirtyWork(workStack);
#if defined(MRT_TESTABLE_INTERNALS)
        NoteY2yBeforeReleaseTestReceipt(pendingY2yDirtyWorkCount());
#endif
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
        // The release above makes this ZGC mark_roots()+mark_follow work concurrent.
        MRT_PHASE_TIMER(ZStatPhases::PYoungMarkClosure);
        ++concWindow.closureCalls;
        TraceYoungClosure(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots,
                          reachableSlotDomain);
    }
    if (collectorResources.GetYoungDriverPort().Abort().Poll()) {
        return;
    }
    const bool remsetConsumedLedgerElideActive = false;

    if (rememberedSlots.empty()) {
        // scan_and_follow (zRemembered.cpp:561-576): previous face as grey
        // roots, mutators alive. Flip already happened under STW1.
        ScanRelocatedRememberedFields(rememberedSlots);
        MinorSlotSet pageSlots;
        concWindow.remsetSlots =
            Heap::GetHeap().GetRememberedSet().ScanPreviousForMinor(pageSlots);
        for (MAddress slot : pageSlots) {
            rememberedSlots.insert(slot);
        }
    }

    MinorSlotSet liveRememberedSlots;
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
    MinorInteriorBaseMap remsetInteriorBases;
    {
        // minortime: ④ remset rescan + ⑤ mark closure pass-2 (from remset edges)
        MRT_PHASE_TIMER(ZStatPhases::PYoungRemsetRescan);
        RescanRememberedSet(workStack, rememberedSlots, reachableSlots, weakSlots, currentMinorRoots,
                            fullYoungScan,
                            remsetConsumedLedgerElideActive ? nullptr : &consumedSlots, &remsetStats,
                            &remsetInteriorBases, stw.get());
    }

    // fysaudit: D2 retained-drop + D4 live-not-consumed (product path already FYS=0 under audit).

    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungMarkFromRemset);
        ++concWindow.closureCalls;
        concWindow.remsetSlots = remsetStats.consumed;
        TraceYoungClosure(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots,
                          reachableSlotDomain);
    }
#if defined(MRT_TESTABLE_INTERNALS)
    // Deterministic T1->T2 export-root window: root enumeration has returned,
    // and the concurrent mark-follow consumer has not started yet.
    PublishExportRootAfterT1TestReceipt();
#endif
    if (collectorResources.GetYoungDriverPort().Abort().Poll()) {
        return;
    }
    for (;;) {
        // Concurrent mark-follow drains work published by the previous pause.
        // Its worker completion is coordinated by YoungMarkTerminate (the
        // ZMarkTerminate worker-count/wakeup state machine), not pool polling.
        const bool workersTerminated =
            FollowYoungMark(workStack, fullYoungScan, reachableVec, reachableSlots,
                                weakSlots, &concWindow);
        if (!workersTerminated) {
            return;
        }
#if defined(MRT_TESTABLE_INTERNALS)
        FlushExportRootAfterT1TestReceipt();
        // Adversarial mutator publication point: workers have terminated, but
        // the pause has not started. The pause must flush once and return
        // failure; it must not consume closure in an in-pause loop.
        PublishMarkBeforeMarkEndTestReceipt();
        PublishLeftoverBeforePauseTestReceipt();
        // Publish after concurrent consumers have terminated. Publishing just
        // after mark-start release lets FollowYoungMark consume this work
        // before the pause, so it cannot exercise mark-end failure/continue.
        // ZGC zGeneration.cpp:897-904: only incomplete mark-end continues.
        PublishY2yAfterReleaseTestReceipt();
#endif

        // ZGenerationYoung::pause_mark_end() does one flush. Work found here is
        // not processed in the pause: releasing this owner and restoring TRACE
        // is the existing concurrent_mark_continue edge.
#if defined(MRT_TESTABLE_INTERNALS)
        const uint64_t markEndPauseStartNs = TimeUtil::NanoSeconds();
#endif
        stw = std::make_unique<ScopedStopTheWorld>("young mark terminate", true,
                                                   GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
        ZVerify::BeforeZOperation();
#if defined(MRT_TESTABLE_INTERNALS)
        const size_t y2yBatchAtMarkEnd = pendingY2yDirtyWorkCount();
#endif
        theAllocator.VisitAllocBuffers([](AllocBuffer& buffer) {
#if defined(MRT_TESTABLE_INTERNALS)
            NoteMarkTerminatePauseProducers(buffer.Y2yDirtyHolderCount() + buffer.Y2yDirtySlotCount());
#else
            (void)buffer;
#endif
        });
        mergeY2yDirtyWork(workStack);
#if defined(MRT_TESTABLE_INTERNALS)
        NoteY2yAfterStw2TestReceipt(y2yBatchAtMarkEnd);
#endif
        const bool markEndSucceeded = TryEndYoungMark(workStack, &concWindow);
#if defined(MRT_TESTABLE_INTERNALS)
        NoteMarkTerminatePauseDuration(TimeUtil::NanoSeconds() - markEndPauseStartNs);
#endif
        if (workersTerminated && markEndSucceeded) {
            MarkingStacks::VerifyEmpty(workStack.size());
#if defined(MRT_TESTABLE_INTERNALS)
            NoteExportRootPublicationAtT2TestReceipt();
            if (testYoungMarkCompleted) {
                testYoungMarkCompleted();
            }
#endif
            break;
        }
        NoteMarkTerminateContinue(workStack.size());
        ++concWindow.reenters;
        stw.reset();
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true, true);
    }
    ReportMarkTerminateContinue();
    if (collectorResources.GetYoungDriverPort().Abort().Poll()) {
        return;
    }
    {
        // Window closes here: the next statement asks every mutator to stop. Read the pair
        // (windowNs, MarkedInWindow) together -- duration alone proves nothing.
        concWindow.markedAtExit = reachableVec.size();
        if (concWindowStartNs != 0) {
            concWindow.windowNs = TimeUtil::NanoSeconds() - concWindowStartNs;
        }
        // The successful mark-end owner is retained for evacuation handoff.
        // Every allocator/y2y batch was either empty at this pause or forced a
        // failed mark-end and was processed by concurrent_mark_continue.
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
        VLOG(REPORT, "[GCV2][youngconc] concurrent young mark done; STW2 evacuation handoff reachable=%zu",
             reachableVec.size());
    }
    // portyoungconc positive control. Emitted on EVERY minor, including the closed arm, so
    // "no line" and "a line of zeros" are distinguishable. window_ns is the only field that
    // a merely-existing window can raise; marked_in_window / closure_calls
    // are GC work, and it is the work fields that decide whether the window is real.
    VLOG(REPORT,
         "[GCV2][youngconc][concwork] run=%zu conc=%d follow=%d window_ns=%llu marked_in_window=%zu "
         "closure_calls=%zu remset_slots=%zu reenters=%zu "
         "marked_at_entry=%zu reachable_total=%zu",
         minorTotalRuns + 1, 1, 1,
         static_cast<unsigned long long>(concWindow.windowNs), concWindow.MarkedInWindow(),
         concWindow.closureCalls, concWindow.remsetSlots,
         concWindow.reenters, concWindow.markedAtEntry, reachableVec.size());
    size_t liveBytes = 0;
    TenuringInputs tenuringIn;
    tenuringIn.softMaxCapacity = Heap::GetHeap().GetMaxCapacity();
    tenuringIn.youngAllocated = stats.candidateBytes;
    for (ZPage* region : minorCandidateRegions) {
        const size_t live = region->is_marked() ? region->live_bytes() : 0;
        liveBytes += live;
        uint32_t age = region->GetYoungAge();
        if (age >= kPageAgeCount) {
            age = untype(PageAge::survivor14);
        }
        tenuringIn.liveByAge[age] += live;
    }
    tenuringIn.youngGarbage = stats.candidateBytes > liveBytes ? (stats.candidateBytes - liveBytes) : 0;
    GCStats& gcStats = GetGCStats(GCCycleGeneration::YOUNG);
    gcStats.youngCandidateBytes = stats.candidateBytes;
    gcStats.youngPromotedBytes = liveBytes;
    for (uint32_t i = 0; i < kPageAgeCount; ++i) {
        gcStats.liveByAge[i] = tenuringIn.liveByAge[i];
    }
    youngCycle.SelectTenuringThreshold(tenuringIn);
    {
        // minortime: ⑧ pre-evac finish (phase + weak/satb clear)
        MRT_PHASE_TIMER(ZStatPhases::PYoungPreEvacClear);
        TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true, true);
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
        // zGeneration.cpp:563 / :699-701: after this young mark_end, reset
        // the previous young relocation set. Independent of old remap.
        StringDedup::Instance().Clean([this](BaseObject* object) {
            ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
            return !region->IsYoungRegion() || IsMarkedObject<Generation::Young>(object);
        });
        GetGenerationCycle(GCCycleGeneration::YOUNG).reset_relocation_set();
        space.GetRegionManager().ResetFlipPromotedPages();
    }

    if (collectorResources.GetYoungDriverPort().Abort().Poll()) {
        return;
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
    EvacuateYoungRegions(reachableVec, consumedSlots, refFixSlotsCoveredByReachable,
                         remsetInteriorBases, &stw);
    if (collectorResources.GetYoungDriverPort().Abort().Poll()) {
        return;
    }
    size_t allocatedAfter = space.AllocatedBytes();
    stats.reclaimedBytes = allocatedBefore > allocatedAfter ? allocatedBefore - allocatedAfter : 0;
    GetGCStats(GCCycleGeneration::YOUNG).collectedBytes = stats.reclaimedBytes;

    // Residual Register and the remset walk now both complete in STW3, before
    // EvacuateYoungRegions retires the forwarding receipts. Then enter IDLE.
    if (stw != nullptr) {
        stw.reset();
    }

    {
        // minortime: ⑧ post-evac finish
        MRT_PHASE_TIMER(ZStatPhases::PYoungPostEvacFinish);
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true, true);
        MergeResurrectExportObjects(Generation::Young);
    }
    ++minorTotalRuns;
    uint64_t pauseUs = (TimeUtil::NanoSeconds() - start) / NS_PER_US;
    VLOG(REPORT,
         "[GCV2Minor] run=%zu fallbackFullScan=%u candidates=%zu candidateBytes=%zu liveBytes=%zu "
         "remembered=%zu reclaimedBytes=%zu pause=%zu us",
         minorTotalRuns, static_cast<unsigned>(fullYoungScan), stats.candidateRegions, stats.candidateBytes,
         liveBytes, liveRememberedCount, stats.reclaimedBytes, pauseUs);



}
} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
// ZGeneration's rendezvous only waits for in-flight mutator accesses. It
// must not start another root scan after old marking has completed
// (zGeneration.cpp:114-122,1343-1353).
class ZRendezvousHandshakeClosure final : public HandshakeClosure {
public:
    ZRendezvousHandshakeClosure() : HandshakeClosure("ZRendezvous") {}
    void do_thread(ThreadLocalData*) override {}
};

void CopyCollector::ProcessOldNonStrongReferences(WorkStack& workStack)
{
    ZBreakpoint::AtAfterReferenceProcessingStarted();
    CHECK_DETAIL(oldCycle.Phase() == GC_PHASE_MARK_COMPLETE,
                 "non-strong references require completed old marking");
    {
        MRT_PHASE_TIMER(ZStatPhases::PIdentifyUselessExternRef);
        FindUselessExternObjects();
    }
    // Finalizable graphs were followed during mark discovery. This phase
    // only classifies the final strong/live state (zReferenceProcessor.cpp:285).
    ProcessFinalizers();
    StringDedup::Instance().Clean([this](BaseObject* object) {
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
        return region->IsYoungRegion() || IsMarkedObject<Generation::Old>(object);
    });
    // zGeneration.cpp:1344-1373: finish in-flight weak loads before unblocking.
    // A serial driver and synchronous ZWorkers::run have already joined GC
    // work here; mutators (including the finalizer thread) need a rendezvous.
    ZRendezvousHandshakeClosure rendezvous;
    Handshake::execute(&rendezvous);
    collectorResources.UnblockResurrection();
    collectorResources.GetFinalizerProcessor().EnqueueReferences();
    // zGeneration.cpp:1147-1168: the serial driver excludes young collections
    // while this verification safepoint observes the weak-inclusive graph.
    if (ZVerifyRoots || ZVerifyObjects) {
        ScopedStopTheWorld stw("verify after weak processing", false);
        ZVerify::BeforeZOperation();
        ZVerify::AfterWeakProcessing();
    }
}

bool CopyCollector::TryEndOldMark(WorkStack& workStack, WorkStack& foreignRootsSet)
{
    // ZGenerationOld::pause_mark_end / ZMark::end: a single pause attempt.
    MarkStripeSet& stripes = oldCycle.Mark().Stripes();
    ScopedStopTheWorld stw("old mark end", true, GC_PHASE_CLEAR_SATB_BUFFER);
    ZVerify::BeforeZOperation();
    NoteMarkTerminatePause();
    const size_t before = stripes.Population();
    (void)workStack;
    const bool ended = oldCycle.Mark().TryEnd();
    const size_t after = stripes.Population();
    NoteMarkTerminateFlushed(after >= before ? after - before : 0);
    if (!ended) {
        NoteMarkTerminateContinue(workStack.size() + stripes.Population());
        return false;
    }
    // Preserve export ownership discovery after the ordinary root closure,
    // while the mark-end pause excludes new mutator publication.
    ProcessExportRoots(foreignRootsSet);
    // ZMark::mark_follow (zMark.cpp:948): after workers join, return abort
    // to the phase owner before verification or publishing mark completion.
    if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
        return false;
    }
    MarkingStacks::VerifyAllEmpty(oldCycle.Mark());
    oldCycle.PublishPhase(GC_PHASE_MARK_COMPLETE);
    ZVerify::AfterMark();
    collectorResources.BlockResurrection();
    ReportMarkTerminateContinue();
    return true;
}

bool CopyCollector::FlushMarkProducers(ZMark* domain)
{
    bool flushed = domain != nullptr ? domain->TryTerminateFlush() : ZMark::FlushAllGenerations();
    if (domain != nullptr) {
        flushed = domain->FlushStacks() || flushed || !domain->Stripes().IsEmpty();
    }
    return flushed;
}



} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
void CopyCollector::Init() {}

void CopyCollector::Fini() { Collector::Fini(); }

BaseObject* CopyCollector::ResolveCurrentValueRoot(BaseObject* value, const void* owner, Generation generation,
                                                      ForwardingStage stage) const
{
    if (value == nullptr || !Heap::IsHeapAddress(value)) {
        return value;
    }
    const ForwardingProvenance provenance{
        ForwardingHolderKind::Static, owner, nullptr, stage, ForwardingWriterKind::CollectorHeal,
        ForwardingSourceKind::CallerValue, nullptr, nullptr, ForwardingFieldKind::RootSlot
    };
    // ZUncoloredRoot::make_load_good (zUncoloredRoot.inline.hpp:62-69)
    // preserves load-good identity. IncomingNew carries the caller's current
    // identity; a page owner alone cannot distinguish overlapping from/to keys.
    if (stage == ForwardingStage::IncomingNew) {
        return ValidateCurrentValue(value, provenance);
    }
    // Stored roots still need remapping using their source page's generation,
    // which can differ from the generation currently visiting the roots.
    (void)generation;
    const auto forwarding = forwarding_for_page(
        Heap::page(reinterpret_cast<MAddress>(value)));
    BaseObject* current = value;
    if (forwarding) {
        const MAddress target = forwarding->find(reinterpret_cast<MAddress>(value));
        current = target != 0 ? reinterpret_cast<BaseObject*>(target)
            : ResolveStoreValue(value, provenance, static_cast<Generation>(forwarding->table_generation()));
    }
    CHECK_DETAIL(current != nullptr && Heap::IsHeapAddress(current),
                 "value root resolve requires a heap to-address from=%p current=%p", value, current);
    CHECK_DETAIL(Collector::JudgeHandOutTarget(current) == HandVerdict::Usable,
                 "value root resolve requires a usable target from=%p current=%p", value, current);
    return current;
}

void CopyCollector::CurrentizeValueRootSet(ValueRootSet& roots, Generation generation) const
{
    ValueRootSet current;
    current.reserve(roots.size());
    for (const ValueRoot& value : roots) {
        current.insert(ValueRoot(ResolveCurrentValueRoot(value, &roots, generation, value.Stage()),
                                 ForwardingStage::IncomingNew));
    }
    roots.swap(current);
}

void CopyCollector::CurrentizeValueRootMap(
    ValueRootMap& roots, Generation generation) const
{
    ValueRootMap current;
    current.reserve(roots.size());
    for (const auto& entry : roots) {
        ValueRoot key(ResolveCurrentValueRoot(entry.first, &roots, generation, entry.first.Stage()),
                      ForwardingStage::IncomingNew);
        ValueRootList& values = current[key];
        for (const ValueRoot& value : entry.second) {
            values.emplace_back(ResolveCurrentValueRoot(value, &roots, generation, value.Stage()),
                                ForwardingStage::IncomingNew);
        }
    }
    roots.swap(current);
}

// Registered finalizers are discovered during old root marking and fixed by
// VisitNativePointers. Only queued/running finalizables are strong mark roots.


void CopyCollector::VisitSurrectedExportRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes, Generation::Old);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, Generation::Old);
        for (BaseObject* obj : resurrectedExportObjectes) {
            visitor(obj);
        }
        for (BaseObject* obj : resurrectedExportObjectesForwardPhase) {
            visitor(obj);
        }
    }
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CurrentizeValueRootMap(cycleRefWorkStack, Generation::Old);
    auto it = cycleRefWorkStack.begin();
    while (it != cycleRefWorkStack.end()) {
        BaseObject* exportObj = it->first;
        visitor(exportObj);
        for (auto &externObj : it->second) {
            visitor(externObj.object);
        }
        it++;
    }
}

} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zCollectedHeap.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/Collector/GcStats.h"
#include "Common/BaseObject.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Common/StateWord.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
GCCycleSnapshot GenerationCycle::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return { generation, sequence, requestIndex, reason.load(std::memory_order_relaxed),
             phase.load(std::memory_order_relaxed), active };
}

void GenerationCycle::SelectReason(GCReason value, uint64_t index)
{
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(!active);
    requestIndex = index;
    reason.store(value, std::memory_order_release);
}

void GenerationCycle::Begin(uint64_t index)
{
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(!active);
    requestIndex = index;
    active = true;
}

void GenerationCycle::PublishPhase(GCPhase value)
{
    std::lock_guard<std::mutex> lock(mutex);
    phase.store(value, std::memory_order_release);
}

void GenerationCycle::End()
{
    std::lock_guard<std::mutex> lock(mutex);
    active = false;
}

}

namespace MapleRuntime {
void GenerationCycle::InitializeWorkers(uint32_t capacity)
{
    CHECK(workers == nullptr);
    workers = std::make_unique<ZWorkers>(generation, capacity, &statWorkers);
}

void GenerationCycle::StopWorkers()
{
    workers.reset();
}
}

namespace MapleRuntime {
void WCollector::DoGarbageCollection(GCCycleGeneration generation)
{
    if (generation == GCCycleGeneration::YOUNG) {
        DoYoungGarbageCollection();
        return;
    }
    // ZGenerationCollectionScopeOld: overlap young with the old body.
    DriverUnlocker unlocker(collectorResources);
    TraceHeap();
    if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
        return;
    }
    PostTrace();
    if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
        return;
    }

    if (!Preforward()) {
        return;
    }
    // ZGenerationOld::collect: no abort boundary after relocate-start.
    // Complete the remaining pages before returning to the request owner.

    ForwardFromSpace(GCCycleGeneration::OLD);
    reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager().FinishIncompleteFromRegions(GCCycleGeneration::OLD);
    if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
        return;
    }

    // Preserve young remembered-set faces across old/full collection. ZGC old
    // relocation transfers remembered fields; it does not globally erase the
    // young current face. ClearRegion/TransferObjectSlots remain the authorities
    // for reclaimed or moved holders (zRelocate.cpp:652-731).
    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    MergeResurrectExportObjects(Generation::Old);
    PostResolveCycleTask();

    CollectSmallSpace();
    // domainon: major path coverage dump (Record may fire under non-YOUNG if youngRegion).
    // retmid: do NOT StampCensusBoundaries / PromoteAllRegions here.
    // Ablation D (both major STWs disabled) restores mid_alloc 5/5; any of
    // Flush/Stamp/Promote in these STWs reintroduces 0/5 or residual 甲 under
    // FYS=0 SKIP_PINNED=1 512MB. Retained-liveness still applies on residual and
    // in-place promote paths that already preserve page liveness.

}
}

namespace MapleRuntime {
void CopyCollector::PreGarbageCollection(GCCycleGeneration generation, bool isConcurrent, uint64_t gcIndex)
{
    const bool continuingPrelude = GetGenerationCycle(generation).Snapshot().active;
    if (!continuingPrelude) {
        GetGenerationCycle(generation).Begin(gcIndex);
    }
    ResetSkippedStackMapCounts();
    VLOG(REPORT, "Begin GC log. GCReason: %s, Current allocated %s, Current threshold %s",
         g_gcRequests[GetCycleSnapshot(generation).reason].name, Pretty(Heap::GetHeap().GetAllocatedSize()).Str(),
         Pretty(Heap::GetHeap().GetCollector().GetGCStats().GetThreshold()).Str());

    // zDriver.cpp:183,399-400: generation workers use their concurrent
    // budget for both pause and concurrent work. Parallel workers are separate.
    const int32_t threadCount = static_cast<int32_t>(GetWorkers(generation).active_workers());
    GetWorkers(generation).set_active();
    VLOG(REPORT, "GC generation active workers: %d", threadCount);

    GetGCStats(generation).reason = GetCycleSnapshot(generation).reason;
    GetGCStats(generation).async = (gcIndex == GCTask::ASYNC_TASK_INDEX);
    GetGCStats(generation).isConcurrentMark = isConcurrent;
#if defined(MRT_TESTABLE_INTERNALS)
    if (testCyclePrepared) {
        testCyclePrepared();
    }
#endif
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    DumpBeforeGC();
#endif
    TRACE_COUNT("CJRT_pre_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}
}

namespace MapleRuntime {
void GenerationCycle::SetYoungType(ZYoungType type)
{
    CHECK(generation == GCCycleGeneration::YOUNG);
    youngType.store(type, std::memory_order_release);
}

YoungTypeSetter::YoungTypeSetter(GenerationCycle& cycle, ZYoungType type) : cycle(cycle)
{
    CHECK(type != ZYoungType::none);
    CHECK(cycle.YoungType() == ZYoungType::none);
    cycle.SetYoungType(type);
}

YoungTypeSetter::~YoungTypeSetter()
{
    CHECK(cycle.YoungType() != ZYoungType::none);
    cycle.SetYoungType(ZYoungType::none);
}
}

namespace MapleRuntime {
void GenerationCycle::SelectTenuringThreshold(const TenuringInputs& inputs)
{
    CHECK(generation == GCCycleGeneration::YOUNG);
    // zGeneration.cpp:704-715: preclean promotes all, other types compute.
    stats.tenuringThreshold = YoungType() == ZYoungType::major_full_preclean
        ? 0 : ComputeTenuringThreshold(inputs);
}
}

namespace MapleRuntime {
void CopyCollector::DoTracing(WorkStack& workStack, WorkStack& foreignRootsSet)
{
    ScopedEntryTrace trace("CJRT_GC_TRACE");
    MRT_PHASE_TIMER(ZStatPhases::PDoTracing);
    VLOG(REPORT, "roots size: %zu", workStack.size());
#if defined(MRT_TESTABLE_INTERNALS)
    if (testOldMarkStarted) {
        testOldMarkStarted();
    }
#endif

    {
        MRT_PHASE_TIMER(ZStatPhases::PConcurrentMarking);
        TracingImpl(workStack);
    }

    ZBreakpoint::AtBeforeMarkingCompleted();

    // ZGenerationOld::collect (zGeneration.cpp:1020-1030): mark-follow
    // returns to the phase owner before any mark-end retry consumes stripes.
    if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
        return;
    }
    while (!TryEndOldMark(workStack, foreignRootsSet)) {
        if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
            return;
        }
        MRT_PHASE_TIMER(ZStatPhases::PConcurrentReMarking);
        TransitionToGCPhase(GC_PHASE_TRACE, true);
        TracingImpl(workStack);
        if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
            return;
        }
    }

    if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
        return;
    }
    // ZGenerationOld::collect processes non-strong references only after the
    // successful mark-end pause has closed ordinary mark publication.
    ProcessOldNonStrongReferences(workStack);

#if defined(MRT_TESTABLE_INTERNALS)
    // All major tasks and finalizer work have flushed before page selection.
    // Major has no reachableVec carrier; observers read the actual page state.
    ObserveMarkClosureForTest(nullptr);
#endif
    VLOG(REPORT, "mark %zu objects", markedObjectCount.load(std::memory_order_relaxed));
}
}

#if defined(MRT_TESTABLE_INTERNALS)
// Instantiate the product template for the policy matrix. Tests import these
// exact bodies from the DSO, as the existing page-mark tests do.
#include "Heap/z/zGeneration.inline.hpp"
namespace MapleRuntime {
template void GenerationCycle::MarkObjectIfActive<false, false, false, false>(zaddress);
template void GenerationCycle::MarkObjectIfActive<false, false, false, true>(zaddress);
template void GenerationCycle::MarkObjectIfActive<false, false, true, false>(zaddress);
template void GenerationCycle::MarkObjectIfActive<false, false, true, true>(zaddress);
template void GenerationCycle::MarkObjectIfActive<false, true, false, false>(zaddress);
template void GenerationCycle::MarkObjectIfActive<false, true, false, true>(zaddress);
template void GenerationCycle::MarkObjectIfActive<false, true, true, false>(zaddress);
template void GenerationCycle::MarkObjectIfActive<false, true, true, true>(zaddress);
template void GenerationCycle::MarkObjectIfActive<true, false, false, false>(zaddress);
template void GenerationCycle::MarkObjectIfActive<true, false, false, true>(zaddress);
template void GenerationCycle::MarkObjectIfActive<true, false, true, false>(zaddress);
template void GenerationCycle::MarkObjectIfActive<true, false, true, true>(zaddress);
template void GenerationCycle::MarkObjectIfActive<true, true, false, false>(zaddress);
template void GenerationCycle::MarkObjectIfActive<true, true, false, true>(zaddress);
template void GenerationCycle::MarkObjectIfActive<true, true, true, false>(zaddress);
template void GenerationCycle::MarkObjectIfActive<true, true, true, true>(zaddress);
}
#endif
