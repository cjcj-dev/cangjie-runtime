// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_MARKING_STACKS_H
#define MRT_MARKING_STACKS_H
#include <cstddef>
#include <cstdint>
namespace MapleRuntime {
class ZMark;
namespace MarkingStacks {
enum class MarkingGeneration : uint8_t { MAJOR, YOUNG };
// zMark.cpp:104,601,982,1022-1038: an empty stack at the product boundary.
void VerifyEmpty(size_t pending);
// zMark.cpp:1022: thread-private stacks followed by shared stripes.
void VerifyAllEmpty(ZMark& domain);
}
}
#endif

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_ENGINE_H
#define MRT_MARK_ENGINE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zForwardingTable.hpp"

#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zMarkingSMR.hpp"
#include "Heap/z/zMarkTerminate.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeap.hpp"

namespace MapleRuntime {

class MarkStripeSet;
class ZWorkers;
class MarkContext;
struct ThreadLocalData;
class Mutator;

// Per-generation mark ownership (zMark.hpp:42-124, zMark.cpp:80-92).
class ZMark {
    friend class ZMarkTask;
public:
    static constexpr bool Resurrect = true;
    static constexpr bool DontResurrect = false;
    static constexpr bool GCThread = true;
    static constexpr bool AnyThread = false;
    static constexpr bool Follow = true;
    static constexpr bool DontFollow = false;
    static constexpr bool Strong = false;
    static constexpr bool Finalizable = true;

    enum class Result { Completed, Partial, Aborted };
    using Process = std::function<void(const MarkStackEntry&)>;

    explicit ZMark(size_t capacity, MarkingStacks::MarkingGeneration generation);
    template<bool resurrect, bool gcThread, bool follow, bool finalizable>
    void MarkObject(zaddress address);
    void Start();
    void PrepareWork();
    void PrepareWork(size_t nworkers);
    void ResizeWorkers(size_t nworkers);
    void FinishWork();
    void MarkFollow(bool partial = false);
    void FollowWorkComplete(bool partial);
    bool FollowWorkPartial();
    void MarkAndFollow(MarkContext& context, const MarkStackEntry& entry);
    void BindWorkers(ZWorkers* workers) { gcWorkers = workers; }
    bool PollStop();
    MarkStripeSet& Stripes() { return stripes; }
    MarkTerminate& Terminate() { return terminate; }
    MarkingSMR& Smr() { return smr; }
    MarkThreadLocalStacks& Stacks();
    size_t NWorkers() const { return nworkers; }
    size_t TargetNStripes() const { return targetNStripes; }
    bool Flush();
    bool Flush(ThreadLocalData* tls);
    static bool FlushThread(ThreadLocalData* tls);
    static bool FlushAllGenerations();
    bool FlushStacks();
    bool TryTerminateFlush();
    bool TryProactiveFlush(size_t workerId);
    bool TryEnd();
    void Free();
    MarkingStacks::MarkingGeneration Generation() const { return generation; }

    static Result FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                             MarkTerminate& terminate, size_t workerId, bool partial,
                             const Process& process, std::atomic<size_t>* stealSuccess = nullptr,
                             std::atomic<size_t>* stealFailure = nullptr, ZMark* domain = nullptr);

private:
    size_t CalculateNStripes(size_t nworkers) const;
    void EnsureWorkers(size_t nworkers);
    static bool HandshakeFlush(ZMark* domain);
    static bool FlushThreadLocal(ThreadLocalData* tls, ZMark* domain);

    MarkingSMR smr;
    MarkStripeSet stripes;
    MarkTerminate terminate;
    std::atomic<size_t> workNProactiveFlush{0};
    std::atomic<size_t> workNTerminateFlush{0};
    size_t nproactiveflush = 0;
    size_t nterminateflush = 0;
    size_t ntrycomplete = 0;
    size_t ncontinue = 0;
    size_t nworkers = 0;
    size_t targetNStripes = 0;
    MarkingStacks::MarkingGeneration generation;
    ZWorkers* gcWorkers = nullptr;
};

} // namespace MapleRuntime

#endif

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_TRACING_H
#define MRT_COLLECTOR_TRACING_H

#include <atomic>
#include <cstdint>
#include <map>

#include "Heap/z/zCollectedHeap.hpp"

#include "Heap/z/zDriver.hpp"
#include "Common/MarkWorkStack.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zMarkStackEntry.hpp"
#include "Mutator/MutatorManager.h"

// set 1 to enable concurrent mark test.
#define MRT_TEST_CONCURRENT_MARK (false)

#include "Heap/z/zRootsIterator.hpp"
namespace MapleRuntime {
class MarkLiveCache;
// number of nanoseconds in a microsecond.
constexpr uint64_t NS_PER_US = 1000;
constexpr uint64_t NS_PER_S = 1000000000;

// Strict mark-end cut shared by major mark-end and young
// FollowYoungMark. ZMark::end -> try_end (zMark.cpp:954-971) decides
// termination with mutators stopped, after ZMark::flush (zMark.cpp:587-605,
// :998-1006), and resumes concurrent follow when that cut exposes work
// (zMark.cpp:973-990). This must stay compile-time and default-on: retired-only
// sampling cannot see a mutator's non-full SATB node.

void NoteMarkTerminatePause();
void NoteMarkTerminateFlushed(size_t n);
void NoteMarkTerminateContinue(size_t stackSize);
void ReportMarkTerminateContinue();
#if defined(MRT_TESTABLE_INTERNALS)
struct MarkTerminateTestReceipt {
    size_t pauses = 0;
    size_t flushed = 0;
    size_t continues = 0;
    uint64_t maxPauseNs = 0;
    size_t pauseY2y = 0;
    size_t closureDuringPause = 0;
};
void ResetMarkTerminateTestReceipt();
MarkTerminateTestReceipt ReadMarkTerminateTestReceipt();
void NoteMarkTerminatePauseDuration(uint64_t pauseNs);
void NoteMarkTerminatePauseProducers(size_t y2y);
void NoteTraceYoungClosureDuringPause();
#endif

// prefetch distance for mark.
#define MACRO_MARK_PREFETCH_DISTANCE 16    // this macro is used for check when pre-compiling.
constexpr int MARK_PREFETCH_DISTANCE = 16; // when it is changed, remember to change MACRO_MARK_PREFETCH_DISTANCE.

// Small queue implementation, for prefetching.
#define MRT_MAX_PREFETCH_QUEUE_SIZE_LOG 5UL
#define MRT_MAX_PREFETCH_QUEUE_SIZE (1UL << MRT_MAX_PREFETCH_QUEUE_SIZE_LOG)
#if MRT_MAX_PREFETCH_QUEUE_SIZE <= MACRO_MARK_PREFETCH_DISTANCE
#error Prefetch queue size must be strictly greater than prefetch distance.
#endif



// For managing gc roots
#if defined(MRT_TESTABLE_INTERNALS)
// One-shot receipt for the actual DoGarbageCollection -> Preforward path. The
// target slot is armed by a test, but the observed word is produced and sampled
// inside the product remap loop before relocate-start changes the good masks.
// Read-only copies of the product's ownership carriers at PostTrace's handoff.
struct ExportOwnershipTestObservation {
    using Edge = std::pair<BaseObject*, BaseObject*>;
    bool afterHandoff = false;
    size_t discoveredOwners = 0;
    size_t handoffOwners = 0;
    std::vector<Edge> discovered;
    std::vector<Edge> handoff;
};

struct RemapYoungRootsTestReceipt {
    uintptr_t targetSlot = 0;
    uintptr_t before = 0;
    uintptr_t after = 0;
    uintptr_t resolvedAddress = 0;
    uint64_t visits = 0;
    uint64_t heals = 0;
    bool storeGoodAfter = false;
    uint64_t oldPendingVisits = 0;
};

// Colored fields select a physical slot; raw roots also accept a source
// address so derived temporary base slots can be observed. Zero disables it.
void ResetRemapYoungRootsTestReceipt(uintptr_t targetSlot);
RemapYoungRootsTestReceipt ReadRemapYoungRootsTestReceipt();
#endif

#if defined(MRT_TESTABLE_INTERNALS)
struct Y2yHandoffTestReceipt {
    uint64_t phase0 = 0; // release-boundary observation
    uint64_t phase1 = 0; // roots consumed after release
    uint64_t phase2 = 0; // STW2 merge
    uint64_t beforeRelease = 0;
    uint64_t afterRoot = 0;
    uint64_t afterStw2 = 0;
};
void ResetY2yHandoffTestReceipt();
Y2yHandoffTestReceipt ReadY2yHandoffTestReceipt();
void NoteY2yBeforeReleaseTestReceipt(uint64_t pending);
void NoteY2yAfterRootTestReceipt(uint64_t pending);
void NoteY2yAfterStw2TestReceipt(uint64_t pending);
void ArmY2yAfterReleaseTestReceipt(BaseObject* holder, uint64_t publications);
void PublishY2yAfterReleaseTestReceipt();
void ArmMarkBeforeMarkEndTestReceipt(Mutator* producer, BaseObject* first, BaseObject* second = nullptr);
void PublishMarkBeforeMarkEndTestReceipt();
void ArmY2yDuringConcurrentTestReceipt(BaseObject* holder);
void PublishConcurrentYoungProducersTestReceipt();
void ArmLeftoverBeforePauseTestReceipt(BaseObject* y2yHolder);
void PublishLeftoverBeforePauseTestReceipt();

struct ExportRootPublicationTestReceipt {
    uint64_t registrationsAfterT1 = 0;
    uint64_t producerFlushes = 0;
    uint64_t observedAtT2 = 0;
    U64 handle = std::numeric_limits<U64>::max();
    bool holderMarked = false;
    bool childMarked = false;
};
void ResetExportRootPublicationTestReceipt();
void ArmExportRootAfterT1TestReceipt(Mutator* producer, BaseObject* holder, BaseObject* child);
void PublishExportRootAfterT1TestReceipt();
void FlushExportRootAfterT1TestReceipt();
void NoteExportRootPublicationAtT2TestReceipt();
ExportRootPublicationTestReceipt ReadExportRootPublicationTestReceipt();
#endif

// portyoungconc: work accounting for the concurrent young mark window.
// ZGC anchor: ZGenerationYoung::concurrent_mark() = mark_roots() + mark_follow()
// (zGeneration.cpp:665-669) — everything a young collector does between
// pause_mark_start and pause_mark_end runs with mutators alive.
// Every field below counts GC work performed while the world is running. A
// Duration alone does not establish that the concurrent window performed marking work.
struct YoungConcWindowStats {
    uint64_t windowNs = 0;    // world-released → STW2 requested
    size_t closureCalls = 0;  // TraceYoungClosure invocations inside the window
    size_t markedAtEntry = 0; // reachableVec.size() at world-release
    size_t markedAtExit = 0;  // reachableVec.size() at STW2 request
    size_t remsetSlots = 0;   // remset slots consumed by the in-window rescan
    size_t reenters = 0;      // ZGC pause_mark_end() == false → concurrent_mark_continue()
    size_t MarkedInWindow() const { return markedAtExit >= markedAtEntry ? markedAtExit - markedAtEntry : 0; }
};

#if defined(MRT_TESTABLE_INTERNALS)


#endif

class ForwardTable {
public:
    explicit ForwardTable(RegionSpace& space) : theSpace(space) {}

    // if region is compacted, return false.

    template<Generation G>
    void PrepareForwardTable()
    {
        DLOG(FORWARD, "reset fwd table");
        theSpace.PrepareFromSpace<G>();
    }

    RegionSpace& theSpace;
};

using CrossRefHandler = void(*)(BaseObject*, BaseObject*);

class VM_ZMarkStartYoung;
class VM_ZMarkStartYoungAndOld;
class VM_ZMarkEndYoung;
class VM_ZRelocateStartYoung;
class VM_ZMarkEndOld;
class VM_ZRelocateStartOld;
class VM_ZVerifyOld;


class MarkingWork;
class Collector {
    friend class ZMarkTask;

public:
    const char* GetCollectorName() const;
    void RequestGC(GCReason reason, bool async);
    ZGeneration& GetZGeneration(ZGenerationId generation);
    const ZGeneration& GetZGeneration(ZGenerationId generation) const;
    ZGeneration& GetZGeneration(Generation generation)
    {
        return GetZGeneration(generation == Generation::Young ? ZGenerationId::young : ZGenerationId::old);
    }
    const ZGeneration& GetZGeneration(Generation generation) const
    {
        return GetZGeneration(generation == Generation::Young ? ZGenerationId::young : ZGenerationId::old);
    }
    GCCycleSnapshot GetCycleSnapshot(ZGenerationId generation) const
    {
        return GetZGeneration(generation).Snapshot();
    }
    void PublishGenerationPhase(ZGenerationId generation, ZGenerationPhase value);
    bool OldActiveRemsetIsCurrent() const
    {
        return GetZGeneration(ZGenerationId::old).ActiveRemsetIsCurrent(
            GetZGeneration(ZGenerationId::young).Sequence());
    }
    Generation ObjectGeneration(BaseObject* object) const;
    void MarkObjectIfActive(BaseObject* object) const;
    [[noreturn]] static void AbortUnimplemented(const char* method);
    static HandVerdict JudgeHandOutTarget(BaseObject* target);
    static uint64_t EmitNeverInstalledDiagnostic(BaseObject* target, uintptr_t rawSlotBits,
                                                 MAddress witnessStart, uint64_t witnessEpoch,
                                                 uint64_t witnessLife, bool witnessValid);
    [[noreturn]] static void FailClosedLoad(const char* site, BaseObject* target, uintptr_t slotBits,
                                            const ForwardingProvenance& provenance);
    BaseObject* ValidateCurrentValue(BaseObject* ref, const ForwardingProvenance& provenance) const;
    static constexpr size_t kHealFpBits = 1u << 22;
    static uint64_t* HealFpTable()
    {
        static uint64_t table[kHealFpBits / 64] = {};
        return table;
    }
    static size_t HealFpIndex(uintptr_t slot)
    {
        const uintptr_t h = (slot >> 3) ^ (slot >> 23) ^ (slot >> 41);
        return static_cast<size_t>(h) & (kHealFpBits - 1);
    }
    static void HealFpMark(uintptr_t slot)
    {
        const size_t i = HealFpIndex(slot);
        __atomic_fetch_or(&HealFpTable()[i / 64], uint64_t(1) << (i % 64), __ATOMIC_RELAXED);
    }
    static bool HealFpTest(uintptr_t slot)
    {
        const size_t i = HealFpIndex(slot);
        return (__atomic_load_n(&HealFpTable()[i / 64], __ATOMIC_RELAXED) & (uint64_t(1) << (i % 64))) != 0;
    }
    bool IsLoadBad(RefField<>& ref) const
    {
        return (raw(ref.GetFieldValue()) & ::g_cjLoadBadMask) != 0;
    }
    BaseObject* make_load_good(RefField<>& ref, const ForwardingProvenance& provenance) const
    {
        BaseObject* target = to_object(ref.GetTargetObject());
        if (target == nullptr || ZPointer::is_load_good(ref.GetFieldValue())) {
            return target;
        }
        return relocate_or_remap_object(target, remap_generation(ref), provenance);
    }
    BaseObject* FindLatestVersion(BaseObject* obj, const ForwardingProvenance& provenance, Generation generation) const;
    CollectorType collectorType = CollectorType::SMOOTH_COLLECTOR;

#if defined(MRT_TESTABLE_INTERNALS)
    friend struct RelocationReceiptTestAccess;
    friend struct ZGenerationRootTestAccess;
#endif
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct MarkPublicationFixture;
#endif
public:
    enum class RefSlotKind : U8 {
        STRONG,
        WEAK_REFERENT,
    };

    explicit Collector(Allocator& allocator, CollectorResources& resources);

    ~Collector() = default;
    ZMark* MajorMark() { return Heap::GetHeap().old().MarkPtr(); }
    const ZMark* MajorMark() const { return Heap::GetHeap().old().MarkPtr(); }
    void PreGarbageCollection(ZGenerationId generation, bool isConcurrent, uint64_t gcIndex);
    void PostGarbageCollection(ZGenerationId generation, uint64_t gcIndex);

    static void VisitStackRoots(const RootVisitor& visitor, RegSlotsMap& regSlotsMap, const FrameInfo& frame,
                                Mutator& mutator);
    static void Process(const RootVisitor& visitor, const DerivedPtrVisitor* derivedPtrVisitor,
                        RegSlotsMap& regSlotsMap, const FrameInfo& frame, Mutator& mutator);
    static size_t CurrentThreadRootMapMissCount();

    static void VisitHeapReferencesOnStack(const RootVisitor& rootVisitor, const DerivedPtrVisitor& derivedPtrVisitor,
                                           RegSlotsMap& regSlotsMap, const FrameInfo& frame, Mutator& mutator,
                                           bool young = false);

    static void VisitHeapReferencesOnStack(const RootVisitor& regRootVisitor, const RootVisitor& slotRootVisitor,
                                           const DerivedPtrVisitor& derivedPtrVisitor, RegSlotsMap& regSlotsMap,
                                           const FrameInfo& frame, Mutator& mutator, bool young = false);

    static void RecordStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp);
#ifdef __arm__
    static void RecordC2NStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp);
    static void RecordExclusiveStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp);
#endif
    static void RecordStubAllRegister(RegSlotsMap& regSlotsMap, Uptr fp);
#if defined(MRT_TESTABLE_INTERNALS)
    // Observers see the product result after dispatch; none supplies work.
    // Static storage keeps the instance layout identical in both build shapes.
    // Observes the post-closure slot; nullptr denotes that worker completing.
    static std::function<void(ZGenerationId, NativeSlot*)> testColoredRootResult;
    static std::function<void()> testCyclePrepared;
    static std::function<void()> testYoungMarkStarted;
    static std::function<void()> testOldMarkStarted;
    static std::function<void(ZGenerationId, MarkStartPoint, const ZMark*)> testMarkStartState;
    static std::function<void()> testYoungMarkCompleted;
    static std::function<void(const ExportOwnershipTestObservation&)> testExportOwnershipResult;
    static std::function<void(Mutator&)> testOldMarkThreadResult;
#endif

    void Init();
    void Fini();

    // zRootsIterator.cpp:159-220. The language has no weak plain code-cache roots.
    void VisitExportColoredRoots(const NativeSlotVisitor& visitor) const;
    OopStorage& StrongRootStorage() const;
    OopStorage& WeakFinalizerRootStorage() const;
    OopStorage& SyncWeakRootStorage() const;
    void VisitStaticAdapterRoots(const NativeSlotVisitor& visitor) const;
    void VisitStrongColoredRoots(const NativeSlotVisitor& visitor) const;
    void VisitWeakColoredRoots(const NativeSlotVisitor& visitor) const;
    void VisitAllColoredRoots(const NativeSlotVisitor& visitor) const;
    void VisitStrongPlainRoots(const RootVisitor& visitor,
                              const std::function<void(Mutator&)>& threadVisitor) const;

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    void DumpRoots(LogType logType);
    void DumpHeap(const CString& tag);
    void DumpBeforeGC();

    void DumpAfterGC();
#endif

    void ResurrectExportObject(BaseObject* obj)
    {
        ZGeneration* generation = ObjectGeneration(obj) == Generation::Young ?
            static_cast<ZGeneration*>(ZGeneration::young()) : static_cast<ZGeneration*>(ZGeneration::old());
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        if (generation == nullptr || !generation->is_phase_relocate()) {
            resurrectedExportObjectes.erase(obj);
            resurrectedExportObjectes.insert(ValueRoot(ResolveCurrentValueRoot(
                obj, &resurrectedExportObjectes, ObjectGeneration(obj), ForwardingStage::IncomingNew),
                ForwardingStage::IncomingNew));
        } else {
            resurrectedExportObjectesForwardPhase.erase(obj);
            resurrectedExportObjectesForwardPhase.insert(ValueRoot(
                ResolveCurrentValueRoot(
                    obj, &resurrectedExportObjectesForwardPhase, ObjectGeneration(obj), ForwardingStage::IncomingNew),
                ForwardingStage::IncomingNew));
        }
    }

    void VisitAllResurrectExportObjects(const RootVisitor& visitor)
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        for (auto& obj : resurrectedExportObjectes) {
            visitor(reinterpret_cast<ObjectRef&>(*obj));
        }
    }



    bool DiscoverReference(BaseObject* reference, ReferenceType type)
    {
        return collectorResources.GetFinalizerProcessor().GetReferenceProcessor().DiscoverReference(reference, type);
    }
    void DiscoverWeakReference(BaseObject* reference, WorkStack& workStack);

    // live but not resurrected object.  The generation is part of the predicate;
    // there is intentionally no IsMarkedObject(BaseObject*) compatibility API.
    template<Generation G>
    bool IsMarkedObject(const BaseObject* obj) const { return RegionSpace::IsMarkedObject<G>(obj); }

    // live or resurrected object.
    template<Generation G>
    inline bool IsSurvivedObject(const BaseObject* obj) const
    {
        return RegionSpace::IsMarkedObject<G>(obj) ||
            (G == Generation::Old && RegionSpace::IsResurrectedObject(obj));
    }
    void StartOldMarkWork();
    void MarkOldObjectIfActive(BaseObject* object, bool gcThread = false) const;
    // Consume one object entry. Partial arrays must be decoded before this
    // entry point; mark=false carries an already-owned accounting obligation.
    bool MarkEntryObject(BaseObject* obj, const MarkStackEntry& entry,
                                 MarkLiveCache* cache) const;
    inline bool IsResurrectedObject(const BaseObject* obj) const { return RegionSpace::IsResurrectedObject(obj); }



    Allocator& GetAllocator() const { return theAllocator; }


    MRT_EXPORT void RunGarbageCollection(uint64_t gcIndex, GCReason reason);

    GCStats& GetGCStats(ZGenerationId generation = ZGenerationId::old)
    {
        return GetZGeneration(generation).Stats();
    }

    void UpdateGCStats();


protected:
    void ForwardFromSpace(ZGenerationId generation);
    void RefineFromSpace();

    void RequestGCInternal(GCReason reason, bool async) { collectorResources.RequestGC(reason, async); }

    Allocator& theAllocator;

    // A collectorResources provides the resources that the tracing collector need,
    // such as gc thread/threadPool, gc task queue.
    // Also provides the resource access interfaces, such as invokeGC, waitGC.
    // This resource should be singleton and shared for multi-collectors
    CollectorResources& collectorResources;
    U32 snapshotFinalizerNum = 0;


    // indicate whether to fix references (including global roots and reference fields).
    // this member field is useful for optimizing concurrent copying gc.
    bool fixReferences = false;

    std::atomic<size_t> markedObjectCount = { 0 };
    std::mutex externMtx;
    // ZGC zUncoloredRoot.hpp:46-49: uncolored roots keep their color in
    // the container. A current address must not be interpreted as a from-key.
    struct ValueRoot {
        BaseObject* object;
        ForwardingStage stage;
        uintptr_t color;
        Generation generation;
        ValueRoot(BaseObject* value, ForwardingStage source = ForwardingStage::OverwritePrevious)
            : object(value), stage(source), color(::g_cjLoadGoodMask),
              generation(source == ForwardingStage::IncomingNew && Heap::IsHeapAddress(value)
                  ? Heap::page(reinterpret_cast<MAddress>(value))->GetOwnerGeneration()
                  : Generation::Old) {}
        operator BaseObject*() const { return object; }
        ForwardingStage Stage() const
        {
            // ZGC zUncoloredRoot.inline.hpp:64-65 selects the remap generation.
            // An unrelated generation flip cannot invalidate this current root.
            const uintptr_t mask = generation == Generation::Young
                ? ZPointerRemappedYoungMask
                : ZPointerRemappedOldMask;
            return (ZPointer::remap_bits(color) & mask) != 0 ? stage : ForwardingStage::OverwritePrevious;
        }
    };
    struct ValueRootHash {
        size_t operator()(const ValueRoot& root) const { return std::hash<BaseObject*>{}(root.object); }
    };
    using ValueRootSet = std::unordered_set<ValueRoot, ValueRootHash>;
    using ValueRootList = std::list<ValueRoot>;
    using ValueRootMap = std::unordered_map<ValueRoot, ValueRootList, ValueRootHash>;
    ValueRootMap discoveredExternObjects;
#if defined(MRT_TESTABLE_INTERNALS)
    void ObserveExportOwnershipForTest(bool afterHandoff);
#endif
    // Resolver callbacks may enter managed code and therefore must not own the
    // root-carrier mutex.  Keep resolver serialization separate from the mutex
    // used by GC root and preforward consumers.
    std::mutex cycleResolverMtx;
    std::mutex cycleWorkStackMtx;
    ValueRootMap cycleRefWorkStack;
    // Number of callbacks already delivered for each stable export id. A
    // resolver can be reposted when PREFORWARD is published while a managed
    // callback is running, so progress must outlive one ResolveCycleRef call.
    // Protected by cycleWorkStackMtx together with the root carrier.
    std::unordered_map<U32, size_t> cycleRefProgress;
    std::mutex resurrectExportMtx;
    ValueRootSet resurrectedExportObjectes;
    ValueRootSet resurrectedExportObjectesForwardPhase;

    // Value-only root containers have no addressable RootSlot to heal. Keep
    // their RootObligation on the existing ResolveStoreValue authority and
    // rebuild key-bearing containers while their owner lock is held.
    BaseObject* ResolveCurrentValueRoot(BaseObject* value, const void* owner, Generation generation,
                                        ForwardingStage stage = ForwardingStage::OverwritePrevious) const;
    void CurrentizeValueRootSet(ValueRootSet& roots, Generation generation) const;
    void CurrentizeValueRootMap(ValueRootMap& roots, Generation generation) const;

    inline WorkStack NewWorkStack() const
    {
        WorkStack workStack = WorkStack();
        return workStack;
    }


    // enum all common roots.
    void EnumAllCommonRoots(ZWorkers& workers);
    ZWorkers& GetWorkers(ZGenerationId generation) const
    {
        return *GetZGeneration(generation).Workers();
    }
    // enum roots referenced by foreign languages.
    void EnumAllExportRoots(RootSet& foreignRootsSet);
    // let finalizerProcessor process finalizers, and mark resurrected if in light sync gc

    void DiscoverFinalizableRoot(NativeSlot& slot) const;

    void MergeMutatorRoots(WorkStack& workStack);
    void DoEnumeration(WorkStack& workStack, WorkStack& foreignRootsSet);
    void DoTracing(WorkStack& workStack, WorkStack& foreignRootsSet);
    bool TryEndOldMark(WorkStack& workStack, WorkStack& foreignRootsSet);
    WorkStack oldMarkWorkStack;
    WorkStack oldMarkForeignRoots;
    bool FlushMarkProducers(ZMark* domain);
    void ProcessOldNonStrongReferences(WorkStack& workStack);
    void ProcessExportRoots(WorkStack& foreignRootsSet);

    // concurrent marking.
    void TracingImpl(WorkStack& workStack);

    void FindUselessExternObjects();

    // Export-root producer consumed by the old roots task (zMark.cpp
    // mark_old_roots -> MarkOldObjectIfActive).
    void VisitSurrectedExportRoots(const std::function<void(BaseObject*)>& visitor);

private:
    size_t RunMajorStripeMark(WorkStack& workStack, bool partial = false);
    void EnumMutatorRoot(ObjectPtr& obj, RootSet& rootSet) const;

    void VisitStaticRoots(const NativeSlotVisitor& visitor) const;
    void VisitFinalizerRoots(const NativeSlotVisitor& visitor) const;
    friend class ZGeneration;
    friend class ZGenerationYoung;
    friend class ZGenerationOld;
    friend class VM_ZMarkStartYoung;
    friend class VM_ZMarkStartYoungAndOld;
    friend class VM_ZMarkEndYoung;
    friend class VM_ZRelocateStartYoung;
    friend class VM_ZMarkEndOld;
    friend class VM_ZRelocateStartOld;
    friend class VM_ZVerifyOld;
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct MutatorPublishTestAccess;
    friend struct PartialArrayTestAccess;
    friend struct RelocationReceiptTestAccess;
    friend struct MarkPort203TestAccess;
    friend struct RemsetRearmTestAccess;
    friend struct LoadHealDeliveryTestAccess;
#endif

#if defined(MRT_TESTABLE_INTERNALS)
    friend struct MarkPublicationFixture;
#endif
public:
    

#if defined(MRT_GC_UNIT_TESTS)
    void SetCycleRefHandlerForTest(CrossRefHandler handler) { cycleRefHandlerForTest = handler; }

    // Controlled test wrapper for the copier route consumer. It keeps the
    // consumer's preconditions visible (heap address, relocation phase and

#endif

    void MarkNewObject(BaseObject* obj);
    void StartYoungMarkWork();
    void DrainAllocBufferMarkProducers(AllocBuffer* buffer, WorkStack& work, bool young);
    bool PublishHandshakeMarkWork(WorkStack& work, ZMark* domain);
    void PublishThreadRoot(BaseObject* object, bool young, bool follow);
    bool FlushThreadMarkProducers(ThreadLocalData* tls, ZMark* domain);
    bool FlushThreadMarkProducers(ThreadLocalData* tls);
    bool FlushGCDataMarkProducers(ThreadGCData& data, ZMark* domain);
    bool FlushGCDataMarkProducers(ThreadGCData& data);
    ZMark* YoungMark() { return Heap::GetHeap().young().MarkPtr(); }
    const ZMark* YoungMark() const { return Heap::GetHeap().young().MarkPtr(); }
    void MarkYoungObjectIfActive(BaseObject* object) const;
    void MarkYoungRootObject(BaseObject* object) const;

    bool ShouldIgnoreRequest(GCRequest& request);
    bool MarkObject(BaseObject* obj) const;
    bool ResurrectObject(BaseObject* obj, size_t offset, ZPage* regionInfo);

    void EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const;
    void TraceRefField(BaseObject* obj, RefField<>& ref, WorkStack& workStack, bool finalizable = false) const;
    void TraceObjectRefFields(BaseObject* obj, WorkStack& workStack, bool finalizable = false);
    void FollowPartialArray(const MarkStackEntry& entry, WorkStack& workStack);
    BaseObject* GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field);
    BaseObject* ForwardObject(BaseObject* fromVersion, Generation generation);
    BaseObject* ForwardObjectExclusive(BaseObject* obj);
    BaseObject* ResolveStoreValue(BaseObject* ref, const ForwardingProvenance& provenance,
                                  Generation generation) const;
    void PostResolveCycleTask();
    void PrepareCycleRef()
    {
        std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
        CurrentizeValueRootMap(cycleRefWorkStack, Generation::Old);
        CurrentizeValueRootMap(discoveredExternObjects, Generation::Old);
        for (auto& entry : discoveredExternObjects) {
            ValueRootList& destination = cycleRefWorkStack[entry.first];
            destination.splice(destination.end(), entry.second);
        }
        discoveredExternObjects.clear();
    }
    void MergeResurrectExportObjects(Generation generation)
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes, generation);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, generation);
        resurrectedExportObjectes.insert(resurrectedExportObjectesForwardPhase.begin(),
            resurrectedExportObjectesForwardPhase.end());
        resurrectedExportObjectesForwardPhase.clear();
    }
    // Phase A of the ZGC-style colouring work (ops/design/G1_WRITE_BARRIER_DESIGN.md §3.6).
    //
    // Today a reference carries no colour unless it is being evacuated, so "needs the barrier"
    // is exactly "tagged", and every consumer spells that as one of the two predicates below.
    // Those two share a blind spot: an untagged value satisfies neither, so an if/else-if chain
    // over them lets it through unexamined. That is correct while good == 0 and wrong the moment
    // a good colour is non-zero, which is what phase C does.
    //
    // IsLoadBad is declared on Collector (Collector.h) so the six phase barriers, which hold a
    // Collector&, can spell it. Phase C changes that one body -- as in ZGC's
    // ZPointer::is_load_bad, zAddress.inline.hpp:626-628 -- instead of ~90 call sites.

    // note this api is not atomic, caller should take care of this.
    // Stale remap colour (ZGC: the value itself says it may be stale). No pointer tagID.
    bool IsOldPointer(RefField<>& ref) const { return IsLoadBad(ref); }

    // note this api is not atomic, caller should take care of this.
    // Current colour: has the remap bit being handed out now. Plain (no colour) is neither.
    bool IsCurrentPointer(RefField<>& ref) const { return ZPointer::is_load_good(ref.GetFieldValue()); }

    // OpenJDK ZPointer::is_young_load_good/is_old_load_good
    // (zAddress.inline.hpp:648-655): the conceptual generation epoch is represented by the two
    // accepted bits in that generation's mask.


    // OpenJDK ZBarrier::remap_generation (zBarrier.inline.hpp:110-137): one generation-good
    // bit identifies the other generation; a double-bad colour consults the forwarding side table.
    ZGenerationId remap_generation(RefField<>& ref) const
    {
        CHECK_DETAIL(!ZPointer::is_load_good(ref.GetFieldValue()), "load-good reference does not need remap");
        if (ZPointer::is_old_load_good(ref.GetFieldValue())) return ZGenerationId::young;
        if (ZPointer::is_young_load_good(ref.GetFieldValue())) return ZGenerationId::old;
        // zBarrier.inline.hpp:124-136: the remembered bits disambiguate old
        // heap fields; otherwise test the young generation's forwarding map.
        if ((raw(ref.GetFieldValue()) & ZPointerRememberedMask) == ZPointerRememberedMask) {
            return ZGenerationId::old;
        }
        const MAddress address = raw(ref.GetTargetObject());
        if (address == 0) {
            return ZGenerationId::old;
        }
        if (Heap::GetHeap().GetZGeneration(Generation::Young).forwarding_table().get(address) != nullptr) {
            return ZGenerationId::young;
        }
        return ZGenerationId::old;
    }

    // OpenJDK ZGeneration::relocate_or_remap_object (zGeneration.inline.hpp:131-140): an address
    // outside the selected generation's forwarding table is already safe; a matching entry routes
    // to the current object. The generation check prevents an address-reuse alias from selecting a
    // route installed by the other generation.
    //
    // ZRelocate::relocate_object (zRelocate.cpp:382-410) has three exits only:
    //   ① find() hit → to    ② retain+copy → to    ③ wait, then forward_object
    // forward_object (zRelocate.cpp:412-416) asserts find()!=null. There is no
    // "return from". non-heap / no-ghost are "not in this forwarding table"
    // (zGeneration.inline.hpp:131-140), not a fourth relocate exit.
    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId generation) const
    {
        return relocate_or_remap_object(
            obj, generation,
            ForwardingProvenance{ ForwardingHolderKind::StackSlot, this, &obj });
    }

    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId generation,
                                         const ForwardingProvenance& provenance) const
    {
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) return obj;
        const MAddress from = reinterpret_cast<MAddress>(obj);
        const Generation ownerGeneration = generation == ZGenerationId::young
            ? Generation::Young : Generation::Old;
        ZForwarding* forwarding = Heap::GetHeap().GetZGeneration(ownerGeneration).forwarding_table().get(from);
        if (forwarding == nullptr) return obj;

        // zRelocate.cpp:383-415: lookup, retain/copy/release, then wait/find.
        // Every leg carries the same set-owned forwarding, including after
        // the source page has been released and its descriptor reused.
        if (const MAddress to = forwarding->find(from)) {
            return reinterpret_cast<BaseObject*>(to);
        }
        ZPage::RetainScope lease{forwarding};
        if (lease.ok()) {
            if (BaseObject* to = TryMutatorRelocate(obj, lease)) return to;
        }
        lease.Release();
        BaseObject* to = WaitForPageForwarding(obj, lease.HoldForwarding());
        if (to == nullptr) {
            FailClosedLoad("ZRelocate::forward_object requires a forwarding entry", obj, 0, provenance);
        }
        return to;
    }

    void AddRawPointerObject(BaseObject* obj)
    {
        (void)PinRawPointerObject(obj);
        // ⚠ Callers of this void form (Sync futures/mutexes) keep using the pointer they
        // passed in. If that pointer was a movable from-copy resolved above, their later
        // RemoveRawPointerObject would Dec the from region — same pairing hazard as
        // oracleblack face c. Sync objects are pinned at creation (never from) today;
        // adopting the resolved pointer there is a tracked follow-up, not done here.
    }

    BaseObject* PinRawPointerObject(BaseObject* obj)
    {
        // oracle R4 / RegionManager.h:507: do not pin a movable from-copy
        // during PREFORWARD/FORWARD (CHECK would fire). Resolve to `to` first;
        // a true VisitLive hole is already Exempt-kept, TryDeleteRegion(FROM)
        // fails and the else arm is taken. CHECK is not relaxed.
        //
        // oracleblack round 10, face c: the resolved pointer MUST flow back to the
        // caller. Inc lands on region(to); MCC_ReleaseRawData Decs the region of the
        // payload pointer the caller kept. Pinning to while handing out from both
        // underflowed the from region's count and gave C a payload the young cycle
        // was about to relocate.
        if (obj != nullptr && Heap::IsHeapAddress(obj)) {
            const MAddress addr = reinterpret_cast<MAddress>(obj);
            if (GetZGeneration(Generation::Young).forwarding_table().get(addr) != nullptr ||
                GetZGeneration(Generation::Old).forwarding_table().get(addr) != nullptr) {
                const ForwardingProvenance provenance{
                    ForwardingHolderKind::HeapRef, this, &obj
                };
                obj = ValidateCurrentValue(obj, provenance);
            }
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        space.AddRawPointerObject(obj);
        return obj;
    }

    void RemoveRawPointerObject(BaseObject* obj)
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        space.RemoveRawPointerObject(obj);
    }

    void ResolveCycleRef();

    // BaseObject* ForwardFixRefField(RefField<>& field) const;
    // lonefrom: "is this object being relocated in this cycle" must not be asked as
    // "is its region still typed FROM_REGION".  ForwardFromRegions takes each region off the
    // from-list with TakeHeadRegion() (RegionManager.cpp:1638), so a
    // region is retyped the moment relocation of it starts.  IsFromRegion() tests FROM_REGION
    // alone -- IsLoneFromRegion() is a separate predicate -- so for the whole window in which a
    // region is actually being evacuated, its objects answer "not from".
    //
    // What that produces: an object in a LONE_FROM region that has not been copied yet answers
    // false to all three staleness authorities (IsFromObject, IsGhostFromObject, IsForwarded --
    // the last only becomes true after the copy), so GetAndTryTagRefField paints it the *current*
    // remap colour.  The slot is then load-good; the object is copied a moment later; and the read
    // barrier's fast path hands the from-version straight to the mutator, which reads its header
    // as one 64-bit word and gets ObjectState::FORWARDED in bits 48-49.
    //
    // afterFlip=1 (slot colour == current good colour, i.e. painted after the relocate-start flip),
    // slotGood=1, hasTo=1 (a to-version exists), unmov=0 -- and unmov=0 is itself explained here,
    // since IsUnmovableFromObject covers UNMOVABLE_FROM/RAW_POINTER_PINNED and not LONE_FROM.
    //
    // OpenJDK never asks a page-type enum this question.  Relocation-set membership is decided once
    // when the set is installed (zGeneration.cpp:254) and answered per address through
    // ZForwardingTable, so it cannot change under a concurrent reader the way a region type does.
    static constexpr bool kLoneFromIsFrom = true;
    mutable std::atomic<uint64_t> loneFromHits{ 0 };

    // PORT_ZFORWARDING step 2: membership answered by address, the way ZGC answers it
    // (ZGeneration::relocate_or_remap_object -> _forwarding_table.get(addr)).  Step 1 established
    // the two answers are equivalent: 1.68e8 comparisons across 10 runs, tableOnly=0 legacyOnly=0.
    //
    // The old path stays as the control arm.  What changes is *which* answer is authoritative: a
    // region type is rewritten as relocation progresses, and a predicate reading it can be right
    // one instant and wrong the next -- that shape produced several of this session's dead ends.
    // An address either is in the set or is not.
    static constexpr bool kMembershipFromTable = true;

    bool IsFromObject(BaseObject* obj) const
    {
        if (kMembershipFromTable) {
            if (!Heap::IsHeapAddress(obj)) {
                return false;
            }
            const MAddress addr = reinterpret_cast<MAddress>(obj);
            return GetZGeneration(Generation::Young).forwarding_table().get(addr) != nullptr ||
                   GetZGeneration(Generation::Old).forwarding_table().get(addr) != nullptr;
        }
        // filter const string object.
        if (Heap::IsHeapAddress(obj)) {
            auto regionInfo = Heap::page(reinterpret_cast<uintptr_t>(obj));
            if (kLoneFromIsFrom && regionInfo->IsLoneFromRegion()) {
                // Arm self-check: a null result from this change is only readable if the branch is
                // known to fire.  Powers of two, so a hot predicate cannot flood the log.
                const uint64_t n = loneFromHits.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((n & (n - 1)) == 0) {
                    LOG(RTLOG_ERROR, "[LONEFROM] trigger n=%lu", n);
                }
                return true;
            }
            return regionInfo->IsFromRegion();
        }

        return false;
    }

    bool IsGhostFromObject(BaseObject* obj) const
    {
        return IsFromObject(obj);
    }

    bool IsUnmovableFromObject(BaseObject* obj) const;

    // Refuses a non-heap address the way FindToVersion does below, and for the same reason:
    // GetGhostFromRegionAt -> GetUnitIdxAt has no heap range
    //
    // Old-tagged fields are exactly where non-heap payloads appear -- a TypeInfo*, a binary
    // constant, immortal metadata: after Flip their colour is IsOldPointer while the payload is
    // still the live non-heap pointer. Every
    // caller's *load-good* arm gated on Heap::IsHeapAddress before looking the route up; none of
    // the *old-tag* arms did.  Guarding here rather than at each arm is a shared guard:
    //
    //   ResolveMinorReference(RefField&) ra1=ResolveMinorReference    (where it moved to)
    //   ResolveMinorReference(RootSlot&) same shape
    //
    // Reproduced 10/10 with cjcj::cjc --package packages/basic/src --output-type=staticlib on a
    // coloured host runtime:
    //   F GetUnitIdxAt OOB addr=0x6282f2cd8c40 heap=[0x719247600000, 0x719257600000)
    // 0x6282f2... is the compiler's own image, the same range as start_ip in that run's stack-map
    // lines.  Gating only the first site moved the abort to the second, which is what showed the
    // population was the old-tag paths rather than one call site.
    FindToVersionResult FindToVersion(BaseObject* obj, Generation generation) const
    {
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
            return FindToVersionResult::NotManaged();
        }
        const MAddress to = forwarding_find(generation, reinterpret_cast<MAddress>(obj));
        return to != 0 ? FindToVersionResult::Found(reinterpret_cast<BaseObject*>(to))
                       : FindToVersionResult::NotForwarded();
    }

protected:
    void CheckStoreGoodTarget(const char* consumer, BaseObject* target,
                              const ForwardingProvenance& provenance) const;
    // zRelocate.cpp:354-379 relocate_object_inner: find hit → return; else
    // alloc (or reuse a prepared dest) → copy → insert; CAS loser uses winner.
    BaseObject* RelocateObjectInner(BaseObject* obj, ZPage* copyPage);
    void UpdateRemsetForFields(BaseObject* from, BaseObject* to);

    // portmutreloc: ZRelocate::relocate_object's middle leg (zRelocate.cpp:391-406) --
    // retain the from-region, relocate the object on this thread, release. Returns the
    // to-version, or nullptr when the owning copier must supply the receipt.
    BaseObject* TryMutatorRelocate(BaseObject* from, ZPage::RetainScope& lease) const;

    bool TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const;

    bool TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const;
    bool TryUpdateRefFieldWithProvenance(BaseObject* obj, RefField<>& field, BaseObject*& newRef,
                                         const ForwardingProvenance& provenance) const;
    bool TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const;

    // ── store value side: typed, mirroring ZGC ────────────────────────────────────────────
    //
    // ZGC colours only a zaddress -- ZAddress::color(zaddress addr, uintptr_t color) -- and the
    // sole producer of a zaddress is ZPointer::uncolor, which asserts
    //     is_load_good(ptr) || is_null_any(ptr)   "Should be load good when handed out"
    // (zAddress.inline.hpp:609-614).  A from-version address therefore cannot reach a store: the
    // type system rejects it, with no runtime check on the hot path.
    //
    // Our slots were typed by ad5c28b3 (HeapSlot/RootSlot/DerivedSlot) but the *value* side was
    // not: everything took a bare BaseObject*, which means both "the live to-version" and "some
    // managed pointer I happen to hold".  That is the same conflation MAddress = Uptr had, one
    // level up, and it let the colouring code paint a from-version with the current remap colour.
    // Measured, N=5 on cjcj::cjc --package packages/basic/src: every run installs load-good slots
    // naming FORWARDED targets (>=1, >=16, >=16, >=16, >=32 by the powers-of-two sampler), 21 of
    //
    // So the colouring code is now reachable only through this typed pair.  To paint the current
    // colour a caller must hold a zaddress, and the only producer is ClassifyStoreValue below,
    // which carries the proof.  "Paint a from-version store-good" no longer compiles.

    // Sole colouring site for a value already proven to be the live to-version.
    // Store-good colour: mark-good | current Remembered (OpenJDK zAddress.cpp:83).
    // Produce the load-bad member of the full-colour family without changing
    // the address.  This is deliberately separate from GetAndTryTagRefField:
    // unclassified store values still have to pass ResolveStoreValue first.
    RefField<> ColourStaleLoadBad(zaddress_unsafe stale) const
    {
        const Uptr notCurrent = ZPointerRemappedMask ^ ZPointerRemapped;
        const Uptr staleOneHot = notCurrent & -notCurrent;
        const Uptr storeColour = ZPointer::remap_bits(staleOneHot) | ZPointerMarkedYoung | ZPointerMarkedOld | ZPointerRemembered;
        return RefField<>(from_region_addr(raw(stale)), storeColour);
    }

    // The ONE adapter from a raw managed pointer to the typed pair.  This is where the proof that
    // ZPointer::uncolor asserts gets actually established, instead of being assumed by
    // ColourTypes.h's from_object ("凭什么: a live BaseObject* in hand"), which checks nothing.
    //
    // ⭐ The authority is the object's own state word, not the region's type.  IsFromObject /
    // IsGhostFromObject ask the *region* whether it is from-space, and a region type is a moving
    // property: once the cycle retires the region becomes TO/RECENT_FULL while the from-version
    // still sits in it with FORWARDED in its header (StateWord.h:22-30 FORWARDED = 3;
    // StateWord.h:174-178 puts stateCode at bits 48-49, which is why a stale value read with the
    // compiler's bare `mov (%rbx),%rdi` faults non-canonically as #GP rather than as a page
    // fault).  Same defect shape as the remset condition fixed in abe3c4d8 -- a predicate testing
    // a property that moves instead of the object's own authoritative state.  ZGC never asks the
    // page either: is_load_good compares the pointer's colour to the global remap colour, and
    // forwarding identity is a per-address ZForwarding lookup.
    // ZGC resolves a field word exactly once.  ZBarrier::make_load_good is the resolve
    // (zBarrier.inline.hpp:294-343); the colouring step that follows it is a pure recolour --
    // ZPointer::uncolor / ZAddress::store_good rebuild the word from the address they were
    // handed and never consult the forwarding table (zAddress.inline.hpp:609-624,806-811).
    //
    // GetAndTryTagRefField resolves again, and under in-place compaction that second resolve is
    // not idempotent: from- and to-addresses share one page span, so a destination this page
    // already produced is itself a from-index of the same table and the lookup shifts it a
    // second time.  Measured on NW256, one 512-element reference array, 3/3: 383 elements
    // rewritten from the correct current address to that address minus the page's own
    // compaction delta, every one of them by this step, with the load-good funnel taking no
    // remap exit at all.
    //
    // The three checks are kept, and they now carry the invariant instead of a resolve: the
    // address handed to the colour producer is already resolved, and an unresolved one stops
    // here rather than being laundered into a store-good word.
    RefField<> ColourResolvedRefField(BaseObject* target,
                                      const ForwardingProvenance& provenance = {}) const
    {
        if (target == nullptr) {
            return RefField<>(static_cast<BaseObject*>(nullptr));
        }
        if (!Heap::IsHeapAddress(target)) {
            return RefField<>(target);
        }
        CheckStoreGoodTarget("ColourResolvedRefField", target, provenance);
        if (kColourWhoProbe) {
            NoteStoreGoodOnBadTarget(target);
        }
        return RefField<>(ZAddress::store_good(from_object(target)));
    }

    RefField<> GetAndTryTagRefField(BaseObject* target) const
    {
        const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, target, &target };
        return GetAndTryTagRefFieldWithProvenance(target, provenance);
    }

    RefField<> GetAndTryTagRefFieldWithProvenance(BaseObject* target,
                                                  const ForwardingProvenance& provenance) const
    {
        // Null carries no colour (ZGC zAddress: null is never load-bad).
        if (target == nullptr) {
            return RefField<>(static_cast<BaseObject*>(nullptr));
        }
        // TypeInfo* / binary constants / immortal metadata are not relocated,
        // but a non-null HeapSlot word is still coloured.  The load-good mask
        // fast path peels it without routing through the collector.
        if (!Heap::IsHeapAddress(target)) {
            return RefField<>(ZAddress::store_good(from_object(target)));
        }
        // ZPointer::uncolor is the sole producer accepted by ZAddress::store_good
        // (zAddress.inline.hpp:609-624,806-811). ResolveStoreValue is our
        // make-load-good producer: a relocation-set address is looked up or copied
        // by this thread; an unresolved address never reaches colouring.
        target = ValidateCurrentValue(target, provenance);
        CHECK_DETAIL(target != nullptr && Heap::IsHeapAddress(target),
                     "store-good requires a resolved heap address");
        CheckStoreGoodTarget("GetAndTryTagRefField", target, provenance);
        // colourwho: installed-slot checking sits after Barrier::WriteReference, so it only sees the
        // mutator store path.  That path now measures ~0 while the read barrier still hands out
        // load-good slots naming from-versions, which means the writer is on the *collector* side --
        // preforward/ref_fix/self-heal all colour through here too.  This is the single funnel for
        // every coloured value in the runtime, so the count belongs here.
        //
        // Fires when we are about to paint the current (load-good) colour on a target whose own
        // header already says FORWARDED, or whose header is zeroed.  Both are the crash families.
        if (kColourWhoProbe) {
            NoteStoreGoodOnBadTarget(target);
        }
        return RefField<>(ZAddress::store_good(from_object(target)));
    }

    // holdermark: probe-only view of the old-generation mark bit.
    bool IsMarkedObjectForProbe(BaseObject* obj) const
    {
        return IsMarkedObject<Generation::Old>(obj);
    }

    // flipwitness: the probe needs the colour currently handed out, without reaching into members.
    Uptr CurrentRemapColourForProbe() const { return ZPointerRemapped; }

    // colourwho: compile-time gated -- this is the funnel every coloured write goes through.
    static constexpr bool kColourWhoProbe = true;

    void NoteStoreGoodOnBadTarget(BaseObject* target) const
    {
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            return;
        }
        const uint64_t hdr = __atomic_load_n(reinterpret_cast<const uint64_t*>(target), __ATOMIC_RELAXED);
        const unsigned stateCode = static_cast<unsigned>((hdr >> 48) & 0x3u);
        const uint64_t typeInfo = hdr & 0xffffffffffffull;
        const uint64_t seen = colourWhoTotal.fetch_add(1, std::memory_order_relaxed) + 1;
        if (seen == 1) {
            // Positive control: a zero below must not be readable as a dead probe.
            LOG(RTLOG_ERROR, "[COLOURWHO] armed first sc=%u", stateCode);
        }
        if (stateCode == 0 && typeInfo != 0) {
            return;
        }
        const uint64_t bad = colourWhoBad.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((bad & (bad - 1)) != 0) {
            return;
        }
        LOG(RTLOG_ERROR, "[COLOURWHO] bad=%lu of %lu target=%p sc=%u typeInfo=0x%lx isFrom=%d isGhost=%d phase=%d",
            bad, seen, static_cast<void*>(target), stateCode, typeInfo, IsFromObject(target) ? 1 : 0,
            IsGhostFromObject(target) ? 1 : 0,
            ZGeneration::old() != nullptr ? static_cast<int>(ZGeneration::old()->Snapshot().phase) : -1);
    }
    mutable std::atomic<uint64_t> colourWhoTotal{ 0 };
    mutable std::atomic<uint64_t> colourWhoBad{ 0 };

    // A store value is stale if *either* authority says so: the region it sits in is from-space,
    // or the object's own state word says it has been forwarded.  Region type moves; the state
    // word does not, so asking only the region is what let load-good slots name FORWARDED targets.
    // ZBarrier::is_good_or_null_fast_path keeps an already-remapped value out
    // of the from-side slow path (zBarrier.inline.hpp:294-343).  The equivalent
    // invariant here is content plus current relocation-set membership: a
    // usable managed object outside every current from range is already the
    // address to hand out.  In particular, a retired carrier may still cover
    // that numerical address; consulting FindToVersion would reinterpret the
    // current to-version as a historical from-version.  Do not turn a lookup
    // miss into success here: current from-range members remain on the normal
    // receipt/relocate path and therefore retain fail-closed handling.
    bool IsAlreadyToStoreValue(BaseObject* target, Generation generation) const
    {
        return target != nullptr && Heap::IsHeapAddress(target) &&
            Collector::JudgeHandOutTarget(target) == HandVerdict::Usable &&
            generation_forwarding_table(generation).get(reinterpret_cast<MAddress>(target)) == nullptr;
    }



    // Typed, unconditional boundary: HeapSlot write-back is coloured while
    // stack/register/static RootSlot write-back stays plain, matching ZGC's
    // ZUncoloredRoot.  There is no runtime switch between the two contracts.
    RefField<> RootSlotWriteback(BaseObject* target, const RefField<>& /*slot*/) const
    {
        return GetAndTryTagRefField(target);
    }

    void CollectLargeGarbage()
    {
        MRT_PHASE_TIMER(ZStatPhases::PCollectLargeGarbage);
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        GCStats& stats = GetGCStats();
        stats.largeSpaceSize = space.LargeObjectBytes();
        stats.largeGarbageSize = space.CollectLargeGarbage();
        stats.collectedBytes += stats.largeGarbageSize;
    }

    void CollectPinnedGarbage()
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        GCStats& stats = GetGCStats();
        stats.pinnedSpaceSize = space.PinnedSpaceSize();
        stats.pinnedGarbageSize = space.CollectPinnedGarbage();
        stats.collectedBytes += stats.pinnedGarbageSize;
    }

    void CollectSmallSpace();

    void DoGarbageCollection(ZGenerationId generation);
    void ProcessFinalizers();
    void EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet, Generation generation) const;

private:
    using MinorObjectSet = std::unordered_set<BaseObject*>;
    using MinorRegionSet = std::unordered_set<ZPage*>;
    using MinorSlotSet = std::unordered_set<MAddress>;
    using MinorInteriorBaseMap = std::unordered_map<MAddress, BaseObject*>;

    bool MarkObjectImpl(BaseObject* obj, bool youngClaim, MarkLiveCache* liveCache = nullptr) const;
    bool MarkYoungObject(BaseObject* obj, MarkLiveCache* liveCache = nullptr) const
    {
        return MarkObjectImpl(obj, true, liveCache);
    }

    // Large reference-array chunking, ported from ZGC's ZMark (zMark.cpp:185-263).
    // `holder` is the owning array, carried only for diagnostics; it is nullptr
    // when the elements arrive as a chunk popped off the work stack, because
    // ZGC's partial-array entry does not carry the holder either.
    void PushPartialArray(RefField<>* addr, size_t length, WorkStack& workStack, bool finalizable = false) const;
    void FollowArrayElementsSmall(BaseObject* holder, RefField<>* addr, size_t length,
                                  WorkStack& workStack, bool finalizable) const;
    void FollowArrayElementsLarge(BaseObject* holder, RefField<>* addr, size_t length,
                                  WorkStack& workStack, bool finalizable) const;
    void FollowArrayElements(BaseObject* holder, RefField<>* addr, size_t length,
                             WorkStack& workStack, bool finalizable) const;

    bool CasInstallResolvedTarget(RefField<>& field, MAddress expected, zaddress target,
                                  bool allowNull = false) const;
    BaseObject* ResolveMinorReference(RefField<>& field,
                                     const ScopedStopTheWorld* stw = nullptr) const;
    BaseObject* ResolveMinorReference(RootSlot& root,
                                     const ScopedStopTheWorld* stw = nullptr) const;
    void VisitMinorRootSlots(RootVisitor& rawRootVisitor, RootVisitor& invisibleRootVisitor,
                             uint64_t stackScanEpoch = 0);
    void VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor);
    void VisitMinorRoots(const std::function<void(BaseObject*)>& visitor,
                         const std::function<void(BaseObject*)>& invisibleVisitor,
                         uint64_t stackScanEpoch = 0);
    // origin tags root source for invalid-minor-root diagnosis (gcbadroot).
    void PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin = "unknown") const;
    void PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin, bool finalizable) const;
    // setbitmap O1③: claim young via MarkObject (region mark bitmap) + collect vector;
    // FYS=0 skips reachableSlots inserts (slots never looked up). Object claims use the bitmap.
    void TraceYoungClosure(WorkStack& workStack, bool fullYoungScan,
                           std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                           MinorSlotSet& weakSlots,
                           const MinorSlotSet* reachableSlotDomain = nullptr);
    void TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,
                                  std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                  MinorSlotSet& weakSlots,
                                  const MinorSlotSet* reachableSlotDomain = nullptr);
    // Follow this generation's published mark work through TraceYoungClosure.
    bool FollowYoungMark(WorkStack& workStack, bool fullYoungScan,
                             std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                             MinorSlotSet& weakSlots,
                             YoungConcWindowStats* windowStats = nullptr);
    // ZMark::try_end sibling: called with mutators stopped; performs exactly one
    // local-buffer flush and reports whether concurrent-mark-continue is needed.
    bool TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats = nullptr);
    friend class ZMarkTask;
    bool FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase = nullptr,
                               const ScopedStopTheWorld* stw = nullptr) const;
    bool FixMinorEvacuatedSlot(RootSlot& root, const ScopedStopTheWorld* stw = nullptr) const;
    bool FixMinorEvacuatedSlot(DerivedSlot& derived, BaseObject* knownBase = nullptr,
                               const ScopedStopTheWorld* stw = nullptr) const;
    void FixMinorRootSlots(const ScopedStopTheWorld* stw = nullptr);
    void FixMinorObjectSlots(BaseObject* object, const ScopedStopTheWorld* stw = nullptr);
    // stw: live handle lets relocate follow ZGC Phase 7/8 (zGeneration.cpp:573-580):
    // pause = flip + phase + root fix; concurrent = ForwardFromSpace; re-STW = heap
    // slot catch-up + evac_finish. nullptr keeps the whole evacuate under the caller STW.
    void EvacuateYoungRegions(const std::vector<BaseObject*>& reachableVec, const MinorSlotSet& rememberedSlots, bool refFixSlotsCoveredByReachable,
                              const MinorInteriorBaseMap& interiorBases,
                              std::unique_ptr<ScopedStopTheWorld>* stw = nullptr);
    // Report-only: find young objs full-reachable but unmarked; attribute via remset MISSING.
    // Gated by MRT_GCMARKGAP_PROBE=1 (default off).
    void DoYoungGarbageCollection();
    void RunYoungCollection();
    void ConcurrentYoungMark();
    bool YoungMarkEndPause();
    void ConcurrentYoungMarkContinue();
    void FinishYoungMarkHandoff();
    void RunOldCollection();
    // After nested young, remaining young survivors hold young→old edges the
    // young closure skipped. ZGC overlapping mark paints old targets from those
    // stores (zBarrier.inline.hpp:742-749). Seed them into the old TRACE stack.
    void FlushAllocationRegions();
    template<bool forward>
    bool TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& ref, BaseObject*& oldRef, BaseObject*& newRef,
                               const ForwardingProvenance& provenance) const;
    void TraceHeap();
    void PostTrace();
    // OpenJDK ZGenerationOld::remap_young_roots (zGeneration.cpp:1503-1523):
    // before old relocate-start flip, remap young roots + remset so none carry
    // two remap-bit errors.
    void RemapYoungRoots();
    bool Preforward();
    void StartRelocationTasks(ZGenerationId generation);
    BaseObject* WaitForPageForwarding(BaseObject* obj, ZForwarding* owner) const;
    void PreforwardDiscoveredExternObjects(Generation generation);
    void PreforwardAllResurrectExportFromObjects(Generation generation);
    CrossRefHandler GetCrossRefHandler(BaseObject* foreignProxy);
#if defined(MRT_GC_UNIT_TESTS)
    CrossRefHandler cycleRefHandlerForTest = nullptr;
#endif

    ForwardTable fwdTable;
    // gc index 0 or 1 is used to distinguish previous gc and current gc.
    uint64_t minorTotalRuns = 0;
    MinorRegionSet minorCandidateRegions;
    std::unique_ptr<ScopedStopTheWorld> youngStw;
    std::vector<BaseObject*> youngReachableVec;
    MinorSlotSet youngConsumedSlots;
    MinorInteriorBaseMap youngRemsetInteriorBases;
    YoungCollectionStats youngStats;
    uint64_t youngStartNs = 0;
    size_t youngLiveBytes = 0;
    size_t youngLiveRememberedCount = 0;
    bool youngFullScan = false;
    WorkStack youngWorkStack;
    uint64_t youngStackScanEpoch = 0;
    YoungConcWindowStats youngConcWindow;
    uint64_t youngConcWindowStartNs = 0;
    MinorSlotSet youngWeakSlots;

};
} // namespace MapleRuntime
#endif // MRT_COLLECTOR_TRACING_H
