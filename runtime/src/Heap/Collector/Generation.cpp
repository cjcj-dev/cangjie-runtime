// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/Verify/ZVerify.h"
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
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Collector/GcTriggerFlags.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/Collector/TenuringThreshold.h"
#include "Heap/GcThreadPool.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Collector/MarkingStacks.h"
#include "Heap/Verify/Zap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/NwDropAudit.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/SurvNodeDiag.h"
#include "Heap/Verify/CsetEmptyWho.h"
#include "Common/ColourPredicates.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
// ZGenerationYoung::mark_start (zGeneration.cpp:871-880). Called under the
// young mark-start safepoint; sequence readers cannot observe half this event.
void GenerationCycle::StartYoungMark(RememberedSet& rememberedSet)
{
    CHECK(generation == GCCycleGeneration::YOUNG);
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(active);
    CHECK(sequence != UINT64_MAX);
    ++sequence;
    rememberedSet.FlipForMinor();
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
std::atomic<Mutator*> g_markBeforeMarkEndProducer { nullptr };
std::atomic<BaseObject*> g_markBeforeMarkEndFirst { nullptr };
std::atomic<BaseObject*> g_markBeforeMarkEndSecond { nullptr };
std::atomic<uint64_t> g_markBeforeMarkEndPublications { 0 };
std::atomic<BaseObject*> g_allocBlackDuringConcurrent { nullptr };
std::atomic<BaseObject*> g_y2yDuringConcurrent { nullptr };
std::atomic<BaseObject*> g_leftoverAllocBlackBeforePause { nullptr };
std::atomic<BaseObject*> g_leftoverY2yBeforePause { nullptr };
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
    auto claimPublication = [](std::atomic<uint64_t>& publications) {
        uint64_t remaining = publications.load(std::memory_order_acquire);
        while (remaining != 0 &&
               !publications.compare_exchange_weak(remaining, remaining - 1,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {}
        return remaining != 0;
    };
    if (claimPublication(g_y2yAfterReleasePublications)) {
        BaseObject* holder = g_y2yAfterReleaseHolder.load(std::memory_order_acquire);
        CHECK_DETAIL(holder != nullptr, "armed y2y after-release receipt without holder");
        AllocBuffer::GetOrCreateAllocBuffer()->PushY2yDirtyHolder(holder);
    }
}

void ArmMarkBeforeMarkEndTestReceipt(Mutator* producer, BaseObject* first, BaseObject* second)
{
    g_markBeforeMarkEndProducer.store(producer, std::memory_order_release);
    g_markBeforeMarkEndFirst.store(first, std::memory_order_release);
    g_markBeforeMarkEndSecond.store(second, std::memory_order_release);
    g_markBeforeMarkEndPublications.store(second == nullptr ? 1 : 2, std::memory_order_release);
}

void PublishMarkBeforeMarkEndTestReceipt()
{
    uint64_t remaining = g_markBeforeMarkEndPublications.load(std::memory_order_acquire);
    while (remaining != 0 &&
           !g_markBeforeMarkEndPublications.compare_exchange_weak(remaining, remaining - 1,
                                                                  std::memory_order_acq_rel,
                                                                  std::memory_order_acquire)) {}
    if (remaining == 0) {
        return;
    }
    Mutator* producer = g_markBeforeMarkEndProducer.load(std::memory_order_acquire);
    BaseObject* first = g_markBeforeMarkEndFirst.load(std::memory_order_acquire);
    BaseObject* second = g_markBeforeMarkEndSecond.load(std::memory_order_acquire);
    BaseObject* object = remaining == 2 ? first : (second != nullptr ? second : first);
    CHECK_DETAIL(producer != nullptr && object != nullptr, "armed mark-end receipt without producer/object");
    Heap::GetHeap().GetCollector().MarkObjectIfActive(object);
    producer->FlushStoreBarrierBuffer();
}

void ArmAllocBlackDuringConcurrentTestReceipt(BaseObject* object)
{
    g_allocBlackDuringConcurrent.store(object, std::memory_order_release);
}

void ArmY2yDuringConcurrentTestReceipt(BaseObject* holder)
{
    g_y2yDuringConcurrent.store(holder, std::memory_order_release);
}

void PublishConcurrentYoungProducersTestReceipt()
{
    BaseObject* allocBlack = g_allocBlackDuringConcurrent.exchange(nullptr, std::memory_order_acq_rel);
    if (allocBlack != nullptr) {
        AllocBuffer::GetOrCreateAllocBuffer()->PushYoungAllocBlack(allocBlack);
    }
    BaseObject* y2y = g_y2yDuringConcurrent.exchange(nullptr, std::memory_order_acq_rel);
    if (y2y != nullptr) {
        AllocBuffer::GetOrCreateAllocBuffer()->PushY2yDirtyHolder(y2y);
    }
}

void ArmLeftoverBeforePauseTestReceipt(BaseObject* allocBlack, BaseObject* y2yHolder)
{
    g_leftoverAllocBlackBeforePause.store(allocBlack, std::memory_order_release);
    g_leftoverY2yBeforePause.store(y2yHolder, std::memory_order_release);
}

void PublishLeftoverBeforePauseTestReceipt()
{
    BaseObject* allocBlack = g_leftoverAllocBlackBeforePause.exchange(nullptr, std::memory_order_acq_rel);
    if (allocBlack != nullptr) {
        AllocBuffer::GetOrCreateAllocBuffer()->PushYoungAllocBlack(allocBlack);
    }
    BaseObject* y2y = g_leftoverY2yBeforePause.exchange(nullptr, std::memory_order_acq_rel);
    if (y2y != nullptr) {
        AllocBuffer::GetOrCreateAllocBuffer()->PushY2yDirtyHolder(y2y);
    }
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
    producer->FlushStoreBarrierBuffer();
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
    collectorResources.NoteYoungMarkStart();
    flip_young_mark_start();
    ZVerify::OnColorFlip();
    StartYoungMarkWork();

    {
        // minortime: ① FlushAllocationRegions
        MRT_PHASE_TIMER(ZStatPhases::PYoungFlushAlloc);
        reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager().ResetTLABUsage();
        FlushAllocationRegions();
    }

    if (const GCDriverRequest* request = collectorResources.YoungPreludeRequest()) {
        oldCycle.SelectReason(request->reason);
        oldCycle.Begin(request->asynchronous ? GCTask::ASYNC_TASK_INDEX : request->sequence);
        StartOldMarkWork();
        flip_old_mark_start();
        ZVerify::OnColorFlip();
        // Reset the old mark face before young roots can publish old work.
        // ZGenerationOld::mark_start -> ZMark::start (zGeneration.cpp:1212-1237).
        reinterpret_cast<RegionSpace&>(theAllocator).AssembleGarbageCandidates();
        oldCycle.PublishPhase(GCPhase::GC_PHASE_ENUM);
    }


    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    minorCandidateRegions.clear();
    YoungCollectionStats stats;
    {
        // minortime: ② PrepareYoungGarbageCandidates
        MRT_PHASE_TIMER(ZStatPhases::PYoungPrepareCandidates);
        stats = manager.PrepareYoungGarbageCandidates(
            [this](RegionInfo* region) { minorCandidateRegions.insert(region); });
    }
    // Complete the mark-start sequence/flip event after flushing pre-flip
    // producers and before publishing the young mark phase (zGeneration.cpp:871-880).
    MinorSlotSet rememberedSlots;
    {
        // Remembered-set face flip; sparse consumption after world release.
        MRT_PHASE_TIMER(ZStatPhases::PYoungRemsetDrain);
        RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
        (void)MutatorManager::Instance().HandshakeFlushMarkProducers(nullptr);
        // S5 flip only (YOUNG_CONCURRENT.md). ScanPreviousForMinor runs after
        // world-release with mark_follow (zRemembered.cpp:561-576).
        youngCycle.StartYoungMark(rememberedSet);

    }

    // Publish the reset young mark face before phase-change store-buffer
    // scanning can enqueue targets (zGeneration.cpp:855-881).
    youngCycle.PublishPhase(GC_PHASE_ENUM);
#if defined(MRT_TESTABLE_INTERNALS)
    if (testYoungMarkStarted) {
        testYoungMarkStarted();
    }
#endif

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
        // release publication point; AcknowledgeEpochHandshake asserts ENUM
        // before it is allowed to snapshot a single frame.
        Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
        stw.reset();


        EpochHandshakeStats handshake = MutatorManager::Instance().RunEpochHandshake("pre-minor-stack", true);
        stackScanEpoch = handshake.epoch;
        CHECK_DETAIL(stackScanEpoch != 0 && handshake.stackScanned + handshake.stackFallback == handshake.requested,
                     "minor concurrent stack scan accounting failed: epoch=%llu requested=%zu scanned=%zu "
                     "fallback=%zu",
                     static_cast<unsigned long long>(stackScanEpoch), handshake.requested, handshake.stackScanned,
                     handshake.stackFallback);

        // CLEAR is the closing edge for ENUM writes: it flushes every mutator's
        // store buffer before the root pass follows published mark work.
        stw = std::make_unique<ScopedStopTheWorld>("young collection", false);
        ZVerify::BeforeZOperation();
        TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);

        // finish_processing semantics: under the closing STW first ask the GC
        // owner to complete every unfinished epoch cursor. If a cursor still
        // cannot establish DONE (for example, missing managed bounds), preserve
        // the exact legacy phase-enum fallback before roots are merged.
        MutatorManager::Instance().VisitAllMutators([stackScanEpoch](Mutator& mutator) {
            if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                (void)mutator.GcPhaseEnum(GCPhase::GC_PHASE_ENUM, true, stackScanEpoch, false);
            }
            if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                (void)mutator.GcPhaseEnum(GCPhase::GC_PHASE_ENUM, true);
            }
        });
        VerifyStackRootPostcondition(stackScanEpoch, "minor");

    }

    constexpr bool fullYoungScan = false;
    WorkStack workStack = NewWorkStack();
    MarkingStacks::VerifyEmpty(workStack.size());
    MarkingStacks::VerifyEmpty(GetWorkers().GetSnapshot().remainingWorkers);
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
        (void)MutatorManager::Instance().HandshakeFlushMarkProducers(youngMarkDomain.get());
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
        // ZMarkYoungRootsTask::work publishes its own root stacks before follow.
        (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), *youngMarkDomain);
#if defined(MRT_TESTABLE_INTERNALS)
        NoteY2yAfterRootTestReceipt(youngMarkDomain->Stacks().Population());
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
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
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
    for (;;) {
        // Concurrent mark-follow drains work published by the previous pause.
        // Its worker completion is coordinated by YoungMarkTerminate (the
        // ZMarkTerminate worker-count/wakeup state machine), not pool polling.
        const bool workersTerminated =
            FollowYoungMark(workStack, fullYoungScan, reachableVec, reachableSlots,
                                weakSlots, &concWindow);
        CHECK_DETAIL(workersTerminated, "young concurrent mark workers did not terminate");
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
        theAllocator.VisitAllocBuffers([&workStack](AllocBuffer& buffer) {
            // Frozen leftovers only. Concurrent FollowYoungMark already
            // merged live alloc-buffer roots / allocate-black / y2y into the
            // termination domain. Pause must not become the first consumer.
#if defined(MRT_TESTABLE_INTERNALS)
            NoteMarkTerminatePauseProducers(buffer.YoungAllocBlackCount(),
                                            buffer.Y2yDirtyHolderCount() + buffer.Y2yDirtySlotCount());
#endif
            buffer.MergeYoungAllocBlackFollow(workStack);
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
            MarkingStacks::VerifyEmpty(GetWorkers().GetSnapshot().remainingWorkers);
#if defined(MRT_TESTABLE_INTERNALS)
            NoteExportRootPublicationAtT2TestReceipt();
#endif
            break;
        }
        NoteMarkTerminateContinue(workStack.size());
        ++concWindow.reenters;
        stw.reset();
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
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
    {
        // minortime: ⑧ pre-evac finish (phase + weak/satb clear)
        MRT_PHASE_TIMER(ZStatPhases::PYoungPreEvacClear);
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
        // zGeneration.cpp:563 / :699-701: after this young mark_end, reset
        // the previous young relocation set. Independent of old remap.
        ForwardingTable::ResetRelocationSet(Generation::Young);
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

    // Residual Register and the remset walk now both complete in STW3, before
    // EvacuateYoungRegions retires the forwarding receipts. Then enter IDLE.
    if (stw != nullptr) {
        stw.reset();
    }

    {
        // minortime: ⑧ post-evac finish
        MRT_PHASE_TIMER(ZStatPhases::PYoungPostEvacFinish);
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
        MergeResurrectExportObjects(Generation::Young);
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
