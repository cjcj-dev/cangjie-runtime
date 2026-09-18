// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zAbort.hpp"
#include "Heap/z/zBreakpoint.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zStringDedup.hpp"
#include "Heap/z/zResurrection.hpp"
#include "Heap/z/zMark.hpp"

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
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zRelocationSetSelector.inline.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zJNICritical.hpp"
#include "Heap/z/zPageTable.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zWeakRootsProcessor.hpp"
#include "Common/SuspendibleThreadSet.h"
#include "Sync/Sync.h"
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
#include "Heap/z/zRelocate.hpp"

#include "Heap/z/z_globals.hpp"
namespace MapleRuntime {
ZGenerationYoung* ZGeneration::_young = nullptr;
ZGenerationOld* ZGeneration::_old = nullptr;

ZGenerationYoung::ZGenerationYoung() : ZGeneration(ZGenerationId::young)
{
    previousYoung = _young;
    _young = this;
}
ZGenerationYoung::~ZGenerationYoung()
{
    if (_young == this) {
        _young = previousYoung;
    }
}
ZGenerationOld::ZGenerationOld() : ZGeneration(ZGenerationId::old)
{
    previousOld = _old;
    _old = this;
}
ZGenerationOld::~ZGenerationOld()
{
    if (_old == this) {
        _old = previousOld;
    }
}

ZGenerationId ZGeneration::id() const { return _id; }

ZGenerationIdOptional ZGeneration::id_optional() const
{
    return static_cast<ZGenerationIdOptional>(id());
}

bool ZGeneration::is_young() const { return id() == ZGenerationId::young; }
bool ZGeneration::is_old() const { return id() == ZGenerationId::old; }
uint32_t ZGeneration::seqnum() const { return static_cast<uint32_t>(Sequence()); }
ZGenerationYoung* ZGeneration::young() { return _young; }
ZGenerationOld* ZGeneration::old() { return _old; }
ZGeneration* ZGeneration::generation(ZGenerationId id)
{
    return id == ZGenerationId::young ? static_cast<ZGeneration*>(_young) : static_cast<ZGeneration*>(_old);
}

ZGeneration::ZGeneration(ZGenerationId generation)
    : mark(std::make_unique<ZMark>(ZMarkStripesMax,
          generation == ZGenerationId::young ? MarkingStacks::MarkingGeneration::YOUNG
                                                 : MarkingStacks::MarkingGeneration::MAJOR)),
      _id(generation == ZGenerationId::young ? ZGenerationId::young : ZGenerationId::old),
      _cycle(generation),
      _relocation_set(this),
      _relocate(std::make_unique<ZRelocate>(this))
{
    ZJNICritical::initialize();
}

ZGeneration::~ZGeneration()
{
    StopWorkers();
}

// ZGC zGeneration.cpp:197-207: select policy at the generation boundary.
static double fragmentation_limit(ZGenerationId generation)
{
    if (generation == ZGenerationId::old) {
        return ZFragmentationLimit;
    } else {
        return ZYoungCompactionLimit;
    }
}
double ZGeneration::FragmentationLimit() const
{
    return fragmentation_limit(_cycle);
}

void ResetSkippedStackMapCounts();
void ReportSkippedStackMapCounts();

// ZGenerationOld::relocate_start (zGeneration.cpp:1379-1397) captures the
// young sequence once for the whole old relocation, not once per forwarding.
void ZGeneration::RecordYoungSequenceAtRelocateStart(uint64_t youngSequence)
{
    CHECK(_cycle == ZGenerationId::old);
    youngSequenceAtRelocateStart.store(youngSequence, std::memory_order_release);
}

bool ZGeneration::ActiveRemsetIsCurrent(uint64_t youngSequence) const
{
    CHECK(_cycle == ZGenerationId::old);
    // zGeneration.inline.hpp:174-182: each young mark start flips the faces.
    return ((youngSequence - youngSequenceAtRelocateStart.load(std::memory_order_acquire)) & 1U) == 0;
}




// ZGeneration::mark_object, zGeneration.inline.hpp:119-123.



class VM_ZOperation {
public:
    virtual ~VM_ZOperation() = default;
    virtual bool do_operation() = 0;
    virtual bool block_jni_critical() const { return false; }
    bool pause()
    {
        if (block_jni_critical()) {
            ZJNICritical::block();
        }
        bool success = false;
        {
            ScopedStopTheWorld stw("zoperation", false);
            ZVerify::BeforeZOperation();
            success = do_operation();
        }
        if (block_jni_critical()) {
            ZJNICritical::unblock();
        }
        return success;
    }
};

class VM_ZMarkStartYoung : public VM_ZOperation {
public:
    bool do_operation() override
    {
        ZGeneration::young()->mark_start();
        return true;
    }
    bool block_jni_critical() const override { return true; }
};

class VM_ZMarkStartYoungAndOld : public VM_ZOperation {
public:
    bool do_operation() override
    {
        ZGeneration::young()->mark_start();
        ZGeneration::old()->mark_start();
        return true;
    }
    bool block_jni_critical() const override { return true; }
};

class VM_ZMarkEndYoung : public VM_ZOperation {
public:
    bool do_operation() override { return ZGeneration::young()->mark_end(); }
};

class VM_ZRelocateStartYoung : public VM_ZOperation {
public:
    bool do_operation() override
    {
        ZGlobalsPointers::flip_young_relocate_start();
        ZVerify::OnColorFlip();
        ZGeneration::young()->set_phase(ZGeneration::Phase::Relocate);
        return true;
    }
    bool block_jni_critical() const override { return true; }
};

class VM_ZMarkEndOld : public VM_ZOperation {
public:
    bool do_operation() override { return ZGeneration::old()->mark_end(); }
};

class VM_ZRelocateStartOld : public VM_ZOperation {
public:
    bool do_operation() override
    {
        ZGlobalsPointers::flip_old_relocate_start();
        ZVerify::OnColorFlip();
        ZGeneration::old()->set_phase(ZGeneration::Phase::Relocate);
        ZGeneration::old()->RecordYoungSequenceAtRelocateStart(ZGeneration::young()->Sequence());
        return true;
    }
    bool block_jni_critical() const override { return true; }
};

class VM_ZVerifyOld : public VM_ZOperation {
public:
    bool do_operation() override
    {
        if (ZVerifyRoots || ZVerifyObjects) {
            ZVerify::AfterWeakProcessing();
        }
        return true;
    }
};

void HeapGcState::DoYoungGarbageCollection()
{
    Heap::GetHeap().young().collect();
}

void ZGeneration::at_collection_start(void* timer)
{
    set_gc_timer(timer);
    reset_statistics();
}

void ZGeneration::at_collection_end()
{
    set_gc_timer(nullptr);
    End();
}

ZGenerationCollectionScopeYoung::ZGenerationCollectionScopeYoung(ZGenerationYoung& generation)
    : generation(generation)
{
    generation.at_collection_start();
}

ZGenerationCollectionScopeYoung::~ZGenerationCollectionScopeYoung()
{
    generation.at_collection_end();
}

ZGenerationCollectionScopeOld::ZGenerationCollectionScopeOld(ZGenerationOld& generation)
    : generation(generation)
{
    generation.at_collection_start();
}

ZGenerationCollectionScopeOld::~ZGenerationCollectionScopeOld()
{
    generation.at_collection_end();
}

static HeapGcState& TheCollector()
{
    return static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
}

void ZGenerationYoung::collect()
{
    ZGenerationCollectionScopeYoung scope(*this);
    pause_mark_start();
    DriverUnlocker unlocker;
    concurrent_mark();
    abortpoint();
    while (!pause_mark_end()) {
        concurrent_mark_continue();
        abortpoint();
    }
    concurrent_mark_free();
    abortpoint();
    concurrent_reset_relocation_set();
    abortpoint();
    concurrent_select_relocation_set();
    abortpoint();
    pause_relocate_start();
    concurrent_relocate();
}

void ZGenerationYoung::pause_mark_start()
{
    if (IsMajorRoots()) {
        VM_ZMarkStartYoungAndOld op;
        (void)op.pause();
    } else {
        VM_ZMarkStartYoung op;
        (void)op.pause();
    }
}

bool ZGenerationYoung::pause_mark_end()
{
    VM_ZMarkEndYoung op;
    return op.pause();
}

void ZGenerationYoung::mark_start()
{
    uint64_t start = TimeUtil::NanoSeconds();
    // VM_ZOperation::pause owns the STW (zGeneration.cpp:474-485).
    const ZYoungType type = Heap::GetHeap().young().YoungType();
    ZStat::Collections().AtYoungMarkStart(type == ZYoungType::major_full_roots ||
                                         type == ZYoungType::major_partial_roots);
    // VM_ZMarkStartYoungAndOld starts the complete young event before old
    // (zGeneration.cpp:601-602); a minor only enters the young event.
    CHECK(_cycle == ZGenerationId::young);
    CHECK(Snapshot().active);
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::Begin, mark.get());
    }
#endif
    ZGlobalsPointers::flip_young_mark_start();
    ZVerify::OnColorFlip();
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeRetire, mark.get());
    }
#endif

    auto& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    auto& manager = space.GetRegionManager();
    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungFlushAlloc);
        manager.ResetTLABUsage();
        Heap::GetHeap().object_allocator().retire_pages(kPageAgeRangeYoung);
        Heap::GetHeap().GetAllocator().VisitAllocBuffers([](AllocBuffer& buffer) { buffer.FlushRegion(); });
    }
    // Cangjie keeps allocation lists and candidate statistics in RegionManager.
    // Preparing those lists retires the young shared/pinned allocation pages.
    minorCandidateRegions.clear();
    YoungCollectionStats stats;
    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungPrepareCandidates);
        stats = manager.PrepareYoungGarbageCandidates(
            [this](ZPage* region) { minorCandidateRegions.insert(region); });
    }
    // Flush pre-flip producers before invalidating their generation sequence.
    (void)ZMark::FlushAllGenerations();
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeSequence, mark.get());
    }
#endif
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(sequence != UINT64_MAX);
        ++sequence;
    }
    set_phase(Phase::Mark);
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeDomain, mark.get());
    }
#endif
    Mark().BindWorkers(Workers());
    Mark().Start();
    MarkingStacks::VerifyEmpty(Mark().Stripes().Population());
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeRemembered, mark.get());
    }
#endif
    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungRemsetDrain);
        Heap::GetHeap().remembered().flip();
    }
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::Complete, mark.get());
    }
#endif
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testYoungMarkStarted) {
        HeapGcState::testYoungMarkStarted();
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

    youngStackScanEpoch = StackWatermark::epoch_id();
    MutatorManager::Instance().VisitAllMutators([epoch = youngStackScanEpoch](Mutator& mutator) {
        if (!mutator.GetStackWatermark().IsDone(epoch)) {
            (void)mutator.GcPhaseEnum(true, epoch, false);
        }
    });
    youngStats = stats;
    youngStartNs = start;
}

void ZGenerationYoung::concurrent_mark()
{
    uint64_t stackScanEpoch = youngStackScanEpoch;
    WorkStack& workStack = youngWorkStack;
    constexpr bool fullYoungScan = false;
    MarkingStacks::VerifyEmpty(workStack.size());
    std::vector<BaseObject*> reachableVec;
    reachableVec.reserve(1 << 17); // ~128k; real_load ~155k reachable
    MinorObjectSet allocationRoots;
    MinorObjectSet currentMinorRoots;
    MinorSlotSet reachableSlots;
    MinorSlotSet weakSlots;
    auto mergeY2yDirtyWork = [&](WorkStack& destination) {
        Heap::GetHeap().GetAllocator().VisitAllocBuffers([this, &destination](AllocBuffer& buffer) {
            buffer.MergeY2yDirtyHolders(destination);
            buffer.MergeY2yDirtySlots([this, &destination](MAddress slot) {
                RefField<>& field = HeapSlotAt<>(slot);
                BaseObject* target = TheCollector().ResolveMinorReference(field);
                TheCollector().PushYoungObject(target, destination, "y2y_slot");
            });
        });
    };
#if defined(MRT_TESTABLE_INTERNALS)
    auto pendingY2yDirtyWorkCount = [&]() {
        size_t pending = 0;
        Heap::GetHeap().GetAllocator().VisitAllocBuffers([&pending](AllocBuffer& buffer) {
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
        (void)Heap::GetHeap().young().Mark().Flush();
        TheCollector().VisitMinorRoots([this, &workStack, &currentMinorRoots](BaseObject* object) {
            if (Heap::IsHeapAddress(object)) {
                ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
                if (region != nullptr && !region->IsYoungRegion()) {
                    currentMinorRoots.insert(object);
                }
            }
            TheCollector().PushYoungObject(object, workStack, "minor_root");
        }, [this, &workStack, &currentMinorRoots](BaseObject* object) {
            if (!Heap::IsHeapAddress(object)) {
                return;
            }
            ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
            if (region != nullptr && !region->IsYoungRegion()) {
                currentMinorRoots.insert(object);
            }
            TheCollector().PushYoungObject(object, workStack, "minor_root");
        }, stackScanEpoch);
        // ZMarkYoungRootsTask::work publishes its own root stacks before follow.
        (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), Heap::GetHeap().young().Mark());
#if defined(MRT_TESTABLE_INTERNALS)
        NoteY2yAfterRootTestReceipt(Heap::GetHeap().young().Mark().Stacks().Population());
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
        CHECK_DETAIL(stackScanEpoch != 0,
                     "young FOLLOW requires an epoch-backed concurrent stack-root receipt");
        concWindow.markedAtEntry = reachableVec.size();
        reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).PrepareTrace();
        mergeY2yDirtyWork(workStack);
#if defined(MRT_TESTABLE_INTERNALS)
        NoteY2yBeforeReleaseTestReceipt(pendingY2yDirtyWorkCount());
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
        MRT_PHASE_TIMER(ZStatPhases::PYoungMarkClosure);
        ++concWindow.closureCalls;
        TheCollector().TraceYoungClosure(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots,
                          reachableSlotDomain);
    }
    if (ZAbort::should_abort()) {
        return;
    }
    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungRemsetRescan);
        Heap::GetHeap().remembered().scan_and_follow(Heap::GetHeap().young().MarkPtr());
    }
#if defined(MRT_TESTABLE_INTERNALS)
    // Deterministic T1->T2 export-root window: root enumeration has returned,
    // and the concurrent mark-follow consumer has not started yet.
    PublishExportRootAfterT1TestReceipt();
#endif
    if (ZAbort::should_abort()) {
        return;
    }
    (void)TheCollector().FollowYoungMark(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots, &concWindow);
#if defined(MRT_TESTABLE_INTERNALS)
    FlushExportRootAfterT1TestReceipt();
    PublishMarkBeforeMarkEndTestReceipt();
    PublishLeftoverBeforePauseTestReceipt();
    PublishY2yAfterReleaseTestReceipt();
#endif
    youngReachableVec = std::move(reachableVec);
    youngConcWindow = concWindow;
    youngConcWindowStartNs = concWindowStartNs;
    youngWeakSlots = std::move(weakSlots);
    youngFullScan = fullYoungScan;
}

bool ZGenerationYoung::mark_end()
{
    WorkStack& workStack = youngWorkStack;
#if defined(MRT_TESTABLE_INTERNALS)
    const uint64_t markEndPauseStartNs = TimeUtil::NanoSeconds();
    const size_t y2yBatchAtMarkEnd = 0;
    (void)y2yBatchAtMarkEnd;
#endif
    Heap::GetHeap().GetAllocator().VisitAllocBuffers([](AllocBuffer& buffer) {
#if defined(MRT_TESTABLE_INTERNALS)
        NoteMarkTerminatePauseProducers(buffer.Y2yDirtyHolderCount() + buffer.Y2yDirtySlotCount());
#else
        (void)buffer;
#endif
    });
    Heap::GetHeap().GetAllocator().VisitAllocBuffers([this, &workStack](AllocBuffer& buffer) {
        buffer.MergeY2yDirtyHolders(workStack);
        buffer.MergeY2yDirtySlots([this, &workStack](MAddress slot) {
            RefField<>& field = HeapSlotAt<>(slot);
            BaseObject* target = TheCollector().ResolveMinorReference(field);
            TheCollector().PushYoungObject(target, workStack, "y2y_slot");
        });
    });
    const bool markEndSucceeded = TheCollector().TryEndYoungMark(workStack, &youngConcWindow);
#if defined(MRT_TESTABLE_INTERNALS)
    NoteMarkTerminatePauseDuration(TimeUtil::NanoSeconds() - markEndPauseStartNs);
#endif
    if (markEndSucceeded) {
        MarkingStacks::VerifyEmpty(workStack.size());
#if defined(MRT_TESTABLE_INTERNALS)
        NoteExportRootPublicationAtT2TestReceipt();
        if (HeapGcState::testYoungMarkCompleted) {
            HeapGcState::testYoungMarkCompleted();
        }
#endif
        ReportMarkTerminateContinue();
        Heap::GetHeap().young().set_phase(ZGeneration::Phase::MarkComplete);
        return true;
    }
    NoteMarkTerminateContinue(workStack.size());
    ++youngConcWindow.reenters;
    return false;
}

void ZGenerationYoung::concurrent_mark_continue()
{
    MinorSlotSet reachableSlots;
    (void)TheCollector().FollowYoungMark(youngWorkStack, youngFullScan, youngReachableVec, reachableSlots, youngWeakSlots,
                          &youngConcWindow);
}

void ZGenerationYoung::concurrent_mark_free()
{
    if (ZAbort::should_abort()) {
        return;
    }
    RegionSpace& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    {
        youngConcWindow.markedAtExit = youngReachableVec.size();
        if (youngConcWindowStartNs != 0) {
            youngConcWindow.windowNs = TimeUtil::NanoSeconds() - youngConcWindowStartNs;
        }
        VLOG(REPORT, "[GCV2][youngconc] concurrent young mark done; STW2 evacuation handoff reachable=%zu",
             youngReachableVec.size());
    }
    VLOG(REPORT,
         "[GCV2][youngconc][concwork] run=%zu conc=%d follow=%d window_ns=%llu marked_in_window=%zu "
         "closure_calls=%zu remset_slots=%zu reenters=%zu "
         "marked_at_entry=%zu reachable_total=%zu",
         minorTotalRuns + 1, 1, 1,
         static_cast<unsigned long long>(youngConcWindow.windowNs), youngConcWindow.MarkedInWindow(),
         youngConcWindow.closureCalls, youngConcWindow.remsetSlots,
         youngConcWindow.reenters, youngConcWindow.markedAtEntry, youngReachableVec.size());
    size_t liveBytes = 0;
    TenuringInputs tenuringIn;
    tenuringIn.softMaxCapacity = Heap::GetHeap().GetMaxCapacity();
    tenuringIn.youngAllocated = youngStats.candidateBytes;
    for (ZPage* region : minorCandidateRegions) {
        const size_t live = region->is_marked() ? region->live_bytes() : 0;
        liveBytes += live;
        uint32_t age = region->GetYoungAge();
        if (age >= kPageAgeCount) {
            age = untype(PageAge::survivor14);
        }
        tenuringIn.liveByAge[age] += live;
    }
    tenuringIn.youngGarbage = youngStats.candidateBytes > liveBytes ? (youngStats.candidateBytes - liveBytes) : 0;
    GCStats& gcStats = Stats();
    gcStats.youngCandidateBytes = youngStats.candidateBytes;
    gcStats.youngPromotedBytes = liveBytes;
    for (uint32_t i = 0; i < kPageAgeCount; ++i) {
        gcStats.liveByAge[i] = tenuringIn.liveByAge[i];
    }
    Heap::GetHeap().young().SelectTenuringThreshold(tenuringIn);
    {
        // minortime: ⑧ pre-evac finish (phase + weak/satb clear)
        MRT_PHASE_TIMER(ZStatPhases::PYoungPreEvacClear);
        // tracecache: PrepareTrace above switched the TRACE-phase region caches on
        // (RegionManager.h:726-727), and this is the young mark's post-trace point -- the
        // same place HeapGcState::PostTrace drains them for a major (RelocationSet.cpp:73-78).
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
            return !region->IsYoungRegion() || RegionSpace::IsMarkedObject<Generation::Young>(object);
        });
    }

    youngLiveBytes = liveBytes;
}

void ZGenerationYoung::concurrent_reset_relocation_set()
{
    reset_relocation_set();
    auto& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().ResetFlipPromotedPages();
}

void ZGenerationYoung::concurrent_select_relocation_set()
{
    select_relocation_set(YoungType() == ZYoungType::major_full_preclean);
}

void ZGenerationYoung::pause_relocate_start()
{
    VM_ZRelocateStartYoung op;
    (void)op.pause();
}

void ZGenerationYoung::concurrent_relocate()
{
    HeapGcState& collector = TheCollector();
    if (ZAbort::should_abort()) {
        return;
    }
    RegionSpace& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    size_t allocatedBefore = space.AllocatedBytes();
    // ⑥⑦⑧ inside EvacuateYoungRegions: pause relocate_start / concurrent copy / evac_finish
    // Pass STW so Phase 8 can release the world for concurrent_relocate.
    //
    // fysfixa / fysaudit D4: slot authority for remset fix = Rescan-admitted
    // consumedSlots, not the pre-rescan liveRememberedSlots ledger.
    // The pre-rescan ledger under FYS=0 held all non-weak recorded slots;
    // rescan may drop retained-dead / free-holder / bad_target
    // without consuming, yet old Evacuate still Fixed those slots → from-object
    // not in liveInfo0 → AdmitForRoute miss → ForwardObjectExclusive
    // "invalid object route" (fysfloor B10). FYS=1 masked via reachableSlots
    // filtering both live-build and Rescan. Unifying on consumed restores
    // fix-domain ⊆ mark/route-domain without widening AdmitForRoute.
    // In non-concurrent FYS, remset consume is scan_and_follow;
    // its holder coverage belongs to the mark scan.
    // Concurrent mark force-admits slots without that proof.
    const bool refFixSlotsCoveredByReachable = false;
    collector.EvacuateYoungRegions(youngReachableVec, youngConsumedSlots,
                                   refFixSlotsCoveredByReachable, youngRemsetInteriorBases,
                                   &youngStw);
    if (ZAbort::should_abort()) {
        return;
    }
    size_t allocatedAfter = space.AllocatedBytes();
    youngStats.reclaimedBytes =
        allocatedBefore > allocatedAfter ? allocatedBefore - allocatedAfter : 0;
    Heap::GetHeap().GetGCStats(ZGenerationId::young).collectedBytes = youngStats.reclaimedBytes;

    if (youngStw != nullptr) {
        youngStw.reset();
    }

    {
        MRT_PHASE_TIMER(ZStatPhases::PYoungPostEvacFinish);
        Heap::GetHeap().cross_vm().MergeResurrectExportObjects(Generation::Young);
    }
    ++minorTotalRuns;
    uint64_t pauseUs = (TimeUtil::NanoSeconds() - youngStartNs) / NS_PER_US;
    VLOG(REPORT,
         "[GCV2Minor] run=%zu fallbackFullScan=%u candidates=%zu candidateBytes=%zu liveBytes=%zu "
         "remembered=%zu reclaimedBytes=%zu pause=%zu us",
         minorTotalRuns, static_cast<unsigned>(youngFullScan),
         youngStats.candidateRegions, youngStats.candidateBytes,
         youngLiveBytes, youngLiveRememberedCount, youngStats.reclaimedBytes,
         pauseUs);
}

} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/z/zStringDedup.hpp"
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

void ZGenerationOld::process_non_strong_references()
{
    ZBreakpoint::AtAfterReferenceProcessingStarted();
    CHECK_DETAIL(Heap::GetHeap().old().is_phase_mark_complete(),
                 "non-strong references require completed old marking");
    {
        MRT_PHASE_TIMER(ZStatPhases::PIdentifyUselessExternRef);
        Heap::GetHeap().cross_vm().FindUselessExternObjects();
    }
    // Finalizable graphs were followed during mark discovery. This phase
    // only classifies the final strong/live state (zReferenceProcessor.cpp:285).
    Heap::GetHeap().GetCollector().ProcessFinalizers();
    if (Heap::GetHeap().old().WeakRootsProcessor() != nullptr) {
        Heap::GetHeap().old().WeakRootsProcessor()->process_weak_roots();
    }
    SyncRetireDead();
    StringDedup::Instance().Clean([this](BaseObject* object) {
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
        return region->IsYoungRegion() || RegionSpace::IsMarkedObject<Generation::Old>(object);
    });
    // zGeneration.cpp:1344-1373: finish in-flight weak loads before unblocking.
    ZRendezvousHandshakeClosure rendezvous;
    Handshake::execute(&rendezvous);
    ZRendezvousGCThreads gcRendezvous;
    gcRendezvous.doit();
    ZResurrection::unblock();
    Heap::GetHeap().GetFinalizerProcessor().EnqueueReferences();

#if defined(MRT_TESTABLE_INTERNALS)
    ObserveMarkClosureForTest(nullptr);
#endif
}


} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/z/zStringDedup.hpp"
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







// Registered finalizers are discovered during old root marking and fixed by
// VisitNativePointers. Only queued/running finalizables are strong mark roots.




} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/z/zStringDedup.hpp"
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
#include "Heap/z/zStat.hpp"
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
GCCycleSnapshot ZGeneration::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return { _cycle, sequence, requestIndex, reason.load(std::memory_order_relaxed), _phase, active };
}

void ZGeneration::SelectReason(GCReason value, uint64_t index)
{
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(!active);
    requestIndex = index;
    reason.store(value, std::memory_order_release);
}

void ZGeneration::Begin(uint64_t index)
{
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(!active);
    requestIndex = index;
    active = true;
}

void ZGeneration::PublishPhase(ZGenerationPhase value)
{
    set_phase(value);
}

void ZGeneration::log_phase_switch(Phase from, Phase to)
{
    const char* const str[] = {
        "Young Mark Start",
        "Young Mark End",
        "Young Relocate Start",
        "Old Mark Start",
        "Old Mark End",
        "Old Relocate Start"
    };
    size_t index = 0;
    if (is_old()) {
        index += 3;
    }
    if (to == Phase::Relocate) {
        index += 2;
    }
    if (from == Phase::Mark && to == Phase::MarkComplete) {
        index += 1;
    }
    (void)str;
    (void)index;
}

bool ZGenerationYoung::should_record_stats()
{
    return YoungType() == ZYoungType::minor || YoungType() == ZYoungType::major_partial_roots;
}

bool ZGenerationOld::should_record_stats()
{
    return true;
}

void ZGeneration::set_phase(Phase new_phase)
{
    log_phase_switch(_phase, new_phase);
    _phase = new_phase;
}

const char* ZGeneration::phase_to_string() const
{
    switch (_phase) {
        case Phase::Mark:
            return "Mark";
        case Phase::MarkComplete:
            return "MarkComplete";
        case Phase::Relocate:
            return "Relocate";
    }
    return "Unknown";
}

void ZGeneration::End()
{
    std::lock_guard<std::mutex> lock(mutex);
    active = false;
}

}

namespace MapleRuntime {
void ZGeneration::InitializeWorkers(uint32_t capacity)
{
    CHECK(workers == nullptr);
    workers = std::make_unique<ZWorkers>(_cycle, capacity, &statWorkers);
    mark->BindWorkers(workers.get());
    if (_cycle == ZGenerationId::old) {
        weakRootsProcessor = std::make_unique<ZWeakRootsProcessor>(workers.get());
    }
}

void ZGeneration::StopWorkers()
{
    mark->BindWorkers(nullptr);
    weakRootsProcessor.reset();
    workers.reset();
}
}

namespace MapleRuntime {
void HeapGcState::DoGarbageCollection(ZGenerationId generation)
{
    if (generation == ZGenerationId::young) {
        DoYoungGarbageCollection();
        return;
    }
    Heap::GetHeap().old().collect();
}

void ZGenerationOld::mark_start()
{
    Begin(Snapshot().requestIndex);
    CHECK(_cycle == ZGenerationId::old);
    CHECK(Snapshot().active);
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::Begin, mark.get());
    }
#endif
    ZGlobalsPointers::flip_old_mark_start();
    ZVerify::OnColorFlip();
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeRetire, mark.get());
    }
#endif
    auto& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    // ZGC holds the VM mark-start pause across retirement and seqnum advance
    // (zGeneration.cpp:1213-1231). Serialize the pinned publication adapter
    // explicitly because our handshake pause permits safe native threads.
    std::unique_lock<std::mutex> pinnedLock(space.GetRegionManager().PinnedAllocationMutex());
    Heap::GetHeap().object_allocator().retire_pages(kPageAgeRangeOld);
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeSequence, mark.get());
    }
#endif
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(sequence != UINT64_MAX);
        ++sequence;
    }
    pinnedLock.unlock();
    set_phase(Phase::Mark);
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeDomain, mark.get());
    }
#endif
    Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor().reset_statistics();
    Mark().BindWorkers(Workers());
    Mark().Start();
#if defined(MRT_TESTABLE_INTERNALS)
    if (HeapGcState::testMarkStartState) {
        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::Complete, mark.get());
    }
#endif
}

bool ZGenerationOld::mark_end()
{
    // ZGenerationOld::pause_mark_end / ZMark::end: a single pause attempt.
    MarkStripeSet& stripes = Mark().Stripes();
    NoteMarkTerminatePause();
    const size_t before = stripes.Population();
    const bool ended = Mark().TryEnd();
    const size_t after = stripes.Population();
    NoteMarkTerminateFlushed(after >= before ? after - before : 0);
    if (!ended) {
        NoteMarkTerminateContinue(oldMarkWorkStack.size() + stripes.Population());
        return false;
    }
    // Preserve export ownership discovery after the ordinary root closure,
    // while the mark-end pause excludes new mutator publication.
    Heap::GetHeap().cross_vm().ProcessExportRoots(oldMarkForeignRoots);
    // ZMark::mark_follow (zMark.cpp:948): after workers join, return abort
    // to the phase owner before verification or publishing mark completion.
    if (ZAbort::should_abort()) {
        return false;
    }
    MarkingStacks::VerifyAllEmpty(Mark());
    set_phase(ZGeneration::Phase::MarkComplete);
    ZVerify::AfterMark();
    ZResurrection::block();
    ReportMarkTerminateContinue();
    return true;
}

void ZGenerationOld::collect()
{
    HeapGcState& collector = TheCollector();
    ZGenerationCollectionScopeOld scope(*this);
    DriverUnlocker unlocker;
    concurrent_mark();
    abortpoint();
    while (!pause_mark_end()) {
        concurrent_mark_continue();
        abortpoint();
    }
    concurrent_mark_free();
    abortpoint();
    concurrent_process_non_strong_references();
    abortpoint();
    concurrent_reset_relocation_set();
    abortpoint();
    pause_verify();
    concurrent_select_relocation_set();
    abortpoint();
    {
        DriverLocker locker;
        concurrent_remap_young_roots();
        abortpoint();
        pause_relocate_start();
    }
    concurrent_relocate();
}

void ZGenerationOld::concurrent_mark()
{
    ZBreakpoint::AtAfterMarkingStarted();
    oldMarkWorkStack.clear();
    oldMarkForeignRoots.clear();
    WorkStack& workStack = oldMarkWorkStack;
    WorkStack& foreignStack = oldMarkForeignRoots;
    MarkingStacks::VerifyEmpty(workStack.size());
    MarkingStacks::VerifyEmpty(foreignStack.size());
    const bool concurrentStackScan = MutatorManager::ConcurrentStackScanEnabled();
    uint64_t stackScanEpoch = 0;

    // Old mark-start belongs to the preceding young pause. The old body
    // begins with concurrent roots/follow (zGeneration.cpp:1015-1020).
    // ZGC old concurrent_mark has no extra stack-scan STW (zGeneration.cpp:1015-1020).
    if (concurrentStackScan) {
        stackScanEpoch = StackWatermark::epoch_id();
    }

    {
        MRT_PHASE_TIMER(ZStatPhases::PEnumRootsUpdateOldPointersWithin);
        if (concurrentStackScan) {
            MutatorManager::Instance().VisitAllMutators([stackScanEpoch](Mutator& mutator) {
                if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                    (void)mutator.GcPhaseEnum(false, stackScanEpoch, false);
                }
                if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                    (void)mutator.GcPhaseEnum(false);
                }
#if defined(MRT_GC_UNIT_TESTS)
                NoteLargeArrayInitRootPhase(LargeArrayRootPhase::MAJOR_MARK, &mutator,
                                            mutator.GetStackWatermark().IsDone(stackScanEpoch));
#endif
            });
            TheCollector().DoEnumeration(workStack, foreignStack);
        } else {
            TheCollector().DoEnumeration(workStack, foreignStack);
        }
    }

    {
        MRT_PHASE_TIMER(ZStatPhases::PTraceLiveObjectsUpdateOldPointersInRefFields);
        reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).PrepareTrace();
#if defined(MRT_TESTABLE_INTERNALS)
        if (HeapGcState::testOldMarkStarted) HeapGcState::testOldMarkStarted();
#endif
        Mark().MarkFollow(false);
        ZBreakpoint::AtBeforeMarkingCompleted();
        if (ZAbort::should_abort()) {
            return;
        }

        MarkingStacks::VerifyEmpty(workStack.size());
        MarkingStacks::VerifyEmpty(foreignStack.size());

    }

}

bool ZGenerationOld::pause_mark_end()
{
    VM_ZMarkEndOld op;
    return op.pause();
}

void ZGenerationOld::concurrent_mark_continue()
{
    Mark().MarkFollow(false);
}
void ZGenerationOld::concurrent_mark_free() {}

void ZGenerationOld::concurrent_process_non_strong_references()
{
    process_non_strong_references();
    TheCollector().PostTrace();
}

void ZGenerationOld::concurrent_reset_relocation_set() {}

void ZGenerationOld::pause_verify()
{
    VM_ZVerifyOld op;
    (void)op.pause();
}

void ZGenerationOld::concurrent_select_relocation_set() {}

void ZGenerationOld::concurrent_remap_young_roots() {}

void ZGenerationOld::pause_relocate_start()
{
    VM_ZRelocateStartOld op;
    (void)op.pause();
    (void)TheCollector().Preforward();
}

void ZGenerationOld::concurrent_relocate()
{
    HeapGcState& collector = TheCollector();
    collector.ForwardFromSpace(ZGenerationId::old);
    reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().FinishIncompleteFromRegions(
        ZGenerationId::old);
    Heap::GetHeap().cross_vm().MergeResurrectExportObjects(Generation::Old);
    Heap::GetHeap().cross_vm().PostResolveCycleTask();
    collector.CollectSmallSpace();
}

}

namespace MapleRuntime {
void HeapGcState::PreGarbageCollection(ZGenerationId generation, bool isConcurrent, uint64_t gcIndex)
{
    const bool continuingPrelude = Heap::GetHeap().GetZGeneration(generation).Snapshot().active;
    if (!continuingPrelude) {
        Heap::GetHeap().GetZGeneration(generation).Begin(gcIndex);
    }
    ResetSkippedStackMapCounts();
    VLOG(REPORT, "Begin GC log. GCReason: %s, Current allocated %s, Current threshold %s",
         g_gcRequests[Heap::GetHeap().GetCycleSnapshot(generation).reason].name, Pretty(Heap::GetHeap().GetAllocatedSize()).Str(),
         Pretty(Heap::GetHeap().GetGCStats().GetThreshold()).Str());

    // zDriver.cpp:183,399-400: generation workers use their concurrent
    // budget for both pause and concurrent work. Parallel workers are separate.
    const int32_t threadCount = static_cast<int32_t>(GetWorkers(generation).active_workers());
    GetWorkers(generation).set_active();
    VLOG(REPORT, "GC generation active workers: %d", threadCount);

    Heap::GetHeap().GetGCStats(generation).reason = Heap::GetHeap().GetCycleSnapshot(generation).reason;
    Heap::GetHeap().GetGCStats(generation).async = (gcIndex == GCTask::ASYNC_TASK_INDEX);
    Heap::GetHeap().GetGCStats(generation).isConcurrentMark = isConcurrent;
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
void ZGeneration::SetYoungType(ZYoungType type)
{
    CHECK(_cycle == ZGenerationId::young);
    youngType.store(type, std::memory_order_release);
}

YoungTypeSetter::YoungTypeSetter(ZGeneration& cycle, ZYoungType type) : cycle(cycle)
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
void ZGeneration::SelectTenuringThreshold(const TenuringInputs& inputs)
{
    CHECK(_cycle == ZGenerationId::young);
    // zGeneration.cpp:704-715: preclean promotes all, other types compute.
    stats.tenuringThreshold = YoungType() == ZYoungType::major_full_preclean
        ? 0 : ComputeTenuringThreshold(inputs);
}

void ZGeneration::free_empty_pages(ZRelocationSetSelector* selector, int bulk)
{
    if (selector->should_free_empty_pages(bulk)) {
        selector->clear_empty_pages();
    }
}

void ZGeneration::flip_age_pages(const ZRelocationSetSelector* selector)
{
    ZWorkers* w = Workers();
    if (w == nullptr) {
        return;
    }
    ZRelocate::flip_age_pages(*w, selector->not_selected_small());
    ZRelocate::flip_age_pages(*w, selector->not_selected_medium());
    ZRelocate::flip_age_pages(*w, selector->not_selected_large());
    ZRelocate::barrier_promoted_pages(*w, _relocation_set.flip_promoted_pages(),
                                     _relocation_set.relocate_promoted_pages());
}

void ZGeneration::select_relocation_set(bool promote_all)
{
    ZRelocationSetSelector selector(FragmentationLimit());
    const ZGenerationId id = _cycle == ZGenerationId::young ? ZGenerationId::young : ZGenerationId::old;
    {
        ZGenerationPagesIterator pt_iter(&Heap::page_table(), id, nullptr);
        for (ZPage* page; pt_iter.next(&page);) {
            if (!page->is_relocatable()) {
                continue;
            }
            if (page->is_marked()) {
                selector.register_live_page(page);
            } else {
                selector.register_empty_page(page);
                pt_iter.yield([&]() { free_empty_pages(&selector, 64); });
            }
        }
        free_empty_pages(&selector, 0);
    }
    selector.select();
    selector.check_selected_relocatable();
    if (_cycle == ZGenerationId::young) {
        TenuringInputs inputs;
        inputs.promoteAll = promote_all;
        const ZRelocationSetSelectorStats st = selector.stats();
        for (PageAge age : kPageAgeRangeAll) {
            inputs.liveByAge[untype(age)] =
                st.small(age).live() + st.medium(age).live() + st.large(age).live();
        }
        SelectTenuringThreshold(inputs);
    }
    _relocation_set.install(&selector);
    if (_cycle == ZGenerationId::young) {
        ZWorkers* w = Workers();
        if (w != nullptr) {
            ZRelocate::flip_age_pages(*w, selector.not_selected_small());
            ZRelocate::flip_age_pages(*w, selector.not_selected_medium());
            ZRelocate::flip_age_pages(*w, selector.not_selected_large());
        }
    }
    ZRelocationSetIterator rs_iter(&_relocation_set);
    for (ZForwarding* forwarding; rs_iter.next(&forwarding);) {
        _forwarding_table.insert(forwarding);
    }
}
}

namespace MapleRuntime {

}

#if defined(MRT_TESTABLE_INTERNALS)
// Instantiate the product template for the policy matrix. Tests import these
// exact bodies from the DSO, as the existing page-mark tests do.
#include "Heap/z/zGeneration.inline.hpp"
namespace MapleRuntime {
#define MRT_P14A_EXPORT __attribute__((visibility("default")))
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<false, false, false, false>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<false, false, false, true>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<false, false, true, false>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<false, false, true, true>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<false, true, false, false>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<false, true, false, true>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<false, true, true, false>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<false, true, true, true>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<true, false, false, false>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<true, false, false, true>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<true, false, true, false>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<true, false, true, true>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<true, true, false, false>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<true, true, false, true>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<true, true, true, false>(zaddress);
template void MRT_P14A_EXPORT ZGeneration::MarkObjectIfActive<true, true, true, true>(zaddress);
}
#endif
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zMark.hpp"
#include "Common/PagePool.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Base/GcLog.h"
#include "Heap/z/zStat.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zDirector.hpp"
#include "Common/Runtime.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#include "schedule.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
void ReportSkippedStackMapCounts();
void HeapGcState::PostGarbageCollection(ZGenerationId generation, uint64_t gcIndex)
{
    reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).DumpRegionStats("region statistics when gc ends");
    GetWorkers(generation).set_inactive();
    ReportSkippedStackMapCounts();
    PagePool::Instance().Trim();
    (void)gcIndex;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    DumpAfterGC();
#endif
    MutatorManager::Instance().DestroyExpiredMutators();
}

void HeapGcState::ForwardFromSpace(ZGenerationId generation)
{
    ScopedEntryTrace trace("CJRT_GC_FORWARD");

    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    GCStats& stats = Heap::GetHeap().GetGCStats(generation);
    stats.liveBytesBeforeGC = space.AllocatedBytes();
    stats.fromSpaceSize = space.FromSpaceSize();
    if (generation == ZGenerationId::young) {
        space.ForwardFromSpace<Generation::Young>(GetWorkers(ZGenerationId::young));
    } else {
        space.ForwardFromSpace<Generation::Old>(GetWorkers(ZGenerationId::old));
    }

}

void HeapGcState::RefineFromSpace()
{
    GCStats& stats = Heap::GetHeap().GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    stats.smallGarbageSize = space.RefineFromSpace();
}
} // namespace MapleRuntime

namespace MapleRuntime {
// zGeneration.inline.hpp:131-140: the owning generation qualifies forwarding.
BaseObject* ZGeneration::relocate_or_remap_object(BaseObject* object)
{
    return relocate_or_remap_object(object,
        ForwardingProvenance{ForwardingHolderKind::StackSlot, this, &object});
}

BaseObject* ZGeneration::relocate_or_remap_object(BaseObject* object,
                                                const ForwardingProvenance& provenance)
{
    // Cangjie fields can also contain immortal metadata outside the managed heap.
    if (object == nullptr || !Heap::IsHeapAddress(object)) return object;
    ZForwarding* const forwarding = _forwarding_table.get(reinterpret_cast<MAddress>(object));
    if (forwarding == nullptr) return object;
    return _relocate->relocate_object(forwarding, object, provenance);
}
}
