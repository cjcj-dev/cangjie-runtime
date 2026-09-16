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
#include <memory>
#include <mutex>
#include <vector>

#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zMarkingSMR.hpp"
#include "Heap/z/zMarkTerminate.hpp"

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
    void MarkAndFollow(MarkContext& context, const MarkStackEntry& entry);
    void BindWorkers(ZWorkers* workers) { gcWorkers = workers; }
    void BindAbort(ZAbort* token) { abortToken = token; }
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
    ZAbort* abortToken = nullptr;
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

struct WeakDiscoveryTestReceipt {
    size_t discovered = 0;
};
void ResetWeakDiscoveryTestReceipt();
WeakDiscoveryTestReceipt ReadWeakDiscoveryTestReceipt();
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

class MarkingWork;
class CopyCollector : public Collector {
    friend class ZMarkTask;
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct RelocationReceiptTestAccess;
    friend struct GenerationCycleRootTestAccess;
#endif
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct MarkPublicationFixture;
#endif
public:
    enum class RefSlotKind : U8 {
        STRONG,
        WEAK_REFERENT,
    };

    explicit CopyCollector(Allocator& allocator, CollectorResources& resources);

    ~CopyCollector() override = default;
    ZMark* MajorMark() { return oldCycle.MarkPtr(); }
    const ZMark* MajorMark() const { return oldCycle.MarkPtr(); }
    virtual void PreGarbageCollection(GCCycleGeneration generation, bool isConcurrent, uint64_t gcIndex);
    virtual void PostGarbageCollection(GCCycleGeneration generation, uint64_t gcIndex);

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
    static std::function<void(GCCycleGeneration, NativeSlot*)> testColoredRootResult;
    static std::function<void()> testCyclePrepared;
    static std::function<void()> testYoungMarkStarted;
    static std::function<void()> testOldMarkStarted;
    static std::function<void(GCCycleGeneration, MarkStartPoint, const ZMark*)> testMarkStartState;
    static std::function<void()> testYoungMarkCompleted;
    static std::function<void(const ExportOwnershipTestObservation&)> testExportOwnershipResult;
    static std::function<void(Mutator&)> testOldMarkThreadResult;
#endif

    void Init() override;
    void Fini() override;

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
        auto phase = GetGCPhase(static_cast<GCCycleGeneration>(ObjectGeneration(obj)));
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
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

    bool ShouldIgnoreRequest(GCRequest& request) override { return request.ShouldBeIgnored(); }

    ReferenceStatus DiscoverReference(BaseObject* reference, ReferenceType type)
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
    void MarkOldObjectIfActive(BaseObject* object, bool gcThread = false) const override;
    virtual bool MarkObject(BaseObject* obj) const;

    // Consume one object entry. Partial arrays must be decoded before this
    // entry point; mark=false carries an already-owned accounting obligation.
    bool MarkEntryObject(BaseObject* obj, const MarkStackEntry& entry,
                                 MarkLiveCache* cache) const;

    virtual void EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const {};
    virtual void TraceObjectRefFields(BaseObject* obj, WorkStack& workStack, bool finalizable = false)
    {
        Collector::AbortUnimplemented("CopyCollector::TraceObjectRefFields");
    }
    // Follow one partial-array chunk popped off the work stack. Ported from
    // ZGC's ZMark::follow_partial_array (zMark.cpp:265-270). Only reachable
    // for typed partial-array entries published by the array traversal.
    virtual void FollowPartialArray(const MarkStackEntry& entry, WorkStack& workStack);
    virtual BaseObject* GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field)
    {
        Collector::AbortUnimplemented("CopyCollector::GetAndTryTagObj");
    }
    inline bool IsResurrectedObject(const BaseObject* obj) const { return RegionSpace::IsResurrectedObject(obj); }

    // ZPage::mark_object(addr, finalizable = true) with direct inc_live accounting.
    virtual bool ResurrectObject(BaseObject* obj, size_t offset, ZPage* regionInfo)
    {
        (void)offset;
        bool incLive = false;
        const bool newlyMarked = regionInfo->mark_object(from_object(obj), true, incLive);
        if (incLive) {
            regionInfo->inc_live(1, obj->GetSize());
        }
        bool resurrected = !newlyMarked;
        if (!resurrected) {
            size_t objSize = obj->GetSize();
            if (!fixReferences && regionInfo->IsFromRegion()) {
                VLOG(REPORT, "resurrection tag w-obj %p<cls %p>+%zu", obj, obj->GetTypeInfo(), objSize);
            }
        }
        return resurrected;
    }

    Allocator& GetAllocator() const { return theAllocator; }


    MRT_EXPORT void RunGarbageCollection(uint64_t gcIndex, GCReason reason) override;
    virtual BaseObject* ForwardObjectExclusive(BaseObject* obj) = 0;

    void TransitionToGCPhase(const GCPhase phase, const bool, bool young = false)
    {
        MutatorManager::Instance().TransitionAllMutatorsToGCPhase(phase, young);
    }

    GCStats& GetGCStats(GCCycleGeneration generation = GCCycleGeneration::OLD) override
    {
        return GetGenerationCycle(generation).Stats();
    }

    virtual void UpdateGCStats();


protected:
    virtual void ForwardFromSpace(GCCycleGeneration generation);
    virtual void RefineFromSpace();
    virtual void DoGarbageCollection(GCCycleGeneration generation) = 0;
    void RequestGCInternal(GCReason reason, bool async) override { collectorResources.RequestGC(reason, async); }

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
    ZWorkers& GetWorkers(GCCycleGeneration generation) const
    {
        return *(generation == GCCycleGeneration::YOUNG ? youngCycle : oldCycle).Workers();
    }
    // enum roots referenced by foreign languages.
    void EnumAllExportRoots(RootSet& foreignRootsSet);
    // let finalizerProcessor process finalizers, and mark resurrected if in light sync gc
    virtual void ProcessFinalizers() {}
    void DiscoverFinalizableRoot(NativeSlot& slot) const;

    void MergeMutatorRoots(WorkStack& workStack);
    void DoEnumeration(WorkStack& workStack, WorkStack& foreignRootsSet);
    void DoTracing(WorkStack& workStack, WorkStack& foreignRootsSet);
    bool TryEndOldMark(WorkStack& workStack, WorkStack& foreignRootsSet);
    bool FlushMarkProducers(ZMark* domain);
    void ProcessOldNonStrongReferences(WorkStack& workStack);
    void ProcessExportRoots(WorkStack& foreignRootsSet);

    // concurrent marking.
    void TracingImpl(WorkStack& workStack);

    virtual void EnumAndTagRawRoot(ObjectRef& root, RootSet& rootSet, Generation generation) const
    {
        Collector::AbortUnimplemented("CopyCollector::EnumAndTagRawRoot");
    }

    void FindUselessExternObjects();

    // Export-root producer consumed by the old roots task (zMark.cpp
    // mark_old_roots -> MarkOldObjectIfActive).
    void VisitSurrectedExportRoots(const std::function<void(BaseObject*)>& visitor);

private:
    size_t RunMajorStripeMark(WorkStack& workStack, bool partial = false);
    void EnumMutatorRoot(ObjectPtr& obj, RootSet& rootSet) const;

    void VisitStaticRoots(const NativeSlotVisitor& visitor) const;
    void VisitFinalizerRoots(const NativeSlotVisitor& visitor) const;
};
} // namespace MapleRuntime
#endif // MRT_COLLECTOR_TRACING_H
