// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zHeap.hpp"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zMark.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <condition_variable>
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
#include "Heap/z/zMark.hpp"
#include "Heap/z/zBreakpoint.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.inline.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Common/SuspendibleThreadSet.h"
#include "Heap/z/zUncoloredRoot.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Mutator/MutatorManager.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/Handshake.h"
#include "Heap/z/zReferenceProcessor.hpp"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zRelocate.hpp"

namespace MapleRuntime {

static const ZStatSubPhase PYoungMarkFollow("young.mark_follow", ZGenerationId::young);
// ZMark::_ncontinue (zMark.cpp:975-981). Always on so a zero is readable as
// "the pre-pause test was right every time" rather than "nobody is counting".
std::atomic<size_t> g_markTerminateContinue{ 0 };
std::atomic<size_t> g_markTerminatePauses{ 0 };
std::atomic<size_t> g_markTerminateFlushed{ 0 };
std::atomic<bool> g_markTerminateAtexitInstalled{ false };
#if defined(MRT_TESTABLE_INTERNALS)
std::atomic<uint64_t> g_markTerminateMaxPauseNs{ 0 };
std::atomic<size_t> g_markTerminatePauseY2y{ 0 };
std::atomic<size_t> g_markTerminateClosureDuringPause{ 0 };
#endif

void NoteMarkTerminatePause() { g_markTerminatePauses.fetch_add(1, std::memory_order_relaxed); }

void NoteMarkTerminateFlushed(size_t n)
{
    g_markTerminateFlushed.fetch_add(n, std::memory_order_relaxed);
}

void NoteMarkTerminateContinue(size_t stackSize)
{
    const size_t nContinue = g_markTerminateContinue.fetch_add(1, std::memory_order_relaxed) + 1;
    LOG(RTLOG_ERROR, "[GCV2][markterm] pause found unflushed SATB work: ncontinue=%zu stack=%zu", nContinue,
        stackSize);
}

void ReportMarkTerminateContinue()
{
    if (!g_markTerminateAtexitInstalled.exchange(true, std::memory_order_relaxed)) {
        (void)std::atexit([]() {
            LOG(RTLOG_ERROR, "[GCV2][markterm] atexit pauses=%zu flushedInPause=%zu ncontinue=%zu",
                g_markTerminatePauses.load(std::memory_order_relaxed),
                g_markTerminateFlushed.load(std::memory_order_relaxed),
                g_markTerminateContinue.load(std::memory_order_relaxed));
        });
    }
}

#if defined(MRT_TESTABLE_INTERNALS)
void ResetMarkTerminateTestReceipt()
{
    g_markTerminateContinue.store(0, std::memory_order_relaxed);
    g_markTerminatePauses.store(0, std::memory_order_relaxed);
    g_markTerminateFlushed.store(0, std::memory_order_relaxed);
    g_markTerminateMaxPauseNs.store(0, std::memory_order_relaxed);
    g_markTerminatePauseY2y.store(0, std::memory_order_relaxed);
    g_markTerminateClosureDuringPause.store(0, std::memory_order_relaxed);
}

MarkTerminateTestReceipt ReadMarkTerminateTestReceipt()
{
    return { g_markTerminatePauses.load(std::memory_order_relaxed),
             g_markTerminateFlushed.load(std::memory_order_relaxed),
             g_markTerminateContinue.load(std::memory_order_relaxed),
             g_markTerminateMaxPauseNs.load(std::memory_order_relaxed),
             g_markTerminatePauseY2y.load(std::memory_order_relaxed),
             g_markTerminateClosureDuringPause.load(std::memory_order_relaxed) };
}

void NoteMarkTerminatePauseDuration(uint64_t pauseNs)
{
    uint64_t observed = g_markTerminateMaxPauseNs.load(std::memory_order_relaxed);
    while (observed < pauseNs &&
           !g_markTerminateMaxPauseNs.compare_exchange_weak(observed, pauseNs, std::memory_order_relaxed)) {}
}

void NoteMarkTerminatePauseProducers(size_t y2y)
{
    g_markTerminatePauseY2y.fetch_add(y2y, std::memory_order_relaxed);
}

void NoteTraceYoungClosureDuringPause()
{
    g_markTerminateClosureDuringPause.fetch_add(1, std::memory_order_relaxed);
}
#endif


#if defined(MRT_TESTABLE_INTERNALS)
std::function<void(ZGenerationId, NativeSlot*)> ZMark::testColoredRootResult;
std::function<void(Mutator&)> ZMark::testOldMarkThreadResult;
#endif
// RefFieldRoot is root in tagged pointer format.
void ZMark::EnumRefFieldRoot(RefField<>& field, ValueRootList& exportOwners)
{
    RefField<> oldField(field);
    CHECK_DETAIL(!Heap::IsHeapAddress(to_object(oldField.GetTargetObject())) ||
                     (raw(oldField.GetFieldValue()) &
                      (ZPointerRemappedMask | ZPointerMarkedYoungMask | ZPointerMarkedOldMask)) != 0,
                 "NativeSlot requires colored value at EnumRefFieldRoot slot=%p word=%#zx",
                 &field, raw(oldField.GetFieldValue()));
    ZBarrier::MarkBarrierOnOopField(field, false);
    BaseObject* latest = to_object(field.GetTargetObject());
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    // Ownership state carries current identity and color, never GC work entries.
    exportOwners.emplace_back(latest, ForwardingStage::IncomingNew);
}




// Shared by mark roots and Cangjie foreign-root traversal.
thread_local const char* gMinorRootOrigin = "unknown";

namespace {
// ZGC zMark.cpp:703-708,827-828,883: both generations use this thread closure.
// Cangjie has no return statepoint: retain the saved head color for the full
// scan and expand stack objects/headerless records before visiting heap slots.
class MarkThreadClosure {
public:
    explicit MarkThreadClosure(uint64_t epoch = StackWatermark::epoch_id(), const RootVisitor* result = nullptr)
        : epoch(epoch), result(result) {}
    static StackWatermarkProcessOopClosure::RootFunction root_function() { return ZUncoloredRoot::mark; }
    void DoThread(Mutator& mutator) const
    {
        const uintptr_t color = mutator.GetGCData().loadGoodMask;
        RootVisitor markRoot = [&](ObjectRef& root) {
            mutator.VisitHeapRootSlots(root, [&](ObjectRef& slot) {
                ZUncoloredRoot::mark(reinterpret_cast<zaddress_unsafe*>(&slot), color);
                if (result != nullptr) (*result)(slot);
            });
        };
        DerivedPtrVisitor derivedVisitor = Mutator::MakeDerivedRootVisitor(markRoot);
        size_t frames = 0;
        (void)StackWatermarkSet::finish_processing(mutator, markRoot, markRoot, epoch,
                                                   &derivedVisitor, frames, reinterpret_cast<void*>(root_function()));
    }
private:
    const uint64_t epoch;
    const RootVisitor* const result;
};
} // namespace

void ZMark::VisitMinorRootSlots(RootVisitor& rawRootVisitor, RootVisitor& invisibleRootVisitor,
                                     uint64_t stackScanEpoch)
{
    gMinorRootOrigin = "mutator_stack";
    RootVisitor plainRoot = [&](ObjectRef& root) {
        ZUncoloredRoot::mark(reinterpret_cast<zaddress_unsafe*>(&root), ZPointerLoadGoodMask);
        rawRootVisitor(root);
    };
    MarkThreadClosure threadClosure(stackScanEpoch, &rawRootVisitor);
    VisitStrongPlainRoots(plainRoot, [&](Mutator& mutator) { threadClosure.DoThread(mutator); });
    (void)invisibleRootVisitor; // The watermark owns the invisible slot and its saved color.
    gMinorRootOrigin = "unknown";
}



// ZReferenceProcessor::should_discover/discover (zReferenceProcessor.cpp:174-201,
// 239-250). Native registration owns the original referent slot, rather than a
// Java FinalReference object. The load barrier heals remapping before discovery.
void ZMark::DiscoverFinalizableRoot(NativeSlot& slot)
{
    CHECK(Heap::GetHeap().old().IsPhaseMark());
    BaseObject* object = ZBarrier::ReadStaticRef(slot);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &slot };
    object = ZBarrier::ValidateCurrentValue(object, provenance);
    if (object == nullptr) return;
    auto* page = Heap::page(reinterpret_cast<MAddress>(object));
    if (page->IsYoungRegion() || page->is_object_strongly_live(from_object(object))) return;
    auto& processor = Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor();
    (void)processor.DiscoverReference(object, ReferenceType::FINAL);
    ZBarrier::MarkFinalizableBarrierOnRoot(slot);
}

void ZMark::DiscoverWeakReference(BaseObject* reference, WorkStack& workStack)
{
    HeapSlot<>& referentField =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(reference) + TYPEINFO_PTR_SIZE);
    BaseObject* referent = ZBarrier::GetAndTryTagObj(ZBarrier::RefSlotKind::WEAK_REFERENT, reference, referentField);
    if (referent == nullptr) {
        return;
    }
    (void)Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor().DiscoverReference(reference, ReferenceType::WEAK);
    (void)workStack;
}

namespace {
// ZMarkOopClosure (zMark.cpp:666-670). P08 owns the missing dedicated old
// mark barrier; this adapter consumes the existing old publication producer.
    class MarkOopClosure {
    public:
        void DoOop(NativeSlot& slot) const
        {
            ZBarrier::MarkBarrierOnOopField(slot, false);
        }
    };

// ZMarkOldRootsTask, zMark.cpp:797-834. Root results are published to the
// generation mark domain by closures, then flushed by each participating worker.
class MarkOldRootsTask final : public ZTask {
public:
    MarkOldRootsTask(ZMark& domain,
                     NativeSlotVisitor finalizable, std::function<void()> uncolored, unsigned workers)
        : ZTask("ZMarkOldRootsTask"), rootsColored(workers),
          finalizerRoots(Heap::GetHeap().GetFinalizerProcessor().WeakRootStorage(), workers),
          finalizable(std::move(finalizable)), domain(domain), uncolored(std::move(uncolored)) {}
    void work() override
    {
        finalizerRoots.OopsDo(finalizable);
        rootsColored.Apply([&](NativeSlot& slot) {
            coloredClosure.DoOop(slot);
#if defined(MRT_TESTABLE_INTERNALS)
            if (ZMark::testColoredRootResult) {
                ZMark::testColoredRootResult(ZGenerationId::old, &slot);
            }
#endif
        });
        rootsUncolored.Apply(uncolored);
        rootsUncolored.ApplyThreads([&](Mutator& mutator) {
            threadClosure.DoThread(mutator);
#if defined(MRT_TESTABLE_INTERNALS)
            if (ZMark::testOldMarkThreadResult) ZMark::testOldMarkThreadResult(mutator);
#endif
        });
        // zMark.cpp:830-834: flush and free worker stacks for both generations
        // here, since the set of workers executing during root scanning can be
        // different from the set of workers executing during mark.
        ThreadLocal::FlushCurrentThreadMarkStacks();
#if defined(MRT_TESTABLE_INTERNALS)
        if (ZMark::testColoredRootResult) {
            ZMark::testColoredRootResult(ZGenerationId::old, nullptr);
        }
#endif
    }
private:
    RootsIteratorStrongColored rootsColored;
    OopStorage::ParState<true> finalizerRoots;
    NativeSlotVisitor finalizable;
    RootsIteratorStrongUncolored rootsUncolored;
    MarkOopClosure coloredClosure;
    MarkThreadClosure threadClosure;
    ZMark& domain;
    std::function<void()> uncolored;
};
} // namespace

void ZMark::EnumAllCommonRoots(ZWorkers& workers)
{
    CHECK_DETAIL(Heap::GetHeap().old().MarkPtr() != nullptr, "old mark domain must start before roots");
    MarkOldRootsTask task(Heap::GetHeap().old().Mark(),
                         [](NativeSlot& slot) { DiscoverFinalizableRoot(slot); }, [&] {
        VisitStrongPlainRoots([&](ObjectRef& root) {
            ZUncoloredRoot::mark(reinterpret_cast<zaddress_unsafe*>(&root), ZPointerLoadGoodMask);
        }, {});
        Heap::GetHeap().cross_vm().VisitSurrectedExportRoots([](BaseObject* object) {
            if (Heap::IsHeapAddress(object)) {
                ZBarrier::Mark<false, false, true, false>(from_object(object));
            }
        });
    }, workers.active_workers());
    workers.run(&task);
}

namespace {
// ZMarkYoungOopClosure, zMark.cpp:678-681.
class MarkYoungOopClosure {
public:
    void DoOop(NativeSlot& slot) const
    {
        ZBarrier::MarkYoungGoodBarrierOnOopField(slot);
    }
};

// ZMarkYoungRootsTask, zMark.cpp:852-891. Colored roots share one closure;
// Cangjie's stack/value-root scanner replaces HotSpot thread/nmethod closures.
class MarkYoungRootsTask final : public ZTask {
public:
    MarkYoungRootsTask(std::function<void()> uncolored, unsigned workers)
        : ZTask("ZMarkYoungRootsTask"), rootsColored(workers), uncolored(std::move(uncolored)) {}

    void work() override
    {
        rootsColored.Apply([this](NativeSlot& slot) {
            coloredClosure.DoOop(slot);
#if defined(MRT_TESTABLE_INTERNALS)
            if (ZMark::testColoredRootResult) {
                ZMark::testColoredRootResult(ZGenerationId::young, &slot);
            }
#endif
        });
        rootsUncolored.Apply(uncolored);
        // zMark.cpp:887-891: flush and free worker stacks for both generations.
        ThreadLocal::FlushCurrentThreadMarkStacks();
#if defined(MRT_TESTABLE_INTERNALS)
        if (ZMark::testColoredRootResult) {
            ZMark::testColoredRootResult(ZGenerationId::young, nullptr);
        }
#endif
    }
private:
    RootsIteratorAllColored rootsColored;
    MarkYoungOopClosure coloredClosure;
    std::function<void()> uncolored;
    RootsIteratorAllUncolored rootsUncolored;
};
} // namespace

void ZMark::VisitMinorRoots(const std::function<void(BaseObject*)>& visitor,
                                 const std::function<void(BaseObject*)>& invisibleVisitor,
                                 uint64_t stackScanEpoch)
{
    RootVisitor rawRootVisitor = [&visitor](ObjectRef& root) {
        visitor(to_object(safe(root.LoadPlain())));
    };
    RootVisitor invisibleRootVisitor = [&invisibleVisitor](ObjectRef& root) {
        invisibleVisitor(to_object(safe(root.LoadPlain())));
    };
    MarkYoungRootsTask task([&] {
        VisitMinorRootSlots(rawRootVisitor, invisibleRootVisitor, stackScanEpoch);
        Heap::GetHeap().cross_vm().VisitMinorValueRoots([&](BaseObject* object) {
            if (Heap::IsHeapAddress(object)) {
                ZBarrier::Mark<false, false, true, false>(from_object(object));
            }
            visitor(object);
        });
        gMinorRootOrigin = "export";
        Heap::GetHeap().VisitAllExportRoots([&](NativeSlot& slot) {
            ZBarrier::MarkBarrierOnOopField(slot, false);
            visitor(to_object(slot.GetTargetObject()));
        });
        gMinorRootOrigin = "unknown";
    }, (*Heap::GetHeap().GetZGeneration(ZGenerationId::young).Workers()).active_workers());
    SuspendibleThreadSetJoiner joiner;
    (*Heap::GetHeap().GetZGeneration(ZGenerationId::young).Workers()).run(&task);

}

void ZMark::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin)
{
    PushYoungObject(object, workStack, origin, false);
}

void ZMark::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin,
                                  bool finalizable)
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    if (!object->IsValidObject()) {
        // Rich diagnosis before fail-closed abort: address looks like a heap range
        // but object header is not a valid managed object (stack-ish residue, stale
        // slot, or stackmap-mislabeled root). Printed once per process by default.
        static std::atomic<size_t> g_invalidMinorRootPrinted{ 0 };
        size_t n = g_invalidMinorRootPrinted.fetch_add(1, std::memory_order_relaxed);
        // Prefer explicit non-generic origin; "minor_root" is a placeholder that
        // should yield to the TLS tag set by VisitMinorRootSlots/ValueRoots.
        const char* src = origin;
        if (src == nullptr || std::strcmp(src, "unknown") == 0 || std::strcmp(src, "minor_root") == 0) {
            if (gMinorRootOrigin != nullptr && std::strcmp(gMinorRootOrigin, "unknown") != 0) {
                src = gMinorRootOrigin;
            } else if (src == nullptr) {
                src = "unknown";
            }
        }
        if (n < 8) {
            ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
            VLOG(REPORT,
                 "[GCV2][invalid-minor-root] obj=%p origin=%s region=%p regionStart=%#zx young=%u pinned=%u "
                 "large=%u free=%u garbage=%u neverExamined=%u "
                 "(fail-closed next; AS1 relation: bad header on stack-live slot vs SKIPPED frame)",
                 object, src, region,
                 region == nullptr ? 0 : static_cast<size_t>(region->GetRegionStart()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsYoungRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsPinnedRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsLargeRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsFreeRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsGarbageRegion()),
                 region == nullptr ? 0u
                                   : static_cast<unsigned>(!region->is_marked() &&
                                                          region->GetRegionAllocPtr() > region->GetRegionStart()));
        }
        CHECK_DETAIL(false, "minor root/reference %p is not a valid object origin=%s", object, src);
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
    if (!region->IsYoungRegion()) {
        return;
    }
    (void)workStack;
    if (finalizable) {
        const_cast<ZGeneration&>(Heap::GetHeap().GetZGeneration(ZGenerationId::young))
            .MarkObjectIfActive<false, true, true, true>(from_object(object));
    } else {
        const_cast<ZGeneration&>(Heap::GetHeap().GetZGeneration(ZGenerationId::young))
            .MarkObjectIfActive<false, true, true, false>(from_object(object));
    }
}

// Young mark closure: address-striped follow_work (ZGC zMark.cpp:635 / zMark.cpp:94-120).
namespace {
constexpr size_t kMarkStripeShift = 20;
constexpr size_t kMarkStripeMultiplier = 4;
constexpr size_t kMarkStripeMax = 64;

std::atomic<size_t> g_markStripeArmed{ 0 };
std::atomic<size_t> g_markStripeTurned{ 0 };

// FYS raw workStack.push_back used to skip PushYoungObject recover + StartWho.
// Admit the same host that FYS=0 would have pushed; never enqueue an interior.
BaseObject* AdmitYoungObject(BaseObject* object, const char* origin, const void* slot = nullptr,
                             BaseObject* holder = nullptr)
{
    if (!Heap::IsHeapAddress(object)) {
        return nullptr;
    }

    return object;
}

} // namespace


namespace {
size_t MarkStripeCount(size_t workers)
{
    size_t target = std::max<size_t>(workers * kMarkStripeMultiplier, kMarkStripeMultiplier);
    size_t count = 1;
    while (count < target && count < kMarkStripeMax) {
        count <<= 1;
    }
    return count;
}

} // namespace

// h3seed3 乙: live-holder slot → free|garbage target → CAS null.
// Criterion fields (RegionInfo state word): IsFreeRegion() / IsGarbageRegion()
// via TryGetRegionInfoAt(target) at the call site (closure edge or Fix).
// Returns true if the slot was scrubbed (caller must not push / treat as live edge).
bool ScrubMinorFreeTarget(RefField<>& field, BaseObject* target, bool /*fromFix*/)
{
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(target));
    if (region == nullptr) {
        return false;
    }
    const bool isFree = region->IsFreeRegion();
    const bool isGarbage = region->IsGarbageRegion();
    if (!isFree && !isGarbage) {
        return false;
    }
    if (SlotHeldByLiveObject(&field)) {
        return false;
    }
    RefField<> oldField(field);
    const MAddress oldVal = raw(oldField.GetFieldValue());
    // zBarrier.inline.hpp:294-343 has no unresolved-to-null installation arm.
    // A free/garbage target means forwarding authority was retired before
    // coverage completed; fail closed instead of manufacturing a null heal.
    (void)field.CompareExchange(oldField.GetFieldValue(), zpointer::null);
    ZBarrier::FailClosedLoad(
        "ZMark::ScrubMinorFreeTarget.unresolved", target, oldVal,
        ForwardingProvenance{ ForwardingHolderKind::Remset, nullptr, &field });
}



class ZMarkTask : public ZRestartableTask {
public:
    explicit ZMarkTask(ZMark* mark, bool partial = false)
        : ZRestartableTask("ZMarkTask"), mark(mark), partial(partial)
    {
        mark->PrepareWork();
    }
    ~ZMarkTask() { mark->FinishWork(); }

    void resize_workers(uint32_t workers) override { mark->ResizeWorkers(workers); }

    void work() override
    {
        SuspendibleThreadSetJoiner stsJoiner;
        mark->FollowWorkComplete(partial);
    }

private:
    ZMark* const mark;
    const bool partial;
};

void ZMark::TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,
                                          std::vector<BaseObject*>& reachableVec, std::unordered_set<MAddress>& reachableSlots,
                                          std::unordered_set<MAddress>& weakSlots,
                                          const std::unordered_set<MAddress>* reachableSlotDomain)
{
    (void)fullYoungScan;
    (void)reachableVec;
    (void)reachableSlots;
    (void)weakSlots;
    (void)reachableSlotDomain;
    (void)workStack;
    g_markStripeArmed.fetch_add(1, std::memory_order_relaxed);
    const size_t dispelAtEntry = ZPage::GetTdWindowCount();
    ZWorkers& workersSet = (*Heap::GetHeap().GetZGeneration(ZGenerationId::young).Workers());
    g_markStripeTurned.fetch_add(1, std::memory_order_relaxed);
    ZMark& domain = Heap::GetHeap().young().Mark();
    (void)ZMark::PublishHandshakeMarkWork(workStack, &domain);
    (void)domain.Stacks().Flush(domain.Stripes(), true);
    ZMarkTask task(&domain, false);
    workersSet.run(&task);
    if (!ZAbort::should_abort()) {
        MarkingStacks::VerifyEmpty(domain.Stripes().Population());
        CHECK_DETAIL(domain.Stripes().IsEmpty(),
                     "young striped closure returned without coordinated worker termination");
    }
    const size_t dispelAtExit = ZPage::GetTdWindowCount();
    CHECK_DETAIL(dispelAtExit == dispelAtEntry,
                 "T-D ghost dispel during striped mark_closure window entry=%zu exit=%zu", dispelAtEntry,
                 dispelAtExit);
}

void ZMark::TraceYoungClosure(WorkStack& workStack, bool fullYoungScan,
                                   std::vector<BaseObject*>& reachableVec, std::unordered_set<MAddress>& reachableSlots,
                                   std::unordered_set<MAddress>& weakSlots,
                                   const std::unordered_set<MAddress>* reachableSlotDomain)
{
#if defined(MRT_TESTABLE_INTERNALS)
    // Observe the completed closure result before the following GC phases
    // can promote/reset its page. This has no product-build call or state.
    struct ClosureObservation {
        const std::vector<BaseObject*>& objects;
        ~ClosureObservation() { ObserveMarkClosureForTest(&objects); }
    } observation{reachableVec};

    if (MutatorManager::Instance().WorldStopped()) {
        NoteTraceYoungClosureDuringPause();
    }
#endif
    (void)Heap::GetHeap().young().Mark().Flush(ThreadLocal::GetThreadLocalData());
    if (workStack.empty() && Heap::GetHeap().young().Mark().Stripes().IsEmpty() &&
        Heap::GetHeap().young().Mark().Stacks().IsEmpty()) {
        return;
    }

    TraceYoungClosureStriped(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots,
                             reachableSlotDomain);
}

// ZGenerationYoung::concurrent_mark_continue follows the generation's
// published mark work. Young marking has no SATB queue (zBarrier.cpp:160-180).
bool ZMark::FollowYoungMark(WorkStack& workStack, bool fullYoungScan,
                                     std::vector<BaseObject*>& reachableVec, std::unordered_set<MAddress>& reachableSlots,
                                     std::unordered_set<MAddress>& weakSlots,
                                     YoungConcWindowStats* windowStats)
{
    ZStatTimerWorker zstatTimer(PYoungMarkFollow);
    // Follow explicit roots and allocation work; young has no SATB queue.
#if defined(MRT_TESTABLE_INTERNALS)
    PublishConcurrentYoungProducersTestReceipt();
#endif
    (void)Heap::GetHeap().young().Mark().Flush();
    (void)Heap::GetHeap().young().Mark().Flush(ThreadLocal::GetThreadLocalData());
    (void)ZMark::PublishHandshakeMarkWork(workStack, &Heap::GetHeap().young().Mark());
    do {
        if (!workStack.empty() || !Heap::GetHeap().young().Mark().Stripes().IsEmpty() ||
            !Heap::GetHeap().young().Mark().Stacks().IsEmpty()) {
            if (windowStats != nullptr) {
                ++windowStats->closureCalls;
            }
            TraceYoungClosure(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots);
        }
        if (ZAbort::should_abort()) {
            return false;
        }
    } while (Heap::GetHeap().young().Mark().TryTerminateFlush());
    return true;
}

bool ZMark::TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats)
{
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "young mark-end flush requires stopped mutators");
    NoteMarkTerminatePause();
    const size_t before = Heap::GetHeap().young().Mark().Stripes().Population();
    (void)ZMark::PublishHandshakeMarkWork(workStack, &Heap::GetHeap().young().Mark());
    const bool ended = Heap::GetHeap().young().Mark().TryEnd();
    const size_t after = Heap::GetHeap().young().Mark().Stripes().Population();
    NoteMarkTerminateFlushed(after >= before ? after - before : 0);
    if (!ended) {
        return false;
    }
    MarkingStacks::VerifyAllEmpty(Heap::GetHeap().young().Mark());
    return true;
}


void ZMark::ProcessFinalizers()
{
    FinalizerProcessor& fp = Heap::GetHeap().GetFinalizerProcessor();
    fp.ProcessReferences([](BaseObject* obj) { return RegionSpace::IsMarkedObject<Generation::Old>(obj); });
}

bool ZMark::PublishHandshakeMarkWork(WorkStack& work, ZMark* domain)
{
    if (domain == nullptr || work.empty()) {
        return false;
    }
    MarkThreadLocalStacks& seed = domain->Stacks();
    bool published = false;
    while (!work.empty()) {
        const MarkStackEntry entry = work.back();
        work.pop_back();
        MAddress address = 0;
        if (entry.partial_array()) {
            size_t length = 0;
            MarkPartialArray::Decode(entry, address, length);
        } else {
            address = reinterpret_cast<MAddress>(to_object(ZOffset::address(to_zoffset(entry.object_address()))));
        }
        if (address == 0) {
            continue;
        }
        seed.Push(domain->Stripes(), domain->Stripes().StripeForAddress(address), entry, true);
        published = true;
    }
    if (published) {
        (void)seed.Flush(domain->Stripes(), true);
        domain->Terminate().Wake();
    }
    return published;
}

void ZMark::DrainAllocBufferMarkProducers(AllocBuffer* buffer, WorkStack& work, bool young)
{
    if (buffer == nullptr) {
        return;
    }
    if (!young) {
        return;
    }
    buffer->MergeY2yDirtyHolders(work);
    buffer->MergeY2yDirtySlots([&work](MAddress slot) {
        ZBarrier::MarkBarrierOnYoungOopField(HeapSlotAt<>(slot));
    });
}


bool ZMark::FlushGCDataMarkProducers(ThreadGCData& data, ZMark* domain)
{
    return domain != nullptr && data.FlushMarkStacks(*domain);
}

bool ZMark::FlushGCDataMarkProducers(ThreadGCData& data)
{
    const bool young = ZMark::FlushGCDataMarkProducers(data, Heap::GetHeap().young().MarkPtr());
    return ZMark::FlushGCDataMarkProducers(data, Heap::GetHeap().old().MarkPtr()) || young;
}

bool ZMark::FlushThreadMarkProducers(ThreadLocalData* tls)
{
    bool published = ZMark::FlushThreadMarkProducers(tls, Heap::GetHeap().young().MarkPtr());
    return ZMark::FlushThreadMarkProducers(tls, Heap::GetHeap().old().MarkPtr()) || published;
}

bool ZMark::FlushThreadMarkProducers(ThreadLocalData* tls, ZMark* domain)
{
    if (tls == nullptr || domain == nullptr) {
        return false;
    }
    WorkStack work;
    const bool young = domain->Generation() == MarkingStacks::MarkingGeneration::YOUNG;
    ZMark::DrainAllocBufferMarkProducers(tls->buffer, work, young);
    const bool published = ZMark::PublishHandshakeMarkWork(work, domain);
    return ThreadLocal::FlushMarkStacks(tls, *domain) || published;
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
bool ZMark::MarkEntryObject(BaseObject* obj, const MarkStackEntry& entry,
                            MarkLiveCache* cache)
{
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(obj));
    CHECK_DETAIL(region->IsRelocatable(), "mark consumer requires a relocatable page");
    bool firstLive = entry.inc_live();
    bool already = false;
    if (entry.mark()) {
        already = !region->mark_object(from_object(obj), entry.finalizable(), firstLive);
    }
    if (!already && firstLive) {
        if (cache != nullptr) {
            cache->IncLive(region, obj->GetSize());
        } else {
            region->inc_live(1, obj->GetSize());
        }
    }
    return already;
}


} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMark.hpp"

#include "Base/Log.h"
#include "Heap/z/zWorkers.hpp"
#include "Mutator/MutatorManager.h"
#include "Heap/z/zMark.hpp"

namespace MapleRuntime {

static bool StealLocalRound(MarkContext& context, MarkStripeSet& stripes)
{
    MarkThreadLocalStacks& stacks = context.Stacks();
    const size_t home = context.StripeId();
    for (size_t victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
        MarkStripeStack* stack = stacks.StealLocal(victim);
        if (stack != nullptr) {
            stacks.Install(home, stack);
            return true;
        }
    }
    return false;
}

static bool StealGlobalRound(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, size_t workerId,
                             std::atomic<size_t>* stealSuccess, std::atomic<size_t>* stealFailure)
{
    MarkThreadLocalStacks& stacks = context.Stacks();
    const size_t home = context.StripeId();
    for (size_t victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
        MarkStripeStack* stack = stripes.At(victim).StealStack(smr, workerId);
        if (stack != nullptr) {
            if (stealSuccess != nullptr) {
                stealSuccess->fetch_add(1, std::memory_order_relaxed);
            }
            stacks.Install(home, stack);
            return true;
        }
        if (stealFailure != nullptr) {
            stealFailure->fetch_add(1, std::memory_order_relaxed);
        }
    }
    return false;
}

static bool RebalanceWork(MarkContext& context, MarkStripeSet& stripes, MarkTerminate& terminate, size_t workerId,
                          size_t nworkers, ZMark* domain)
{
    const size_t assumed = context.NStripes();
    const size_t nstripes = stripes.NStripes();
    if (assumed != nstripes) {
        context.SetNStripes(nstripes);
    } else if (nstripes < stripes.CalculateNStripes(nworkers) && stripes.IsCrowded()) {
        const size_t restored = nstripes << 1;
        if (stripes.TrySetNStripes(nstripes, restored)) {
            context.SetNStripes(restored);
        }
    }
    const size_t stripe = stripes.StripeForWorker(nworkers, workerId);
    if (context.StripeId() != stripe) {
        context.SetStripeId(stripe);
        (void)context.Stacks().Flush(stripes, false);
        terminate.Wake();
    } else if (!terminate.Saturated()) {
        (void)context.Stacks().Flush(stripes, false);
        terminate.Wake();
    }
    return domain != nullptr && domain->PollStop();
}

static bool Drain(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, MarkTerminate& terminate,
                  size_t workerId, size_t nworkers, const ZMark::Process& process, ZMark* domain)
{
    MarkStackEntry entry;
    size_t processed = 0;
    context.SetStripeId(stripes.StripeForWorker(nworkers, workerId));
    context.SetNStripes(stripes.NStripes());
    while (context.Stacks().Pop(smr, workerId, stripes, context.StripeId(), entry)) {
        process(entry);
        if ((processed++ & 31) == 0 && RebalanceWork(context, stripes, terminate, workerId, nworkers, domain)) {
            return false;
        }
    }
    return true;
}

ZMark::Result ZMark::FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                                          MarkTerminate& terminate, size_t workerId, bool partial,
                                          const Process& process, std::atomic<size_t>* stealSuccess,
                                          std::atomic<size_t>* stealFailure, ZMark* domain)
{
    const size_t nworkers = terminate.workerCount;
    for (;;) {
        if (!Drain(context, smr, stripes, terminate, workerId, nworkers, process, domain)) {
            terminate.Leave();
            return Result::Aborted;
        }
        if (StealLocalRound(context, stripes) ||
            StealGlobalRound(context, smr, stripes, workerId, stealSuccess, stealFailure)) {
            continue;
        }
        if (context.Stacks().Flush(stripes, false)) {
            terminate.Wake();
            continue;
        }
        if (partial) {
            return Result::Partial;
        }
        if (domain != nullptr && domain->TryProactiveFlush(workerId)) {
            continue;
        }
        if (terminate.TryTerminate(stripes, context.NStripes())) {
            context.Cache().Flush();
            smr.reclaim();
            return Result::Completed;
        }
    }
}

ZMark::ZMark(size_t capacity, MarkingStacks::MarkingGeneration generation)
    : stripes(capacity), generation(generation)
{
    stripes.SetTerminate(&terminate);
}

size_t ZMark::CalculateNStripes(size_t workers) const
{
    return stripes.CalculateNStripes(workers);
}

void ZMark::EnsureWorkers(size_t workers)
{
    CHECK_DETAIL(workers <= ConcGCThreads, "mark workers exceed per-worker storage capacity");
}

void ZMark::Start()
{
    MarkingStacks::VerifyEmpty(stripes.Population());
    nproactiveflush = 0;
    nterminateflush = 0;
    ntrycomplete = 0;
    ncontinue = 0;
    CHECK_DETAIL(gcWorkers != nullptr, "ZMark::start requires workers");
    nworkers = gcWorkers->active_workers();
    targetNStripes = CalculateNStripes(nworkers);
    stripes.SetNStripes(targetNStripes);
    EnsureWorkers(nworkers);
    terminate.Reset(nworkers);
    // zMark.cpp:118-123: stripe count goes to the generation's mark account.
    const ZGenerationId statId =
        generation == MarkingStacks::MarkingGeneration::YOUNG ? ZGenerationId::young : ZGenerationId::old;
    Heap::GetHeap().GetZGeneration(statId).StatMark()->AtMarkStart(targetNStripes);
}

void ZMark::PrepareWork()
{
    CHECK_DETAIL(nworkers != 0, "mark domain needs a worker");
    targetNStripes = CalculateNStripes(nworkers);
    stripes.SetNStripes(targetNStripes);
    EnsureWorkers(nworkers);
    terminate.Reset(nworkers);
    workNProactiveFlush.store(0, std::memory_order_relaxed);
    workNTerminateFlush.store(0, std::memory_order_relaxed);
    terminate.SetResurrected(false);
}

void ZMark::PrepareWork(size_t workers)
{
    nworkers = workers;
    PrepareWork();
}

void ZMark::FollowWorkComplete(bool partial)
{
    const uint32_t workerId = WorkerThread::worker_id();
    MarkContext local(nworkers, workerId, stripes, Stacks());
    (void)FollowWork(local, smr, stripes, terminate, workerId, partial,
                     [this, &local](const MarkStackEntry& entry) { MarkAndFollow(local, entry); },
                     nullptr, nullptr, this);
    (void)local.Stacks().Flush(stripes, true);
    local.Cache().Flush();
    MarkingStacks::VerifyEmpty(local.Stacks().Population());
    ThreadLocal::FlushCurrentThreadMarkStacks();
}

bool ZMark::FollowWorkPartial()
{
    const uint32_t workerId = WorkerThread::worker_id();
    MarkContext local(nworkers, workerId, stripes, Stacks());
    const Result result = FollowWork(local, smr, stripes, terminate, workerId, true,
                     [this, &local](const MarkStackEntry& entry) { MarkAndFollow(local, entry); },
                     nullptr, nullptr, this);
    (void)local.Stacks().Flush(stripes, true);
    local.Cache().Flush();
    return result != Result::Aborted;
}

void ZMark::MarkFollow(bool partial)
{
    for (;;) {
        ZMarkTask task(this, partial);
        gcWorkers->run(&task);
        if (ZAbort::should_abort() || !TryTerminateFlush()) {
            break;
        }
    }
}

void ZMark::MarkAndFollow(MarkContext& ctx, const MarkStackEntry& entry)
{
    if (generation == MarkingStacks::MarkingGeneration::YOUNG) {
        auto visitSlot = [](MAddress slot) {
            auto& field = HeapSlotAt<>(slot);
            ZBarrier::MarkBarrierOnYoungOopField(field);
        };
        auto publish = [this, &ctx](const MarkStackEntry& work) {
            MAddress address = 0;
            if (work.partial_array()) {
                size_t length = 0;
                MarkPartialArray::Decode(work, address, length);
            } else {
                address = reinterpret_cast<MAddress>(to_object(ZOffset::address(to_zoffset(work.object_address()))));
            }
            const size_t stripeIndex = stripes.StripeForAddress(address);
            const bool published = stripeIndex != ctx.StripeId();
            ctx.Stacks().Push(stripes, stripeIndex, work, published);
            if (published) {
                terminate.Wake();
            }
        };
        if (entry.partial_array()) {
            MarkPartialArray::FollowPartialReferences(entry, visitSlot, publish);
            return;
        }
        BaseObject* object = to_object(ZOffset::address(to_zoffset(entry.object_address())));
        if (!Heap::IsHeapAddress(object)) {
            return;
        }
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
        if (!region->IsYoungRegion()) {
            return;
        }
        const bool wasMarked = MarkEntryObject(object, entry, &ctx.Cache());
        if (entry.mark() && wasMarked) {
            return;
        }
#if defined(MRT_TESTABLE_INTERNALS)
        {
            const std::vector<BaseObject*> observed{ object };
            ObserveMarkClosureForTest(&observed);
        }
#endif
        if (!object->HasRefField() || !entry.follow()) {
            return;
        }
        MarkPartialArray::FollowObjectReferences(object, entry.finalizable(), visitSlot, publish);
        return;
    }
    auto publish = [this, &ctx](const MarkStackEntry& work) {
        MAddress address = 0;
        if (work.partial_array()) {
            size_t length = 0;
            MarkPartialArray::Decode(work, address, length);
        } else {
            address = reinterpret_cast<MAddress>(to_object(ZOffset::address(to_zoffset(work.object_address()))));
        }
        const size_t stripeIndex = stripes.StripeForAddress(address);
        const bool published = stripeIndex != ctx.StripeId();
        ctx.Stacks().Push(stripes, stripeIndex, work, published);
        if (published) {
            terminate.Wake();
        }
    };
    if (UNLIKELY(MarkPartialArray::IsPartialArrayEntry(entry))) {
        MarkPartialArray::FollowPartialReferences(entry, [&entry](MAddress slot) {
            auto& field = HeapSlotAt<>(slot);
            ZBarrier::MarkBarrierOnOldOopField(nullptr, field, entry.finalizable());
        }, publish);
        return;
    }
    BaseObject* obj = to_object(ZOffset::address(to_zoffset(entry.object_address())));
    const bool wasMarked = MarkEntryObject(obj, entry, &ctx.Cache());
    if ((!entry.mark() || !wasMarked) && entry.follow()) {
        if (!obj->HasRefField()) {
            return;
        }
        if (UNLIKELY(obj->IsWeakRef())) {
            WorkStack discovered;
            ZMark::DiscoverWeakReference(obj, discovered);
            while (!discovered.empty()) {
                publish(discovered.back());
                discovered.pop_back();
            }
            return;
        }
        auto visitSlot = [obj, &entry](MAddress slot) {
            auto& field = HeapSlotAt<>(slot);
            ZBarrier::MarkBarrierOnOldOopField(obj, field, entry.finalizable());
        };
        MarkPartialArray::FollowObjectReferences(obj, entry.finalizable(), visitSlot, publish);
    }
}

void ZMark::ResizeWorkers(size_t workers)
{
    CHECK_DETAIL(workers != 0, "mark domain needs a worker");
    nworkers = workers;
    targetNStripes = CalculateNStripes(workers);
    stripes.SetNStripes(targetNStripes);
    EnsureWorkers(workers);
    terminate.Reset(workers);
}

void ZMark::FinishWork()
{
    nproactiveflush += workNProactiveFlush.load(std::memory_order_relaxed);
    nterminateflush += workNTerminateFlush.load(std::memory_order_relaxed);
}

bool ZMark::PollStop()
{
    if (ZAbort::should_abort()) {
        return true;
    }
    if (gcWorkers != nullptr && gcWorkers->should_worker_resize()) {
        return true;
    }
    return false;
}

MarkThreadLocalStacks& ZMark::Stacks()
{
    return ThreadLocal::GetMarkStacks(*this);
}

bool ZMark::FlushStacks()
{
    return ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), *this);
}

namespace {
bool HeapMarkReady()
{
    return true;
}

bool FlushTargetGCData(ThreadGCData& data, ZMark* domain)
{
    data.storeBarrierBuffer->Flush();
    if (!HeapMarkReady()) {
        return domain != nullptr ? data.FlushMarkStacks(*domain) : false;
    }
    return domain == nullptr ? ZMark::FlushGCDataMarkProducers(data)
                             : ZMark::FlushGCDataMarkProducers(data, domain);
}

} // namespace

bool ZMark::FlushThreadLocal(ThreadLocalData* tls, ZMark* domain)
{
    if (tls == nullptr) {
        return false;
    }
    bool published = false;
    if (tls->nativeGCData != nullptr && tls->nativeGCData != tls->gcData) {
        published = FlushTargetGCData(*tls->nativeGCData, domain);
    }
    if (tls->gcData != nullptr) {
        published = FlushTargetGCData(*tls->gcData, domain) || published;
    }
    if (!HeapMarkReady()) {
        return published;
    }
    return (domain == nullptr ? ZMark::FlushThreadMarkProducers(tls)
                              : ZMark::FlushThreadMarkProducers(tls, domain)) || published;
}

bool ZMark::HandshakeFlush(ZMark* domain)
{
    auto& manager = MutatorManager::Instance();
    bool flushed = false;
    if (manager.WorldStopped()) {
        {
            std::lock_guard<std::mutex> lock(manager.markFlushThreadMutex);
            for (auto& entry : manager.markFlushThreads) {
                if (entry.second->bufferLive.load(std::memory_order_acquire) == 0) {
                    continue;
                }
                if (FlushThreadLocal(entry.first, domain)) {
                    flushed = true;
                }
            }
        }
        ThreadGCData::VisitOwners([&](ThreadGCData& data, Mutator*, ThreadLocalData*) {
            flushed = FlushTargetGCData(data, domain) || flushed;
        });
        flushed = FlushThreadLocal(ThreadLocal::GetThreadLocalData(), domain) || flushed;
        return flushed;
    }

    class ZMarkFlushStacksHandshakeClosure : public HandshakeClosure {
    public:
        explicit ZMarkFlushStacksHandshakeClosure(ZMark* d)
            : HandshakeClosure("ZMarkFlushStacks"), domain_(d), flushed_(false) {}
        void do_thread(ThreadLocalData* tls) override
        {
            if (FlushThreadLocal(tls, domain_)) {
                flushed_ = true;
            }
        }
        bool flushed() const { return flushed_.load(std::memory_order_relaxed); }
    private:
        ZMark* domain_;
        std::atomic<bool> flushed_;
    } cl(domain);
    if (HeapMarkReady()) {
        Heap::GetHeap().GetFinalizerProcessor().Notify();
        Handshake::execute(&cl);
    } else {
        cl.do_thread(ThreadLocal::GetThreadLocalData());
    }
    ThreadGCData::VisitOwners([&](ThreadGCData& data, Mutator* target, ThreadLocalData*) {
        if (target == nullptr) { return; }
        target->MutatorLock();
        if (target->InSaferegion()) {
            flushed = FlushTargetGCData(data, domain) || flushed;
        }
        target->MutatorUnlock();
    });
    flushed = FlushThreadLocal(ThreadLocal::GetThreadLocalData(), domain) || flushed;
    if (cl.flushed()) {
        flushed = true;
    }
    {
        std::lock_guard<std::mutex> lock(manager.markFlushThreadMutex);
        for (auto it = manager.markFlushThreads.begin(); it != manager.markFlushThreads.end();) {
            if (it->second->dying.load(std::memory_order_acquire) != 0 &&
                it->second->refs.load(std::memory_order_acquire) == 0) {
                it = manager.markFlushThreads.erase(it);
            } else {
                ++it;
            }
        }
    }
    return flushed;
}

bool ZMark::Flush()
{
    return HandshakeFlush(this);
}

bool ZMark::Flush(ThreadLocalData* tls)
{
    return FlushThreadLocal(tls, this);
}

bool ZMark::FlushThread(ThreadLocalData* tls)
{
    return FlushThreadLocal(tls, nullptr);
}

bool ZMark::FlushAllGenerations()
{
    return HandshakeFlush(nullptr);
}

bool ZMark::TryProactiveFlush(size_t workerId)
{
    constexpr size_t proactiveFlushMax = 10;
    if (workerId != 0 || workNProactiveFlush.load(std::memory_order_relaxed) == proactiveFlushMax) {
        return false;
    }
    workNProactiveFlush.fetch_add(1, std::memory_order_relaxed);
    return Flush() || !stripes.IsEmpty();
}

bool ZMark::TryTerminateFlush()
{
    terminate.SetResurrected(false);
    workNTerminateFlush.fetch_add(1, std::memory_order_relaxed);
    (void)Flush();
    return !stripes.IsEmpty() || terminate.Resurrected();
}

bool ZMark::TryEnd()
{
    if (terminate.Resurrected()) {
        return false;
    }
    (void)Flush(ThreadLocal::GetThreadLocalData());
    // zMark.cpp:954-970: resurrected, then non-Java flush; empty stripes => complete.
    if (!HeapMarkReady()) {
        return stripes.IsEmpty();
    }
    (void)HandshakeFlush(this);
    (void)FlushStacks();
    if (!stripes.IsEmpty()) {
        return false;
    }
    // zMark.cpp:983-987: completed mark publishes its flush/continue counters.
    const ZGenerationId statId =
        generation == MarkingStacks::MarkingGeneration::YOUNG ? ZGenerationId::young : ZGenerationId::old;
    Heap::GetHeap().GetZGeneration(statId).StatMark()->AtMarkEnd(nproactiveflush, nterminateflush,
                                                                 ntrycomplete, ncontinue);
    return true;
}

void ZMark::Free()
{
    smr.free();
}

} // namespace MapleRuntime

#include "Heap/z/zMarkTerminate.inline.hpp"

// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/z/zMark.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"
namespace MapleRuntime {
namespace MarkingStacks {
// zMark.cpp:1016-1028. Inspect the same per-generation containers used by
// ThreadLocal::GetMarkStacks; verification must not flush or create a stack.
void VerifyAllEmpty(ZMark& domain)
{
    if (!ZVerifyMarking) { return; }
    const size_t index = domain.Generation() == MarkingGeneration::YOUNG ? 0 : 1;
    MutatorManager::Instance().VisitMarkingThreads([&](const ThreadGCData* data) {
        const auto& stacks = data->markStacks[index];
        CHECK_DETAIL(stacks.IsEmpty(),
                     "Thread marking stack is not empty: owner=%p generation=%zu", data, index);
    });
    CHECK_DETAIL(domain.Stripes().IsEmpty(), "Shared marking stripes are not empty");
}

void VerifyEmpty(size_t pending)
{
    if (ZVerifyMarking) { CHECK_DETAIL(pending == 0, "Marking stack is not empty: %zu", pending); }
}
}
}

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMarkPartialArray.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/z/zHeap.hpp"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
namespace MarkPartialArray {
// zMark.cpp:177-183 encode_partial_array_offset / decode_partial_array_offset.
MarkStackEntry Encode(const void* chunkStart, size_t length, bool finalizable)
{
    const MAddress addr = reinterpret_cast<MAddress>(chunkStart);
    DCHECK_D((addr & (MIN_SIZE - 1)) == 0, "Address misaligned");
    const size_t offset = untype(ZAddress::offset(to_zaddress(addr))) >> MIN_SIZE_SHIFT;
    return MarkStackEntry(offset, length, finalizable);
}

void Decode(const MarkStackEntry& entry, MAddress& chunkStart, size_t& length)
{
    const size_t offset = entry.partial_array_offset();
    length = entry.partial_array_length();
    chunkStart = raw(ZOffset::address(to_zoffset(offset << MIN_SIZE_SHIFT)));
}

// ZGC zMark.cpp:208-263 follow_array_elements: small arrays are visited
// locally, large arrays publish their aligned middle and trailing parts as
// partial-array entries and follow the leading part locally.
void FollowElements(MAddress start, size_t length, bool finalizable,
                    const FieldVisitor& visit, const EntryPublisher& publish)
{
    if (length <= MIN_LENGTH) {
        for (size_t i = 0; i < length; ++i) {
            visit(start + i * sizeof(MAddress));
        }
        return;
    }
    const MAddress end = start + length * sizeof(MAddress);
    const MAddress middleStart = AlignUp(start + sizeof(MAddress), MIN_SIZE);
    const size_t middleLength = AlignDown((end - middleStart) / sizeof(MAddress), MIN_LENGTH);
    const MAddress middleEnd = middleStart + middleLength * sizeof(MAddress);
    auto push = [&](MAddress address, size_t count) {
        publish(Encode(reinterpret_cast<const void*>(address), count, finalizable));
    };
    if (end > middleEnd) {
        push(middleEnd, (end - middleEnd) / sizeof(MAddress));
    }
    MAddress part = middleEnd;
    while (part > middleStart) {
        const size_t count = AlignUp((part - middleStart) / sizeof(MAddress) / 2, MIN_LENGTH);
        part -= count * sizeof(MAddress);
        push(part, count);
    }
    for (MAddress field = start; field < middleStart; field += sizeof(MAddress)) {
        visit(field);
    }
}

void FollowObjectReferences(BaseObject* object, bool finalizable,
                            const FieldVisitor& visit, const EntryPublisher& publish)
{
    if (object->GetTypeInfo()->IsRawArray()) {
        MArray* array = reinterpret_cast<MArray*>(object);
        TypeInfo* component = array->GetComponentTypeInfo();
        if (component->IsObjectType() || component->IsArrayType() || component->IsInterface()) {
            // zMark.cpp:346-368: array following does not contain a safe
            // iterator split. Invisible roots carry DontFollow upstream.
            FollowElements(reinterpret_cast<MAddress>(array->ConvertToCArray()), array->GetLength(), finalizable, visit, publish);
            return;
        }
    }
    auto fields = [&](RefField<>& field) { visit(reinterpret_cast<MAddress>(&field)); };
    ZBasicOopIterateClosure<decltype(fields)> closure(fields);
    ZIterator::oop_iterate(object, &closure);
}

void FollowPartialReferences(const MarkStackEntry& entry,
                             const FieldVisitor& visit, const EntryPublisher& publish)
{
    MAddress start = 0;
    size_t length = 0;
    Decode(entry, start, length);
    FollowElements(start, length, entry.finalizable(), visit, publish);
}

}
}

namespace MapleRuntime {

}

#include "Heap/z/zMark.inline.hpp"


