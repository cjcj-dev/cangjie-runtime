// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.inline.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zUncoloredRoot.hpp"
#include "Mutator/Mutator.inline.h"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zBreakpoint.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
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
// ZGC zGeneration.cpp:161-163.
void ZGeneration::mark_flush(ThreadGCData& data)
{
    mark->Flush(data);
}


// ZGC zGeneration.cpp:69-76: one generation timer per young collection type.
static const ZStatPhaseGeneration ZPhaseGenerationYoung[] {
    {"Young Generation", ZGenerationId::young},
    {"Young Generation (Promote All)", ZGenerationId::young},
    {"Young Generation (Collect Roots)", ZGenerationId::young},
    {"Young Generation", ZGenerationId::young}
};
static const ZStatPhaseGeneration ZPhaseGenerationOld("Old Generation", ZGenerationId::old);

// ZGC zGeneration.cpp:78-98: phase identity is selected at the VM operation or concurrent entry.
static const ZStatPhasePause ZPhasePauseMarkStartYoung("Pause Mark Start", ZGenerationId::young);
static const ZStatPhasePause ZPhasePauseMarkStartYoungAndOld("Pause Mark Start (Major)", ZGenerationId::young);
static const ZStatPhasePause ZPhasePauseMarkEndYoung("Pause Mark End", ZGenerationId::young);
static const ZStatPhasePause ZPhasePauseRelocateStartYoung("Pause Relocate Start", ZGenerationId::young);
static const ZStatPhasePause ZPhasePauseMarkEndOld("Pause Mark End", ZGenerationId::old);
static const ZStatPhasePause ZPhasePauseRelocateStartOld("Pause Relocate Start", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkYoung("Concurrent Mark", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkContinueYoung("Concurrent Mark Continue", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkFreeYoung("Concurrent Mark Free", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentResetRelocationSetYoung("Concurrent Reset Relocation Set", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentSelectRelocationSetYoung("Concurrent Select Relocation Set", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentRelocateYoung("Concurrent Relocate", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkOld("Concurrent Mark", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkContinueOld("Concurrent Mark Continue", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkFreeOld("Concurrent Mark Free", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentResetRelocationSetOld("Concurrent Reset Relocation Set", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentSelectRelocationSetOld("Concurrent Select Relocation Set", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentRelocateOld("Concurrent Relocate", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentProcessNonStrongOld("Concurrent Process Non-Strong", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentRemapRootsOld("Concurrent Remap Roots", ZGenerationId::old);

static const ZStatSubPhase ZSubPhaseConcurrentMarkFollowYoung("Concurrent Mark Follow", ZGenerationId::young);
static const ZStatSubPhase ZSubPhaseConcurrentMarkRootsYoung("Concurrent Mark Roots", ZGenerationId::young);
static const ZStatSubPhase ZSubPhaseConcurrentMarkRootsOld("Concurrent Mark Roots", ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentMarkFollowOld("Concurrent Mark Follow", ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentRemapRootsColoredOld("Concurrent Remap Roots Colored", ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentRemapRootsUncoloredOld("Concurrent Remap Roots Uncolored", ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentRemapRememberedOld("Concurrent Remap Remembered", ZGenerationId::old);
ZGenerationYoung* ZGeneration::_young = nullptr;
ZGenerationOld* ZGeneration::_old = nullptr;

ZGenerationYoung::ZGenerationYoung(ZPageTable* page_table, const ZForwardingTable* old_forwarding_table,
                                     RegionManager* page_allocator)
    : ZGeneration(ZGenerationId::young), _remembered(page_table, old_forwarding_table, page_allocator)
{
    _young = this;
}
ZGenerationOld::ZGenerationOld() : ZGeneration(ZGenerationId::old)
{
    _old = this;
}

ZGenerationId ZGeneration::id() const { return _id; }

ZGenerationIdOptional ZGeneration::id_optional() const
{
    return static_cast<ZGenerationIdOptional>(id());
}

bool ZGeneration::is_young() const { return id() == ZGenerationId::young; }
bool ZGeneration::is_old() const { return id() == ZGenerationId::old; }
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
      statHeap(),
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
    return fragmentation_limit(_id);
}



// ZGenerationOld::relocate_start (zGeneration.cpp:1379-1397) captures the
// young sequence once for the whole old relocation, not once per forwarding.
void ZGeneration::RecordYoungSequenceAtRelocateStart(uint64_t youngSequence)
{
    CHECK(_id == ZGenerationId::old);
    youngSequenceAtRelocateStart.store(youngSequence, std::memory_order_release);
}

bool ZGeneration::ActiveRemsetIsCurrent(uint64_t youngSequence) const
{
    CHECK(_id == ZGenerationId::old);
    // zGeneration.inline.hpp:174-182: each young mark start flips the faces.
    return ((youngSequence - youngSequenceAtRelocateStart.load(std::memory_order_acquire)) & 1U) == 0;
}




// ZGeneration::mark_object, zGeneration.inline.hpp:119-123.



class VM_ZOperation {
public:
    virtual ~VM_ZOperation() = default;
    virtual bool do_operation() = 0;
    virtual const char* name() const = 0;
    virtual bool block_jni_critical() const { return false; }
    bool pause()
    {
        if (block_jni_critical()) {
            ZJNICritical::block();
        }
        bool success = false;
        {
            ScopedStopTheWorld stw(name(), false);
            // zGeneration.cpp:432-452: skip_thread_oop_barriers, then verify.
            // Frame slots are healed on safepoint exit (Mutator.h:175), not here.
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
    const char* name() const override { return ZPhasePauseMarkStartYoung.Name(); }
    bool do_operation() override
    {
        ZStatTimerYoung timer(ZPhasePauseMarkStartYoung);
        Heap::GetHeap().increment_total_collections();
        ZGeneration::young()->mark_start();
        return true;
    }
    bool block_jni_critical() const override { return true; }
};

class VM_ZMarkStartYoungAndOld : public VM_ZOperation {
public:
    const char* name() const override { return ZPhasePauseMarkStartYoungAndOld.Name(); }
    bool do_operation() override
    {
        ZStatTimerYoung timer(ZPhasePauseMarkStartYoungAndOld);
        Heap::GetHeap().increment_total_collections();
        ZGeneration::young()->mark_start();
        ZGeneration::old()->mark_start();
        return true;
    }
    bool block_jni_critical() const override { return true; }
};

class VM_ZMarkEndYoung : public VM_ZOperation {
public:
    const char* name() const override { return ZPhasePauseMarkEndYoung.Name(); }
    bool do_operation() override
    {
        ZStatTimerYoung timer(ZPhasePauseMarkEndYoung);
        return ZGeneration::young()->mark_end();
    }
};

class VM_ZRelocateStartYoung : public VM_ZOperation {
public:
    const char* name() const override { return ZPhasePauseRelocateStartYoung.Name(); }
    bool do_operation() override
    {
        ZStatTimerYoung timer(ZPhasePauseRelocateStartYoung);
        ZGeneration::young()->relocate_start();
        return true;
    }
    bool block_jni_critical() const override { return true; }
};

class VM_ZMarkEndOld : public VM_ZOperation {
public:
    const char* name() const override { return ZPhasePauseMarkEndOld.Name(); }
    bool do_operation() override
    {
        ZStatTimerOld timer(ZPhasePauseMarkEndOld);
        return ZGeneration::old()->mark_end();
    }
};

class VM_ZRelocateStartOld : public VM_ZOperation {
public:
    const char* name() const override { return ZPhasePauseRelocateStartOld.Name(); }
    bool do_operation() override
    {
        ZStatTimerOld timer(ZPhasePauseRelocateStartOld);
        ZGlobalsPointers::flip_old_relocate_start();
        ZVerify::OnColorFlip();
        ZGeneration::old()->set_phase(ZGeneration::Phase::Relocate);
        ZGeneration::old()->RecordYoungSequenceAtRelocateStart(ZGeneration::young()->Sequence());
        ZGeneration::old()->StatHeap()->AtRelocateStart(
            static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().Stats(
                ZGeneration::old()));
        ZRelocate::StartRelocationTasks(ZGenerationId::old);
        return true;
    }
    bool block_jni_critical() const override { return true; }
};

class VM_ZVerifyOld : public VM_ZOperation {
public:
    const char* name() const override { return "Verify Old"; }
    bool do_operation() override
    {
        ZVerify::AfterWeakProcessing();
        return true;
    }
};



void ZGeneration::at_collection_start(void* timer)
{
    set_gc_timer(timer);
    CycleStats().AtStart(TimeUtil::NanoSeconds());
    // zGeneration.cpp:380-385: the heap account opens at collection start.
    statHeap.AtCollectionStart(
        static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().Stats(this));
    Workers()->set_active();
}

void ZGeneration::at_collection_end()
{
    Workers()->set_inactive();
    // ZGC zStat.cpp:1247: warmup accounting reads the major driver cause.
    const GCReason reason = ZDriver::major()->gc_cause();
    CycleStats().AtEnd(TimeUtil::NanoSeconds(), StatWorkers(),
                       reason == GC_REASON_WARMUP, should_record_stats());
    set_gc_timer(nullptr);
}

// ZGC zGeneration.cpp:514-532: collection state belongs to the scope.
class ZGenerationCollectionScopeYoung {
private:
    YoungTypeSetter _type_setter;
    ZStatTimer _stat_timer;

public:
    ZGenerationCollectionScopeYoung(ZYoungType type, void* gc_timer)
        : _type_setter(*ZGeneration::young(), type),
          _stat_timer(ZPhaseGenerationYoung[static_cast<int>(type)])
    {
        ZGeneration::young()->at_collection_start(gc_timer);
    }

    ~ZGenerationCollectionScopeYoung()
    {
        ZGeneration::young()->at_collection_end();
    }
};

void ZGenerationYoung::collect(ZYoungType type, void* timer)
{
    ZGenerationCollectionScopeYoung scope(type, timer);
    pause_mark_start();
    // ZGC zGeneration.cpp:538-576: young keeps the driver lock throughout;
    // only the old collection scope releases it (zGeneration.cpp:995).
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
    CHECK(_id == ZGenerationId::young);
    ZGlobalsPointers::flip_young_mark_start();
    ZVerify::OnColorFlip();

    auto& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    auto& manager = space.GetRegionManager();
    {
        manager.ResetTLABUsage();
        Heap::GetHeap().object_allocator().retire_pages(kPageAgeRangeYoung);
    }
    reset_statistics();
    _seqnum++;
    set_phase(Phase::Mark);
    Mark().BindWorkers(Workers());
    Mark().Start();
    {
        Heap::GetHeap().remembered().flip();
    }

    // zGeneration.cpp:880-885: mark-start sample (also resets the
    // collection's used high/low trackers, zPageAllocator.cpp:1332-1346).
    statHeap.AtMarkStart(
        static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().UpdateAndStats(this));

    youngStartNs = start;
}

void ZGenerationYoung::produceYoungRoots()
{
    ZStatTimerYoung timer(ZSubPhaseConcurrentMarkRootsYoung);
    // ZGC zMark.cpp:932-936: the root task publishes its worker stacks.
    ZMark::VisitMinorRoots([](BaseObject*) {}, [](BaseObject*) {});
}

void ZGenerationYoung::mark_follow()
{
    // ZGC zGeneration.cpp:891-895: scan remembered slots and follow root
    // work in the same worker task, including on mark-end continuation.
    ZStatTimerYoung timer(ZSubPhaseConcurrentMarkFollowYoung);
    _remembered.scan_and_follow(MarkPtr());
}

void ZGenerationYoung::concurrent_mark()
{
    ZStatTimerYoung timer(ZPhaseConcurrentMarkYoung);
    youngFullScan = false;
    // ZGC zGeneration.cpp:665-669: roots, then combined scan and follow.
    produceYoungRoots();
    mark_follow();
}

bool ZGenerationYoung::mark_end()
{
    WorkStack& workStack = youngWorkStack;
    const bool markEndSucceeded = ZMark::TryEndYoungMark(workStack);
    if (markEndSucceeded) {
        Heap::GetHeap().young().set_phase(ZGeneration::Phase::MarkComplete);
        // zGeneration.cpp:906-911: mark-end sample.
        statHeap.AtMarkEnd(
            static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().Stats(this));
        return true;
    }

    return false;
}

void ZGenerationYoung::concurrent_mark_continue()
{
    ZStatTimerYoung timer(ZPhaseConcurrentMarkContinueYoung);
    // ZGC zGeneration.cpp:689-692 uses the same combined follow path.
    mark_follow();
}

void ZGeneration::mark_free()
{
    Mark().Free();
}

void ZGenerationYoung::concurrent_mark_free()
{
    ZStatTimerYoung timer(ZPhaseConcurrentMarkFreeYoung);
    mark_free();
    if (ZAbort::should_abort()) {
        return;
    }
    // Cangjie String values use an explicitly populated dedup table. Clean its
    // weak entries here; ZGC processes weak OopStorage entries through
    // ZWeakRootsProcessor (zWeakRootsProcessor.cpp:55-74).
    StringDedup::Instance().Clean([this](BaseObject* object) {
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
        return !region->IsYoungRegion() || RegionSpace::IsMarkedObject<Generation::Young>(object);
    });
}

void ZGenerationYoung::concurrent_reset_relocation_set()
{
    ZStatTimerYoung timer(ZPhaseConcurrentResetRelocationSetYoung);
    reset_relocation_set();
    auto& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().ResetFlipPromotedPages();
}

void ZGenerationYoung::concurrent_select_relocation_set()
{
    ZStatTimerYoung timer(ZPhaseConcurrentSelectRelocationSetYoung);
    select_relocation_set(YoungType() == ZYoungType::major_full_preclean);
}

void ZGenerationYoung::pause_relocate_start()
{
    VM_ZRelocateStartYoung op;
    (void)op.pause();
}

// ZGC zGeneration.cpp:650-654.
void ZGenerationYoung::flip_relocate_start()
{
    ZGlobalsPointers::flip_young_relocate_start();
    ZVerify::OnColorFlip();
}

// ZGC zGeneration.cpp:918-931: publish the phase and activate its queue
// before the relocate-start pause releases mutators.
void ZGenerationYoung::relocate_start()
{
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "Should be at safepoint");
    flip_relocate_start();
    set_phase(Phase::Relocate);
    StatHeap()->AtRelocateStart(
        static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().Stats(this));
    ZRelocate::StartRelocationTasks(ZGenerationId::young);
}

void ZGenerationYoung::concurrent_relocate()
{
    ZStatTimerYoung timer(ZPhaseConcurrentRelocateYoung);
    // ZGC zGeneration.cpp:575-580: after relocate-start every selected page
    // must finish relocation, including when shutdown requests an abort.
    RegionSpace& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    size_t allocatedBefore = space.AllocatedBytes();
    EvacuateYoungRegions();
    size_t allocatedAfter = space.AllocatedBytes();
    const size_t reclaimedBytes =
        allocatedBefore > allocatedAfter ? allocatedBefore - allocatedAfter : 0;
    ZGeneration::young()->increase_freed(reclaimedBytes);

    {
        Heap::GetHeap().cross_vm().MergeResurrectExportObjects(Generation::Young);
    }
    ++minorTotalRuns;
    uint64_t pauseUs = (TimeUtil::NanoSeconds() - youngStartNs) / NS_PER_US;
    VLOG(REPORT,
         "[GCV2Minor] run=%zu fallbackFullScan=%u liveBytes=%zu "
         "reclaimedBytes=%zu pause=%zu us",
         minorTotalRuns, static_cast<unsigned>(youngFullScan),
         statHeap.LiveAtMarkEnd(), reclaimedBytes,
         pauseUs);
    statHeap.AtRelocateEnd(space.GetRegionManager().Stats(this), should_record_stats());
}

} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
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
    void do_thread(Mutator*) override {}
};

void ZGenerationOld::process_non_strong_references()
{
    ZBreakpoint::AtAfterReferenceProcessingStarted();
    CHECK_DETAIL(Heap::GetHeap().old().is_phase_mark_complete(),
                 "non-strong references require completed old marking");
    {
        Heap::GetHeap().cross_vm().FindUselessExternObjects(discoveredExternObjects);
    }
    // Finalizable graphs were followed during mark discovery. This phase
    // only classifies the final strong/live state (zReferenceProcessor.cpp:285).
    ZMark::ProcessFinalizers();
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
    {
        // ZGC zGeneration.cpp:1323-1327 runs this as a concurrent VM op.
        // The host VM-operation lock is shared with StopTheWorld, so two
        // operations cannot simultaneously synchronize the suspendible set.
        ScopedSTWLock vmOperation;
        ZRendezvousGCThreads gcRendezvous;
        gcRendezvous.doit();
    }
    ZResurrection::unblock();
    Heap::GetHeap().GetFinalizerProcessor().EnqueueReferences();
    PostTrace();
    // ZGC zGeneration.cpp:1361-1372: publish non-strong work after
    // resurrection is unblocked, before resetting the relocation set.
    Heap::GetHeap().cross_vm().PostResolveCycleTask();
}


} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
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
#include "Heap/shared/stringdedup/stringDedup.hpp"
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


}

namespace MapleRuntime {
void ZGeneration::InitializeWorkers(uint32_t capacity)
{
    CHECK(workers == nullptr);
    workers = std::make_unique<ZWorkers>(_id, capacity, &statWorkers);
    mark->BindWorkers(workers.get());
    if (_id == ZGenerationId::old) {
        weakRootsProcessor = std::make_unique<ZWeakRootsProcessor>(workers.get());
        Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor().set_workers(workers.get());
    }
}

// ZGC zGeneration.cpp:153-155.
void ZGeneration::set_active_workers(uint32_t nworkers)
{
    Workers()->set_active_workers(nworkers);
}

void ZGeneration::StopWorkers()
{
    mark->BindWorkers(nullptr);
    weakRootsProcessor.reset();
    workers.reset();
}
}

namespace MapleRuntime {


// ZGC zGeneration.inline.hpp:170-172. The native finalizer owner stores
// the old generation's existing reference processor.
ReferenceDiscoverer* ZGenerationOld::reference_discoverer()
{
    return &Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor();
}

// ZGC zGeneration.cpp:1296-1302: policy access belongs to the old generation.
void ZGenerationOld::set_soft_reference_policy(bool clear)
{
    Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor().set_soft_reference_policy(clear);
}

bool ZGenerationOld::uses_clear_all_soft_reference_policy() const
{
    return Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor()
        .uses_clear_all_soft_reference_policy();
}

void ZGenerationOld::mark_start()
{
    // zGeneration.cpp:1248
    _total_collections_at_start = Heap::GetHeap().total_collections();
    CHECK(_id == ZGenerationId::old);
    ZGlobalsPointers::flip_old_mark_start();
    ZVerify::OnColorFlip();
    Heap::GetHeap().object_allocator().retire_pages(kPageAgeRangeOld);
    reset_statistics();
    _seqnum++;
    set_phase(Phase::Mark);
    // zReferenceProcessor.cpp:347-359, zGeneration.cpp:1228: owner-cycle reset.
    CHECK(discoveredExternObjects.empty());
    Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor().reset_statistics();
    Mark().BindWorkers(Workers());
    Mark().Start();
    // zGeneration.cpp:1238-1242: old mark-start sample.
    statHeap.AtMarkStart(
        static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().UpdateAndStats(this));
}

bool ZGenerationOld::mark_end()
{
    // ZGenerationOld::pause_mark_end / ZMark::end: a single pause attempt.

    const bool ended = Mark().TryEnd();

    if (!ended) {

        return false;
    }
    // Preserve export ownership discovery after the ordinary root closure,
    // while the mark-end pause excludes new mutator publication.
    Heap::GetHeap().cross_vm().ProcessExportRoots(oldExportOwners, discoveredExternObjects);
    // ZMark::mark_follow (zMark.cpp:948): after workers join, return abort
    // to the phase owner before verification or publishing mark completion.
    if (ZAbort::should_abort()) {
        return false;
    }

    set_phase(ZGeneration::Phase::MarkComplete);
    // zGeneration.cpp:1275-1278: old mark-end sample.
    statHeap.AtMarkEnd(
        static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().Stats(this));
    ZVerify::AfterMark();
    ZResurrection::block();

    return true;
}

// ZGC zGeneration.cpp:993-1010: the destructor body deactivates workers
// before the unlocker member reacquires the driver lock.
class ZGenerationCollectionScopeOld {
private:
    ZStatTimer _stat_timer;
    DriverUnlocker _unlocker;

public:
    ZGenerationCollectionScopeOld(void* gc_timer)
        : _stat_timer(ZPhaseGenerationOld), _unlocker()
    {
        ZGeneration::old()->at_collection_start(gc_timer);
    }

    ~ZGenerationCollectionScopeOld()
    {
        ZGeneration::old()->at_collection_end();
    }
};

void ZGenerationOld::collect(void* timer)
{
    ZGenerationCollectionScopeOld scope(timer);
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

void ZGenerationOld::mark_roots()
{
    ZStatTimerOld timer(ZSubPhaseConcurrentMarkRootsOld);
    oldMarkWorkStack.clear();
    oldExportOwners.clear();
    ZMark::DoEnumeration(oldMarkWorkStack, oldExportOwners);
}

void ZGenerationOld::mark_follow()
{
    ZStatTimerOld timer(ZSubPhaseConcurrentMarkFollowOld);
    Mark().MarkFollow(false);
}

void ZGenerationOld::concurrent_mark()
{
    ZStatTimerOld timer(ZPhaseConcurrentMarkOld);
    ZBreakpoint::AtAfterMarkingStarted();
    mark_roots();
    mark_follow();
    ZBreakpoint::AtBeforeMarkingCompleted();
}

bool ZGenerationOld::pause_mark_end()
{
    VM_ZMarkEndOld op;
    return op.pause();
}

void ZGenerationOld::concurrent_mark_continue()
{
    ZStatTimerOld timer(ZPhaseConcurrentMarkContinueOld);
    mark_follow();
}
void ZGenerationOld::concurrent_mark_free()
{
    ZStatTimerOld timer(ZPhaseConcurrentMarkFreeOld);
    mark_free();
}

void ZGenerationOld::concurrent_process_non_strong_references()
{
    ZStatTimerOld timer(ZPhaseConcurrentProcessNonStrongOld);
    process_non_strong_references();
}

void ZGenerationOld::concurrent_reset_relocation_set()
{
    ZStatTimerOld timer(ZPhaseConcurrentResetRelocationSetOld);
    reset_relocation_set();
}

void ZGenerationOld::pause_verify()
{
    // ZGC zGeneration.cpp:1155-1168: exclude young collections while verifying
    // old fields, so store barrier buffer lookup cannot race with base pointer installation.
    if (ZVerifyRoots || ZVerifyObjects) {
        DriverLocker locker;
        VM_ZVerifyOld op;
        (void)op.pause();
    }
}

void ZGenerationOld::concurrent_select_relocation_set()
{
    ZStatTimerOld timer(ZPhaseConcurrentSelectRelocationSetOld);
    select_relocation_set(false);
}

// zGeneration.cpp:1470-1527: shared iterators, one worker task, then restore
// the old generation's active worker budget. Native stack/record expansion is
// the Cangjie adapter for the ZGC uncolored-root closure.
class ZRemapYoungRootsTask final : public ZTask {
    ZRemsetTableIterator remset;
    RootsIteratorAllColored colored;
    RootsIteratorAllUncolored uncolored;
public:
    explicit ZRemapYoungRootsTask(unsigned workers)
        : ZTask("ZRemapYoungRootsTask"), remset(&Heap::GetHeap().remembered(), false),
          colored(workers, ZGenerationIdOptional::old), uncolored(ZGenerationIdOptional::old) {}
    void work() override
    {
        {
            ZStatTimerWorker timer(ZSubPhaseConcurrentRemapRootsColoredOld);
            colored.Apply([](NativeSlot& root) { (void)to_object(ZBarrier::load_barrier_on_oop_field(reinterpret_cast<volatile zpointer*>(&(root)))); });
        }
        {
            ZStatTimerWorker timer(ZSubPhaseConcurrentRemapRootsUncoloredOld);
            uncolored.Apply([] { Runtime::Current().GetConcurrencyModel().VisitGCRoots(); });
            uncolored.ApplyThreads([&](Mutator& mutator) {
                // ZGC ZRemapThreadClosure (zGeneration.cpp:1419-1424).
                StackWatermarkSet::finish_processing(mutator);
            });
        }
        {
            ZStatTimerWorker timer(ZSubPhaseConcurrentRemapRememberedOld);
            Heap::GetHeap().remembered().remap_current(&remset);
        }
    }
};

void ZGenerationOld::remap_young_roots()
{
    ZWorkers& workers = *Workers();
    const uint32_t previous = workers.active_workers();
    const uint32_t requested = std::min(std::max(Heap::GetHeap().young().Workers()->active_workers() + previous,
                                                    uint32_t{1}), ZOldGCThreads);
    workers.set_active_workers(requested);
    SuspendibleThreadSetJoiner joiner;
    ZRemapYoungRootsTask task(workers.active_workers());
    workers.run(&task);
    workers.set_active_workers(previous);
}

void ZGenerationOld::concurrent_remap_young_roots()
{
    ZStatTimerOld timer(ZPhaseConcurrentRemapRootsOld);
    remap_young_roots();
}

void ZGenerationOld::pause_relocate_start()
{
    VM_ZRelocateStartOld op;
    (void)op.pause();
}

void ZGenerationOld::concurrent_relocate()
{
    ZStatTimerOld timer(ZPhaseConcurrentRelocateOld);
    relocate().relocate(&relocation_set());
    Heap::GetHeap().cross_vm().MergeResurrectExportObjects(Generation::Old);
    statHeap.AtRelocateEnd(
        static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager().Stats(this),
        should_record_stats());
}

}

namespace MapleRuntime {

}

namespace MapleRuntime {
void ZGeneration::SetYoungType(ZYoungType type)
{
    CHECK(_id == ZGenerationId::young);
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
void ZGenerationYoung::flip_promote(ZPage* from_page, ZPage* to_page)
{
    // zGeneration.cpp:941-948: replace + statistics only. The from_page's
    // role handoff happens at the flip fork (zRelocate.cpp
    // ZFlipAgePagesTask::work) before this call, not here.
    Heap::page_table().replace(from_page, to_page);
    Heap::GetHeap().page_allocator().promote_used(from_page, to_page);
    increase_freed(from_page->size());
    increase_promoted(from_page->live_bytes());
}

void ZGenerationYoung::in_place_relocate_promote(ZPage* from_page, ZPage* to_page)
{
    // zGeneration.cpp:950-955: replace + statistics only.
    Heap::page_table().replace(from_page, to_page);
    Heap::GetHeap().page_allocator().promote_used(from_page, to_page);
}

void ZGenerationYoung::register_in_place_relocate_promoted(ZPage* page)
{
    _relocation_set.register_in_place_relocate_promoted(page);
}

void ZGenerationYoung::register_flip_promoted(const ZArray<ZPage*>& pages)
{
    _relocation_set.register_flip_promoted(pages);
}

void ZGenerationYoung::SelectTenuringThreshold(const TenuringInputs& inputs)
{
    // ZGC zGeneration.cpp:704-715: promote-all precedes an explicit override.
    if (inputs.promoteAll) {
        _tenuring_threshold = 0;
    } else if (ZTenuringThreshold != -1) {
        _tenuring_threshold = static_cast<uint32_t>(ZTenuringThreshold);
    } else {
        _tenuring_threshold = ComputeTenuringThreshold(inputs);
    }
}

void ZGeneration::free_empty_pages(ZRelocationSetSelector* selector, int bulk)
{
    if (selector->should_free_empty_pages(bulk)) {
        const size_t freed = Heap::free_empty_pages(id(), selector->empty_pages());
        increase_freed(freed);
        selector->clear_empty_pages();
    }
}

void ZGeneration::flip_age_pages(const ZRelocationSetSelector* selector)
{
    ZWorkers* w = Workers();
    ZRelocate::flip_age_pages(*w, selector->not_selected_small());
    ZRelocate::flip_age_pages(*w, selector->not_selected_medium());
    ZRelocate::flip_age_pages(*w, selector->not_selected_large());
    // ZGC zGeneration.cpp:185-192: finish compiled stores that omitted barriers
    // before making every promoted reference field store-good.
    ZRendezvousHandshakeClosure rendezvous;
    Handshake::execute(&rendezvous);
    ZRelocate::barrier_promoted_pages(*w, _relocation_set.flip_promoted_pages(),
                                     _relocation_set.relocate_promoted_pages());
}

void ZGeneration::select_relocation_set(bool promote_all)
{
    ZRelocationSetSelector selector(FragmentationLimit());
    const ZGenerationId id = _id == ZGenerationId::young ? ZGenerationId::young : ZGenerationId::old;
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
    if (_id == ZGenerationId::young) {
        TenuringInputs inputs;
        inputs.promoteAll = promote_all;
        inputs.youngGarbage = statHeap.GarbageAtMarkEnd();
        inputs.youngAllocated = statHeap.AllocatedAtMarkEnd();
        inputs.softMaxCapacity = Heap::GetHeap().soft_max_capacity();
        const ZRelocationSetSelectorStats st = selector.stats();
        for (PageAge age : kPageAgeRangeAll) {
            inputs.liveByAge[untype(age)] =
                st.small(age).live() + st.medium(age).live() + st.large(age).live();
        }
        ZGeneration::young()->SelectTenuringThreshold(inputs);
    }
    _relocation_set.install(&selector);
    if (_id == ZGenerationId::young) {
        flip_age_pages(&selector);
    }
    ZRelocationSetIterator rs_iter(&_relocation_set);
    for (ZForwarding* forwarding; rs_iter.next(&forwarding);) {
        forwarding->page()->SetRegionRole(ZPageRole::From);
        _forwarding_table.insert(forwarding);
    }
    // ZGC zGeneration.cpp:268-269: publish after installing the set/table.
    statRelocation.AtSelectRelocationSet(selector.stats());
    statHeap.AtSelectRelocationSet(selector.stats());
}
}

namespace MapleRuntime {

}

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






} // namespace MapleRuntime

namespace MapleRuntime {
// zGeneration.inline.hpp:131-140: the owning generation qualifies forwarding.
// ZGC zGeneration.inline.hpp:142-151: remapping never starts relocation.
BaseObject* ZGeneration::remap_object(BaseObject* object)
{
    ZForwarding* const forwarding = _forwarding_table.get(reinterpret_cast<MAddress>(object));
    if (forwarding == nullptr) { return object; }
    return _relocate->forward_object(forwarding, object);
}

BaseObject* ZGeneration::relocate_or_remap_object(BaseObject* object)
{
    // Cangjie fields can also contain immortal metadata outside the managed heap.
    if (object == nullptr || !Heap::IsHeapAddress(object)) return object;
    ZForwarding* const forwarding = _forwarding_table.get(reinterpret_cast<MAddress>(object));
    if (forwarding == nullptr) return object;
    return _relocate->relocate_object(forwarding, object);
}
}

namespace MapleRuntime {
void ZGenerationYoung::EvacuateYoungRegions()
{
    RegionManager& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    ZWorkers& workers = *Workers();
    {
        VLOG(REPORT, "[GCV2][relocate][conc] concurrent_relocate start flip=1");
        relocate().relocate(&relocation_set());
    }

    // zRelocate.cpp:1289-1306: finish relocation before walking flip-promoted pages.
    // Keep forwarding entries available until every field has been remapped.
    {
        manager.RememberFlipPromotedPages(workers);

    }
    {
        // zGeneration.cpp:563: keep this set until the next young mark-end reset.
        // zRelocate.cpp:1289-1310: completeness is workers()->run(relocation_set).
    }
}
}

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVerify.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
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
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zRelocate.hpp"



namespace MapleRuntime {
// ZGC zGeneration.cpp:287-293.
void ZGeneration::synchronize_relocation() { relocate().synchronize(); }
void ZGeneration::desynchronize_relocation() { relocate().desynchronize(); }
} // namespace MapleRuntime
