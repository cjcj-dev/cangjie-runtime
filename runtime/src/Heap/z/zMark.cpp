// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/WCollector/WCollector.h"

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
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.inline.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zUncoloredRoot.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
bool WCollector::MarkObject(BaseObject* obj) const
{
    return MarkObjectImpl(obj, false);
}

bool WCollector::MarkObjectImpl(BaseObject* obj, bool youngClaim, MarkLiveCache* liveCache) const
{
    (void)youngClaim;
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(obj));

    size_t objectSize = obj->GetSize();
    // ZPage::mark_object (zPage.inline.hpp:284-294) followed by the caller's
    // inc_live (zMark.cpp:417-425): per worker through the ZMarkCache when one
    // is supplied, otherwise straight onto the page.
    bool firstLive = false;
    bool marked = !region->mark_object(from_object(obj), false, firstLive);
    if (firstLive) {
        if (liveCache != nullptr) {
            liveCache->IncLive(region, objectSize);
        } else {
            region->inc_live(1, objectSize);
        }
    }
    if (!marked) {
        DLOG(TRACE, "mark obj %p<%p>(%zu) in region %p(%u)@%#zx, live %zu", obj, obj->GetTypeInfo(), objectSize,
             region, 0u, region->GetRegionStart(), region->live_bytes());
    }
    return marked;
}

bool WCollector::ResurrectObject(BaseObject* obj, size_t offset, ZPage* region)
{
    (void)offset;
    // ZPage::mark_object(addr, finalizable = true) + inc_live on the first claim.
    bool firstLive = false;
    bool resurrected = !region->mark_object(from_object(obj), true, firstLive);
    if (firstLive) {
        region->inc_live(1, obj->GetSize());
    }
    if (!resurrected) {
        DLOG(TRACE, "resurrect region %p@%#zx obj %p<%p>(%zu), live bytes %zu", region, region->GetRegionStart(),
             obj, obj->GetTypeInfo(), obj->GetSize(), region->live_bytes());
    }
    return resurrected;
}
// RefFieldRoot is root in tagged pointer format.
void WCollector::EnumRefFieldRoot(RefField<>& field, RootSet& rootSet) const
{
    RefField<> oldField(field);
    // The major root iterator bypasses ReadStaticRef. Enforce the same
    // colored-carrier contract before its mark-good shortcut or RootSet push
    // (ZPointer::assert_is_valid, zAddress.inline.hpp:320-393).
    // Non-heap ELF literals are not GC roots and keep the skip below.
    CHECK_DETAIL(!Heap::IsHeapAddress(to_object(oldField.GetTargetObject())) ||
                     (raw(oldField.GetFieldValue()) &
                      (ZPointerRemappedMask | ZPointerMarkedYoungMask | ZPointerMarkedOldMask)) != 0,
                 "NativeSlot requires colored value at EnumRefFieldRoot slot=%p word=%#zx",
                 &field, raw(oldField.GetFieldValue()));
    // A mark-good root has passed this mark epoch and is necessarily load-good
    // (OpenJDK zAddress.inline.hpp:658-664).
    if (ZPointer::is_mark_good(oldField.GetFieldValue())) {
        // Anchor main 8cd248497dd8c251ca824d9f089d5e30125c80c9
        BaseObject* target = to_object(oldField.GetTargetObject());
        // Reject non-heap: do not call make_load_good (remap would touch non-heap).
        if (!Heap::IsHeapAddress(target)) {
            return;
        }
        CHECK_DETAIL(target->IsValidObject(), "Enum static root %p(%p) encounters invalid object", target, &field);
        rootSet.push_back(MarkStackEntry(untype(ZAddress::offset(from_object(target))), true, true, true, false));
        return;
    }

    // tracecov: is the mark's field walk broad enough to be the thing that keeps colours fresh?
    // The mark-good fast path above returns without healing, which is correct because mark-good
    // implies load-good; a stale field is therefore mark-bad and reaches the code below, which does
    // heal (CAS at the end of this function).  So "stale slots survive" reduces to "the mark
    // never visited that field".  Counting how much this path actually runs is the cheapest way to
    // tell a narrow walk from a broad one -- and unlike IsMarkedObject<Old>, a counter here is
    // valid at any phase (the mark-bitmap query answered 0 for 4.2M live objects at barrier time,
    // which is why that measurement was void).
    {
        static std::atomic<uint64_t> slowEnter{ 0 };
        const uint64_t n = slowEnter.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0) {
            LOG(RTLOG_ERROR, "[TRACECOV] slow_enter=%lu", n);
        }
    }

    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &field };
    BaseObject* latest = make_load_good(oldField, provenance);

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Enum static root %p(%p) encounters invalid object", latest, &field);
    // static roots stay Phase-C coloured (writable statics need colour; rostatic skips non-heap CAS).
    // plainroots only applies to stack/reg ObjectRef slots (RootSlotWriteback via !IsHeapAddress).
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, raw(oldField.GetFieldValue()), latest,
             latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &field, raw(oldField.GetFieldValue()),
             raw(newField.GetFieldValue()), latest, latest->GetTypeInfo(), latest->GetSize());
    } else {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, raw(oldField.GetFieldValue()), latest,
             latest->GetTypeInfo(), latest->GetSize());
    }
    rootSet.push_back(MarkStackEntry(untype(ZAddress::offset(from_object(latest))), true, true, true, false));
}

void WCollector::EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet, Generation generation) const
{
    zaddress_unsafe observed = ref.LoadPlain();
    if (is_null(observed)) {
        return;
    }

    // ZUncoloredRoot supplies an address, never a colored HeapSlot word.
    BaseObject* root = to_object(safe(observed));
    if (root == nullptr || !Heap::IsHeapAddress(root)) {
        return;
    }
    if (IsGhostFromObject(root)) {
        const ForwardingProvenance provenance{ ForwardingHolderKind::StackSlot, this, &ref };
        BaseObject* to = FindToVersion(root, generation).GetOrFailClosed(
            "WCollector::MarkStackRoots", provenance);
        if (to != nullptr) {
            root = to;
        }
    }
    CHECK_DETAIL(root->IsValidObject(), "Enum and tag runtime root %p(%p) encounters invalid object", root, &ref);
    ZUncoloredRoot::process_no_keepalive(reinterpret_cast<zaddress_unsafe*>(&ref), ZPointerLoadGoodMask);
    rootSet.push_back(MarkStackEntry(untype(ZAddress::offset(from_object(root))), true, true, true, false));
}

// ZMarkOopClosure::do_oop, zMark.cpp:198-205. The field barrier owns
// remapping and generation routing; the original colored slot is authoritative.
void WCollector::TraceRefField(BaseObject* obj, RefField<>& field, WorkStack&, bool finalizable) const
{
    ZBarrier::MarkBarrierOnOldOopField(obj, field, finalizable);
}

// Ported from ZGC ZMark::push_partial_array (zMark.cpp:185-196): the heap
// offset is bounded, so the descriptor always fits one entry word.
void WCollector::PushPartialArray(RefField<>* addr, size_t length, WorkStack& workStack, bool finalizable) const
{
    workStack.push_back(MarkPartialArray::Encode(addr, length, finalizable));
}

// zMark.cpp:208-214 (follow_array_elements_small).
void WCollector::FollowArrayElementsSmall(BaseObject* holder, RefField<>* addr, size_t length,
                                          WorkStack& workStack, bool finalizable) const
{
    for (size_t i = 0; i < length; ++i) {
        TraceRefField(holder, addr[i], workStack, finalizable);
    }
}

// zMark.cpp:216-255 (follow_array_elements_large), transcribed.
void WCollector::FollowArrayElementsLarge(BaseObject* holder, RefField<>* addr, size_t length,
                                          WorkStack& workStack, bool finalizable) const
{
    RefField<>* const start = addr;
    RefField<>* const end = start + length;

    // Calculate the aligned middle start/end/size, where the middle start
    // should always be greater than the start (hence the +1 below) to make
    // sure we always do some follow work, not just split the array into pieces.
    RefField<>* const middleStart = AlignUp(start + 1, MarkPartialArray::MIN_SIZE);
    const size_t middleLength =
        AlignDown(static_cast<size_t>(end - middleStart), MarkPartialArray::MIN_LENGTH);
    RefField<>* const middleEnd = middleStart + middleLength;

    // Push unaligned trailing part
    if (end > middleEnd) {
        PushPartialArray(middleEnd, static_cast<size_t>(end - middleEnd), workStack, finalizable);
    }

    // Push aligned middle part(s)
    RefField<>* partialAddr = middleEnd;
    while (partialAddr > middleStart) {
        const size_t parts = 2;
        const size_t partialLength = AlignUp(static_cast<size_t>(partialAddr - middleStart) / parts,
                                             MarkPartialArray::MIN_LENGTH);
        partialAddr -= partialLength;
        PushPartialArray(partialAddr, partialLength, workStack, finalizable);
    }

    // Follow leading part
    CHECK_DETAIL(start < middleStart, "Miscalculated middle start");
    FollowArrayElementsSmall(holder, start, static_cast<size_t>(middleStart - start), workStack, finalizable);
}

// zMark.cpp:257-263 (follow_array_elements).
void WCollector::FollowArrayElements(BaseObject* holder, RefField<>* addr, size_t length,
                                     WorkStack& workStack, bool finalizable) const
{
    MarkPartialArray::FollowElements(reinterpret_cast<MAddress>(addr), length, finalizable,
        [this, holder, &workStack, finalizable](MAddress slot) {
            TraceRefField(holder, HeapSlotAt<>(slot), workStack, finalizable);
        }, [&workStack](const MarkStackEntry& entry) { workStack.push_back(entry); });
}

// zMark.cpp:265-270 (follow_partial_array).
void WCollector::FollowPartialArray(const MarkStackEntry& entry, WorkStack& workStack)
{
    MAddress chunkStart = 0;
    size_t length = 0;
    MarkPartialArray::Decode(entry, chunkStart, length);
    FollowArrayElements(nullptr, &HeapSlotAt<>(chunkStart), length, workStack, entry.finalizable());
}

void WCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack, bool finalizable)
{

    auto visitor = [this, obj, &workStack, finalizable](RefField<>& field) {
        TraceRefField(obj, field, workStack, finalizable);
    };
    TypeInfo* typeInfo = obj->GetTypeInfo();
    if (!typeInfo->HasRefField()) {
        return;
    }

    if (UNLIKELY(typeInfo->IsRawArray())) {
        MArray* array = reinterpret_cast<MArray*>(obj);
        MIndex arrayLength = array->GetLength();
        TypeInfo* componentTypeInfo = array->GetComponentTypeInfo();
        if (componentTypeInfo->IsStructType()) {
            GCTib gcTib = componentTypeInfo->GetGCTib();
            MAddress contentAddr = reinterpret_cast<Uptr>(array) + MArray::GetContentOffset();
            size_t elementSize = array->GetElementSize();
            for (MIndex i = 0; i < arrayLength; ++i) {
                gcTib.ForEachBitmapWord(contentAddr, visitor);
                contentAddr += elementSize;
            }
        } else if (componentTypeInfo->IsObjectType() || componentTypeInfo->IsArrayType() ||
                   componentTypeInfo->IsInterface()) {
            HeapSlot<>* arrayContent = &HeapSlotAt<>(array->ConvertToCArray());
            // This is ZGC's objArrayOop case (zMark.cpp:346-369 follow_array_object):
            // a flat run of reference slots, the only shape it chunks. The struct
            // -component branch above has no ZGC counterpart and is left alone.
            FollowArrayElements(obj, arrayContent, arrayLength, workStack, finalizable);
        } else {
            LOG(RTLOG_FATAL, "array object %p has wrong component type", array);
        }
        return;
    }

    // zMark.cpp:371-388: non-array following uses the unsafe oop iterator.
    ZBasicOopIterateClosure<decltype(visitor)> closure(visitor);
    ZIterator::oop_iterate(obj, &closure);
}

BaseObject* WCollector::GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field)
{
    RefField<> oldField(field);
    const char* sourceKind = kind == RefSlotKind::WEAK_REFERENT ? "weak" : "strong";
    BaseObject* latest = nullptr;
    if (ZPointer::is_mark_good(oldField.GetFieldValue())) {
        BaseObject* targetObj = to_object(oldField.GetTargetObject());
        if (!Heap::IsHeapAddress(targetObj)) {
            return nullptr;
        }
        // Anchor main ced6b14fe41380fd2dfb94c91b7fe6973786a80e
        CHECK_DETAIL(targetObj->IsValidObject(),
                     "Invalid object %p is referenced by %s object %p: %s and offset %zd", targetObj, sourceKind, obj,
                     obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
        return targetObj;
    }
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, obj, &field };
    latest = make_load_good(oldField, provenance);
    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return nullptr;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by %s object %p: %s and offset %zd",
                 latest, sourceKind, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, raw(oldField.GetFieldValue()),
            raw(newField.GetFieldValue()), latest, latest->GetTypeInfo(), latest->GetSize());
    }
    return latest;
}
void WCollector::TraceHeap()
{
    ZBreakpoint::AtAfterMarkingStarted();
    WorkStack workStack = NewWorkStack();
    WorkStack foreignStack = NewWorkStack();
    MarkingStacks::VerifyEmpty(workStack.size());
    MarkingStacks::VerifyEmpty(foreignStack.size());
    const bool concurrentStackScan = MutatorManager::ConcurrentStackScanEnabled();
    uint64_t stackScanEpoch = 0;

    // Old mark-start belongs to the preceding young pause. The old body
    // begins with concurrent roots/follow (zGeneration.cpp:1015-1020).
    if (concurrentStackScan) {
        ScopedStopTheWorld stw("major stack scan prepare", false);
        ZVerify::BeforeZOperation();
        Heap::GetHeap().SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_ENUM);
    }

    if (concurrentStackScan) {

        EpochHandshakeStats handshake = MutatorManager::Instance().RunEpochHandshake("pre-major-stack", false);
        stackScanEpoch = handshake.epoch;
        CHECK_DETAIL(stackScanEpoch != 0 && handshake.stackScanned + handshake.stackFallback == handshake.requested,
                     "major concurrent stack scan accounting failed: epoch=%llu requested=%zu scanned=%zu "
                     "fallback=%zu",
                     static_cast<unsigned long long>(stackScanEpoch), handshake.requested, handshake.stackScanned,
                     handshake.stackFallback);
    }

    {
        MRT_PHASE_TIMER(ZStatPhases::PEnumRootsUpdateOldPointersWithin);
        if (concurrentStackScan) {
            // This is major's root-enumeration closing edge. StopTheWorld establishes
            // InSaferegion for the fixed mutator roster, so WM_OWNER_GC may finish a
            // different mutator's epoch cursor. If completion still cannot be
            // established, run the legacy enum but leave the watermark incomplete;
            // the report-only postcondition below must observe that residual state.
            {
                ScopedStopTheWorld stw("major stack scan close", false);
                ZVerify::BeforeZOperation();
                TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
                MutatorManager::Instance().VisitAllMutators([stackScanEpoch](Mutator& mutator) {
                    if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                        (void)mutator.GcPhaseEnum(GCPhase::GC_PHASE_ENUM, false, stackScanEpoch, false);
                    }
                    if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                        (void)mutator.GcPhaseEnum(GCPhase::GC_PHASE_ENUM, false);
                    }
#if defined(MRT_GC_UNIT_TESTS)
                    NoteLargeArrayInitRootPhase(LargeArrayRootPhase::MAJOR_MARK, &mutator,
                                                mutator.GetStackWatermark().IsDone(stackScanEpoch));
#endif
                });
                // CLEAR freezes further ENUM pushes before releasing the
                // mutator-list lock owned by StopTheWorld. DoEnumeration cannot
                // run inside this scope: MergeMutatorRoots takes that same
                // non-recursive write lock.
            }

            // Merge mutator alloc-buffer roots before declaring enumeration closed.
            // Mutators are under the TRACE barrier's CLEAR phase, but DoTracing has
            // not started; this is the last point at which an incomplete stack-root
            // receipt can be reported before any mark-closure work consumes the roots.
            DoEnumeration(workStack, foreignStack);

            TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        } else {
            TransitionToGCPhase(GCPhase::GC_PHASE_ENUM, true, false);
            DoEnumeration(workStack, foreignStack);
        }
    }

    {
        MRT_PHASE_TIMER(ZStatPhases::PTraceLiveObjectsUpdateOldPointersInRefFields);
        markedObjectCount.store(0, std::memory_order_relaxed);
        if (!concurrentStackScan) {
            TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        }
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        DoTracing(workStack, foreignStack);
        if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
            return;
        }

        MarkingStacks::VerifyEmpty(workStack.size());
        MarkingStacks::VerifyEmpty(foreignStack.size());

    }

}
namespace {
// gcbadroot: tag which root family is currently being walked so PushYoungObject
// can attribute invalid headers without threading origin through every visitor.
thread_local const char* gMinorRootOrigin = "unknown";
} // namespace

void WCollector::VisitMinorRootSlots(RootVisitor& rawRootVisitor, RootVisitor& invisibleRootVisitor,
                                     uint64_t stackScanEpoch)
{
#if defined(MRT_GC_UNIT_TESTS)
    RootVisitor observedInvisibleRootVisitor = [&invisibleRootVisitor](ObjectRef& root) {
        NoteLargeArrayInitRootVisit(LargeArrayRootVisitSite::MINOR_MARK,
                                    to_object(safe(root.LoadPlain(std::memory_order_acquire))));
        invisibleRootVisitor(root);
    };
    RootVisitor& visitedInvisibleRootVisitor = observedInvisibleRootVisitor;
#else
    RootVisitor& visitedInvisibleRootVisitor = invisibleRootVisitor;
#endif
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    RootVisitor checkedRawRootVisitor = [&remset, &rawRootVisitor](ObjectRef& root) {
        remset.VisitStaticForCrossCheck(reinterpret_cast<MAddress>(&root));
        rawRootVisitor(root);
    };
    RootVisitor& visitedRawRootVisitor = checkedRawRootVisitor;
#else
    RootVisitor& visitedRawRootVisitor = rawRootVisitor;
#endif
    gMinorRootOrigin = "mutator_stack";
    size_t concurrentDone = 0;
    size_t stwFallback = 0;
    VisitStrongPlainRoots(visitedRawRootVisitor, [&](Mutator& mutator) {
        bool watermarkDone =
            stackScanEpoch != 0 && mutator.GetStackWatermark().IsDone(stackScanEpoch);
#if defined(MRT_GC_UNIT_TESTS)
        NoteLargeArrayInitRootPhase(LargeArrayRootPhase::MINOR_MARK, &mutator, watermarkDone);
#endif
        if (watermarkDone) {
            ++concurrentDone;
            return;
        }
        if (stackScanEpoch != 0) {
            ++stwFallback;
        }
        mutator.VisitMutatorRoots(visitedRawRootVisitor, visitedInvisibleRootVisitor);
    });
    if (stackScanEpoch != 0) {
        LOG(RTLOG_ERROR,
            "[GCV2][stack-scan-fallback] epoch=%llu concurrent_done=%zu stw_fallback=%zu "
            "stack_scan=required",
            static_cast<unsigned long long>(stackScanEpoch), concurrentDone, stwFallback);
    }
    gMinorRootOrigin = "unknown";
}

void WCollector::VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes, Generation::Young);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, Generation::Young);
        gMinorRootOrigin = "value_export";
        for (BaseObject* object : resurrectedExportObjectes) {
            visitor(object);
        }
        gMinorRootOrigin = "value_export_fwd";
        for (BaseObject* object : resurrectedExportObjectesForwardPhase) {
            visitor(object);
        }
    }
    std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
    CurrentizeValueRootMap(cycleRefWorkStack, Generation::Young);
    gMinorRootOrigin = "value_cycle";
    for (const auto& entry : cycleRefWorkStack) {
        visitor(entry.first);
        for (BaseObject* object : entry.second) {
            visitor(object);
        }
    }
    gMinorRootOrigin = "unknown";
}

// ZReferenceProcessor::should_discover/discover (zReferenceProcessor.cpp:174-201,
// 239-250). Native registration owns the original referent slot, rather than a
// Java FinalReference object. The load barrier heals remapping before discovery.
void TracingCollector::DiscoverFinalizableRoot(NativeSlot& slot) const
{
    CHECK(oldCycle.IsPhaseMark());
    BaseObject* object = ZBarrier::ReadStaticRef(slot);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &slot };
    object = ValidateCurrentValue(object, provenance);
    if (object == nullptr) return;
    auto* page = Heap::page(reinterpret_cast<MAddress>(object));
    if (page->IsYoungRegion() || page->is_object_strongly_live(from_object(object))) return;
    auto& processor = collectorResources.GetFinalizerProcessor().GetReferenceProcessor();
    const auto status = processor.DiscoverReference(object, ReferenceType::FINAL);
    CHECK(status == ReferenceStatus::DISCOVERED || status == ReferenceStatus::ALREADY_DISCOVERED);
    ZBarrier::MarkFinalizableBarrierOnRoot(slot);
}

namespace {
// ZMarkOopClosure (zMark.cpp:666-670). P08 owns the missing dedicated old
// mark barrier; this adapter consumes the existing old publication producer.
class MarkOopClosure {
public:
    explicit MarkOopClosure(const TracingCollector& collector) : collector(collector) {}
    void DoOop(NativeSlot& slot) const
    {
        BaseObject* object = ZBarrier::ReadStaticRef(slot);
        collector.MarkOldObjectIfActive(object);
    }
private:
    const TracingCollector& collector;
};

// ZMarkOldRootsTask, zMark.cpp:797-834. Root results are published to the
// generation mark domain by closures, then flushed by each participating worker.
class MarkOldRootsTask final : public ZTask {
public:
    MarkOldRootsTask(const TracingCollector& collector, ZMark& domain,
                     NativeSlotVisitor finalizable, std::function<void()> uncolored, unsigned workers)
        : ZTask("ZMarkOldRootsTask"), rootsColored(collector, workers),
          finalizerRoots(Heap::GetHeap().GetFinalizerProcessor().WeakRootStorage(), workers),
          finalizable(std::move(finalizable)), coloredClosure(collector), domain(domain), uncolored(std::move(uncolored)) {}
    void work() override
    {
        finalizerRoots.OopsDo(finalizable);
        rootsColored.Apply([&](NativeSlot& slot) {
            coloredClosure.DoOop(slot);
#if defined(MRT_TESTABLE_INTERNALS)
            if (TracingCollector::testColoredRootResult) {
                TracingCollector::testColoredRootResult(GCCycleGeneration::OLD, &slot);
            }
#endif
        });
        rootsUncolored.Apply(uncolored);
        // zMark.cpp:830-834: flush and free worker stacks for both generations
        // here, since the set of workers executing during root scanning can be
        // different from the set of workers executing during mark.
        ThreadLocal::FlushCurrentThreadMarkStacks();
#if defined(MRT_TESTABLE_INTERNALS)
        if (TracingCollector::testColoredRootResult) {
            TracingCollector::testColoredRootResult(GCCycleGeneration::OLD, nullptr);
        }
#endif
    }
private:
    RootsIteratorStrongColored rootsColored;
    OopStorage::ParState<true> finalizerRoots;
    NativeSlotVisitor finalizable;
    RootsIteratorStrongUncolored rootsUncolored;
    MarkOopClosure coloredClosure;
    ZMark& domain;
    std::function<void()> uncolored;
};
} // namespace

void TracingCollector::EnumAllCommonRoots(ZWorkers& workers)
{
    CHECK_DETAIL(majorMark != nullptr, "old mark domain must start before roots");
    MarkOldRootsTask task(*this, *majorMark,
                         [this](NativeSlot& slot) { DiscoverFinalizableRoot(slot); }, [&] {
        VisitStrongPlainRoots([&](ObjectRef& root) {
            MarkOldObjectIfActive(to_object(safe(root.LoadPlain())));
        }, {});
        VisitSurrectedExportRoots([&](BaseObject* object) { MarkOldObjectIfActive(object); });
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
    MarkYoungRootsTask(const TracingCollector& collector, std::function<void()> uncolored, unsigned workers)
        : ZTask("ZMarkYoungRootsTask"), rootsColored(collector, workers), uncolored(std::move(uncolored)) {}

    void work() override
    {
        rootsColored.Apply([this](NativeSlot& slot) {
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
            Heap::GetHeap().GetRememberedSet().VisitStaticForCrossCheck(reinterpret_cast<MAddress>(&slot));
#endif
            coloredClosure.DoOop(slot);
#if defined(MRT_TESTABLE_INTERNALS)
            if (TracingCollector::testColoredRootResult) {
                TracingCollector::testColoredRootResult(GCCycleGeneration::YOUNG, &slot);
            }
#endif
        });
        rootsUncolored.Apply(uncolored);
        // zMark.cpp:887-891: flush and free worker stacks for both generations.
        ThreadLocal::FlushCurrentThreadMarkStacks();
#if defined(MRT_TESTABLE_INTERNALS)
        if (TracingCollector::testColoredRootResult) {
            TracingCollector::testColoredRootResult(GCCycleGeneration::YOUNG, nullptr);
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

void WCollector::VisitMinorRoots(const std::function<void(BaseObject*)>& visitor,
                                 const std::function<void(BaseObject*)>& invisibleVisitor,
                                 uint64_t stackScanEpoch)
{
    RootVisitor rawRootVisitor = [this, &visitor](ObjectRef& root) {
        BaseObject* obj = ResolveMinorReference(root);
        visitor(obj);
    };
    RootVisitor invisibleRootVisitor = [this, &invisibleVisitor](ObjectRef& root) {
        BaseObject* obj = ResolveMinorReference(root);
        invisibleVisitor(obj);
    };
    MarkYoungRootsTask task(*this, [&] {
        VisitMinorRootSlots(rawRootVisitor, invisibleRootVisitor, stackScanEpoch);
        VisitMinorValueRoots(visitor);
    }, GetWorkers(GCCycleGeneration::YOUNG).active_workers());
    GetWorkers(GCCycleGeneration::YOUNG).run(&task);
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    Heap::GetHeap().GetRememberedSet().CheckStaticCoverageForMinor();
#endif
}

void WCollector::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin) const
{
    PushYoungObject(object, workStack, origin, false);
}

void WCollector::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin,
                                  bool finalizable) const
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
        if (GetGenerationCycle(GCCycleGeneration::YOUNG).IsMajorRoots()) {
            MarkOldObjectIfActive(object, true);
        }
        return;
    }
    if (region->IsYoungRegion() && !region->is_object_marked(from_object(object), finalizable)) {
        // ZMark::mark_object gc_thread arm (zMark.inline.hpp:58-73): mark before
        // push; the first-live claim travels with the entry.
        bool firstLive = false;
        const bool already = !region->mark_object(from_object(object), finalizable, firstLive);
        if (!already) {
            workStack.push_back(MarkStackEntry(untype(ZAddress::offset(from_object(object))), false, firstLive, true, finalizable));
        }
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

namespace WCollectorInternal {
void PushAdmittedYoung(BaseObject* object, TracingCollector::WorkStack& workStack, const char* origin,
                       const void* slot, BaseObject* holder)
{
    BaseObject* admitted = AdmitYoungObject(object, origin, slot, holder);
    if (admitted != nullptr) {
        workStack.push_back(MarkStackEntry(untype(ZAddress::offset(from_object(admitted))), true, true, true, false));
    }
}

void PushAdmittedYoung(const MarkStackEntry& entry, TracingCollector::WorkStack& workStack, const char* origin,
                       const void* slot, BaseObject* holder)
{
    BaseObject* admitted =
        AdmitYoungObject(to_object(ZOffset::address(to_zoffset(entry.object_address()))), origin, slot, holder);
    if (admitted != nullptr) {
        workStack.push_back(MarkStackEntry(untype(ZAddress::offset(from_object(admitted))), entry.mark(), entry.inc_live(), entry.follow(),
                                           entry.finalizable()));
    }
}
} // namespace WCollectorInternal

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

namespace WCollectorInternal {
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
    Collector::FailClosedLoad(
        "WCollector::ScrubMinorFreeTarget.unresolved", target, oldVal,
        ForwardingProvenance{ ForwardingHolderKind::Remset, nullptr, &field });
}

} // namespace WCollectorInternal


struct alignas(64) YoungStripedWorkerOutput {
    std::vector<BaseObject*> objects;
    std::vector<MAddress> slots;
    std::vector<MAddress> weaks;
    size_t objectsMarked = 0;
    bool touched = false;
};

struct YoungStripedShared {
    WCollector* collector = nullptr;
    const std::unordered_set<MAddress>* reachableSlotDomain = nullptr;
    bool fullYoungScan = false;
    bool recordSlots = false;
    bool partial = false;
    size_t workerCount = 0;
    ZMark* domain = nullptr;
    std::vector<std::unique_ptr<YoungStripedWorkerOutput>> outputs;
    std::atomic<size_t> stealSuccess{ 0 };
    std::atomic<size_t> stealFailure{ 0 };

    MarkStripeSet& Stripes() { return domain->Stripes(); }
    MarkingSMR& Smr() { return domain->Smr(); }
    MarkTerminate& Terminate() { return domain->Terminate(); }
    MarkThreadLocalStacks& Stacks() { return domain->Stacks(); }

    size_t StripeFor(BaseObject* object) const
    {
        return domain->Stripes().StripeForAddress(reinterpret_cast<uintptr_t>(object));
    }
};

// ZMarkTask (zMark.cpp:895-923): a restartable task whose work() reads the
// thread-local worker id and flushes both generations' stacks on exit.
class YoungStripedMarkingWork : public ZRestartableTask {
public:
    explicit YoungStripedMarkingWork(YoungStripedShared& shared) : ZRestartableTask("ZMarkTask"), shared(shared) {}

    void resize_workers(uint32_t workers) override
    {
        shared.workerCount = workers;
        shared.domain->ResizeWorkers(workers);
        while (shared.outputs.size() < workers) {
            shared.outputs.emplace_back(std::make_unique<YoungStripedWorkerOutput>());
        }
    }

    void work() override
    {
        const uint32_t workerId = WorkerThread::worker_id();
        MarkContext local(shared.workerCount, workerId, shared.Stripes(), shared.Stacks());
        size_t nMarked = 0;
        (void)ZMark::FollowWork(local, shared.Smr(), shared.Stripes(), shared.Terminate(), workerId,
                                     shared.partial,
                                     [this, workerId, &nMarked, &local](const MarkStackEntry& entry) {
                                         shared.outputs[workerId]->touched = true;
                                         ProcessObject(local, workerId, entry, nMarked);
                                     },
                                     &shared.stealSuccess, &shared.stealFailure, shared.domain);
        (void)local.Stacks().Flush(shared.Stripes(), true);
        local.Cache().Flush();
        shared.outputs[workerId]->objectsMarked += nMarked;
        MarkingStacks::VerifyEmpty(local.Stacks().Population());
        // zMark.cpp:909-918: we might have found pointers into the other
        // generation; publish both generations' stacks before this worker
        // reports completion, also in case a resize changes the worker set.
        ThreadLocal::FlushCurrentThreadMarkStacks();
    }

private:

    void PublishEntry(MarkContext& ctx, const MarkStackEntry& entry)
    {
        MAddress address = 0;
        if (entry.partial_array()) {
            size_t length = 0;
            MarkPartialArray::Decode(entry, address, length);
        } else {
            address = reinterpret_cast<MAddress>(to_object(ZOffset::address(to_zoffset(entry.object_address()))));
        }
        const size_t stripeIndex = shared.StripeFor(reinterpret_cast<BaseObject*>(address));
        const bool publish = stripeIndex != ctx.StripeId();
        ctx.Stacks().Push(shared.Stripes(), stripeIndex, entry, publish);
        if (publish) {
            shared.Terminate().Wake();
        }
    }

    void ProcessObject(MarkContext& ctx, size_t workerId, const MarkStackEntry& entry, size_t& nMarked)
    {
        YoungStripedWorkerOutput& output = *shared.outputs[workerId];
        auto visitSlot = [this, &ctx, &output, &entry](MAddress slot) {
            if (shared.recordSlots &&
                (shared.reachableSlotDomain == nullptr || shared.reachableSlotDomain->count(slot) != 0)) {
                output.slots.push_back(slot);
            }
            auto& field = HeapSlotAt<>(slot);
            ZBarrier::MarkBarrierOnYoungOopField(field);
        };
        auto publish = [this, &ctx](const MarkStackEntry& work) { PublishEntry(ctx, work); };
        if (entry.partial_array()) {
            MarkPartialArray::FollowPartialReferences(entry, visitSlot, publish);
            return;
        }
        BaseObject* object = to_object(ZOffset::address(to_zoffset(entry.object_address())));
        if (!Heap::IsHeapAddress(object)) {
            return;
        }
        auto& localObjects = output.objects;
        WCollector* collector = shared.collector;
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
        const bool isYoung = region->IsYoungRegion();

        if (isYoung) {
            bool wasMarked = collector->MarkEntryObject(object, entry, &ctx.Cache());
            if (entry.mark() && wasMarked) {
                return;
            }
            if (entry.mark()) {
                ++nMarked;
            }
            CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
            localObjects.push_back(object);
        } else {
            return;
        }
        if (!object->HasRefField()) {
            return;
        }
        if (!entry.follow()) {
            return;
        }
        // ZReferenceProcessor::should_discover rejects young references
        // (zReferenceProcessor.cpp:175-185). Their referents are followed
        // strongly by the ordinary object-field closure.
        MarkPartialArray::FollowObjectReferences(object, entry.finalizable(), visitSlot, publish);
    }

    YoungStripedShared& shared;
};

void WCollector::StartYoungMarkWork()
{
    if (youngMark == nullptr) {
        youngMark = std::make_unique<ZMark>(kMarkStripeMax, MarkingStacks::MarkingGeneration::YOUNG);
    }
    ZWorkers& workers = GetWorkers(GCCycleGeneration::YOUNG);
    youngMark->BindWorkers(&workers);
    youngMark->BindAbort(&collectorResources.GetYoungDriverPort().Abort());
    youngMark->Start();
    youngCycle.BindMark(youngMark.get());
    MarkingStacks::VerifyEmpty(youngMark->Stripes().Population());
}

void WCollector::MarkYoungObjectIfActive(BaseObject* object) const
{
    const GCCycleSnapshot young = GetCycleSnapshot(GCCycleGeneration::YOUNG);
    if (!young.active || (young.phase != GC_PHASE_ENUM && young.phase != GC_PHASE_TRACE &&
                          young.phase != GC_PHASE_CLEAR_SATB_BUFFER)) {
        return;
    }
    if (!Heap::IsHeapAddress(object) || IsMarkedObject<Generation::Young>(object)) {
        return;
    }
    CHECK_DETAIL(youngMark != nullptr, "young mark domain must start before publication");
    MarkStripeSet& stripes = youngMark->Stripes();
    MarkThreadLocalStacks& publication = ThreadLocal::GetMarkStacks(*youngMark);
    publication.Push(stripes, stripes.StripeForAddress(reinterpret_cast<uintptr_t>(object)),
                     MarkStackEntry(untype(ZAddress::offset(from_object(object))), true, true, true, false), true);
}

void WCollector::TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,
                                          std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                          MinorSlotSet& weakSlots,
                                          const MinorSlotSet* reachableSlotDomain)
{
    g_markStripeArmed.fetch_add(1, std::memory_order_relaxed);
    const size_t dispelAtEntry = ZPage::GetDispelGhostCount();

    ZWorkers& workersSet = GetWorkers(GCCycleGeneration::YOUNG);
    const size_t workers = workersSet.active_workers();
    g_markStripeTurned.fetch_add(1, std::memory_order_relaxed);

    youngMark->PrepareWork(workers);
    const size_t stripeCount = youngMark->Stripes().Count();
    YoungStripedShared shared;
    shared.collector = this;
    shared.reachableSlotDomain = reachableSlotDomain;
    shared.fullYoungScan = fullYoungScan;
    shared.recordSlots = fullYoungScan;
    shared.workerCount = workers;
    shared.domain = youngMark.get();
    shared.outputs.reserve(shared.workerCount);
    for (size_t i = 0; i < shared.workerCount; ++i) {
        shared.outputs.emplace_back(std::make_unique<YoungStripedWorkerOutput>());
    }

    MarkThreadLocalStacks& seed = youngMark->Stacks();
    (void)seed.Flush(shared.Stripes(), true);
    MarkingStacks::VerifyEmpty(seed.Population());
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();
        MAddress address = 0;
        if (entry.partial_array()) {
            size_t length = 0;
            MarkPartialArray::Decode(entry, address, length);
        } else {
            address = reinterpret_cast<MAddress>(to_object(ZOffset::address(to_zoffset(entry.object_address()))));
        }
        seed.Push(shared.Stripes(), shared.StripeFor(reinterpret_cast<BaseObject*>(address)), entry, true);
    }
    // Roots and concurrently published stripes are independent sources of work.

    (void)seed.Flush(shared.Stripes(), true);

    MarkingStacks::VerifyEmpty(seed.Population());

    YoungStripedMarkingWork task(shared);
    workersSet.run(&task);
    youngMark->FinishWork();
    if (!collectorResources.GetYoungDriverPort().Abort().Poll()) {
        MarkingStacks::VerifyEmpty(shared.Stripes().Population());
        CHECK_DETAIL(shared.Terminate().Terminated(),
                     "young striped closure returned without coordinated worker termination");
    }

    const size_t dispelAtExit = ZPage::GetDispelGhostCount();
    CHECK_DETAIL(dispelAtExit == dispelAtEntry,
                 "T-D ghost dispel during striped mark_closure window entry=%zu exit=%zu", dispelAtEntry,
                 dispelAtExit);

    size_t active = 0;
    std::string markedStr;
    for (size_t i = 0; i < shared.outputs.size(); ++i) {
        YoungStripedWorkerOutput& output = *shared.outputs[i];
        active += output.touched ? 1 : 0;
        if (!markedStr.empty()) {
            markedStr += ',';
        }
        markedStr += std::to_string(output.objectsMarked);
        for (BaseObject* object : output.objects) {
            reachableVec.push_back(object);
        }
        for (MAddress slot : output.slots) {
            reachableSlots.insert(slot);
        }
        for (MAddress slot : output.weaks) {
            weakSlots.insert(slot);
        }
    }

    VLOG(REPORT,
         "[GCV2][markpar][striped] workers_active=%zu workers_scheduled=%zu stripes=%zu stripe_shift=%zu "
         "objects_marked=[%s] reachable_n=%zu parallel=1 armed=%zu turned=%zu steal_ok=%zu steal_fail=%zu",
         active, workers, stripeCount, kMarkStripeShift, markedStr.c_str(), reachableVec.size(),
         g_markStripeArmed.load(std::memory_order_relaxed),
         g_markStripeTurned.load(std::memory_order_relaxed),
         shared.stealSuccess.load(std::memory_order_relaxed),
         shared.stealFailure.load(std::memory_order_relaxed));
}

void WCollector::TraceYoungClosure(WorkStack& workStack, bool fullYoungScan,
                                   std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                   MinorSlotSet& weakSlots,
                                   const MinorSlotSet* reachableSlotDomain)
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
    if (workStack.empty() && (youngMark == nullptr || youngMark->Stripes().IsEmpty())) {
        return;
    }

    TraceYoungClosureStriped(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots,
                             reachableSlotDomain);
}

// ZGenerationYoung::concurrent_mark_continue follows the generation's
// published mark work. Young marking has no SATB queue (zBarrier.cpp:160-180).
bool WCollector::FollowYoungMark(WorkStack& workStack, bool fullYoungScan,
                                     std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                     MinorSlotSet& weakSlots,
                                     YoungConcWindowStats* windowStats)
{
    MRT_PHASE_TIMER(ZStatPhases::PYoungMarkFollow);
    // Follow explicit roots and allocation work; young has no SATB queue.
#if defined(MRT_TESTABLE_INTERNALS)
    PublishConcurrentYoungProducersTestReceipt();
#endif
    (void)MutatorManager::Instance().HandshakeFlushMarkProducers(youngMark.get());
    do {
        if (!workStack.empty() || !youngMark->Stripes().IsEmpty()) {
            if (windowStats != nullptr) {
                ++windowStats->closureCalls;
            }
            TraceYoungClosure(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots);
        }
        if (collectorResources.GetYoungDriverPort().Abort().Poll()) {
            return false;
        }
    } while (youngMark->TryTerminateFlush());
    CHECK_DETAIL(workStack.empty(), "young concurrent follow returned with owner work");
    return true;
}

bool WCollector::TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats)
{
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "young mark-end flush requires stopped mutators");
    NoteMarkTerminatePause();
    const size_t before = youngMark->Stripes().Population();
    (void)MutatorManager::Instance().HandshakeFlushMarkProducers(youngMark.get());
    const size_t after = youngMark->Stripes().Population();
    NoteMarkTerminateFlushed(after >= before ? after - before : 0);
    if (!workStack.empty() || !youngMark->Stripes().IsEmpty()) {
        return false;
    }
    // zMark.cpp:973-989: successful mark-end verifies the current generation.
    MarkingStacks::VerifyAllEmpty(*youngMark);
    return true;
}
void WCollector::MarkNewObject(BaseObject* obj)
{
    // Registration follows object initialization (BaseObject::RegisterFinalizer).
    // ZMark::AnyThread / DontFollow: publish mark-only work for this current object.
    auto& cycle = ObjectGeneration(obj) == Generation::Young ? youngCycle : oldCycle;
    cycle.MarkObjectIfActive<false, false, false, false>(from_object(obj));
}

void WCollector::ProcessFinalizers()
{
    FinalizerProcessor& fp = collectorResources.GetFinalizerProcessor();
    fp.ProcessReferences([this](BaseObject* obj) { return IsMarkedObject<Generation::Old>(obj); });
}

bool WCollector::PublishHandshakeMarkWork(WorkStack& work, ZMark* domain)
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

void WCollector::DrainAllocBufferMarkProducers(AllocBuffer* buffer, WorkStack& work, bool young)
{
    if (buffer == nullptr) {
        return;
    }
    if (!young) {
        return;
    }
    buffer->MergeY2yDirtyHolders(work);
    buffer->MergeY2yDirtySlots([this, &work](MAddress slot) {
        RefField<>& field = HeapSlotAt<>(slot);
        BaseObject* target = ResolveMinorReference(field);
        if (target != nullptr && Heap::IsHeapAddress(target)) {
            work.push_back(MarkStackEntry(untype(ZAddress::offset(from_object(target))), true, true, true, false));
        }
    });
}

void WCollector::PublishThreadRoot(BaseObject* object, bool young, bool follow)
{
    ZMark* domain = young ? youngMark.get() : majorMark.get();
    CHECK_DETAIL(domain != nullptr, "root publication requires an active mark domain");
    MarkStripeSet& stripes = domain->Stripes();
    ThreadLocal::GetMarkStacks(*domain).Push(stripes,
        stripes.StripeForAddress(reinterpret_cast<uintptr_t>(object)),
        MarkStackEntry(untype(ZAddress::offset(from_object(object))), true, true, follow, false), true);
}

bool WCollector::FlushGCDataMarkProducers(ThreadGCData& data, ZMark* domain)
{
    return domain != nullptr && data.FlushMarkStacks(*domain);
}

bool WCollector::FlushGCDataMarkProducers(ThreadGCData& data)
{
    const bool young = FlushGCDataMarkProducers(data, youngMark.get());
    return FlushGCDataMarkProducers(data, majorMark.get()) || young;
}

bool WCollector::FlushThreadMarkProducers(ThreadLocalData* tls)
{
    bool published = FlushThreadMarkProducers(tls, youngMark.get());
    return FlushThreadMarkProducers(tls, majorMark.get()) || published;
}

bool WCollector::FlushThreadMarkProducers(ThreadLocalData* tls, ZMark* domain)
{
    if (tls == nullptr || domain == nullptr) {
        return false;
    }
    WorkStack work;
    const bool young = domain->Generation() == MarkingStacks::MarkingGeneration::YOUNG;
    DrainAllocBufferMarkProducers(tls->buffer, work, young);
    const bool published = PublishHandshakeMarkWork(work, domain);
    return ThreadLocal::FlushMarkStacks(tls, *domain) || published;
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
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
struct MajorMarkShared {
    TracingCollector* collector = nullptr;
    size_t workerCount = 0;
    ZMark* domain = nullptr;
    bool partial = false;
    std::atomic<size_t> newlyMarked{ 0 };

    MarkStripeSet& Stripes() { return domain->Stripes(); }
    MarkingSMR& Smr() { return domain->Smr(); }
    MarkTerminate& Terminate() { return domain->Terminate(); }
    MarkThreadLocalStacks& Stacks() { return domain->Stacks(); }

    size_t StripeFor(const MarkStackEntry& entry) const
    {
        MAddress address = 0;
        if (entry.partial_array()) {
            size_t length = 0;
            MarkPartialArray::Decode(entry, address, length);
        } else {
            address = reinterpret_cast<MAddress>(to_object(ZOffset::address(to_zoffset(entry.object_address()))));
        }
        return domain->Stripes().StripeForAddress(address);
    }
};

// ZMarkTask (zMark.cpp:895-923) for the old generation.
class ConcurrentMarkingWork : public ZRestartableTask {
public:
    explicit ConcurrentMarkingWork(MajorMarkShared& shared) : ZRestartableTask("ZMarkTask"), shared(shared) {}

    void resize_workers(uint32_t workers) override
    {
        shared.workerCount = workers;
        shared.domain->ResizeWorkers(workers);
    }

    void work() override
    {
        const uint32_t workerId = WorkerThread::worker_id();
        MarkContext local(shared.workerCount, workerId, shared.Stripes(), shared.Stacks());
        size_t nNewlyMarked = 0;
        TracingCollector::WorkStack staging;
        (void)ZMark::FollowWork(local, shared.Smr(), shared.Stripes(), shared.Terminate(), workerId,
                                     shared.partial,
                                     [this, &nNewlyMarked, &staging, &local](const MarkStackEntry& entry) {
                                         ProcessEntry(local, entry, nNewlyMarked, staging);
                                     },
                                     nullptr, nullptr, shared.domain);
        (void)local.Stacks().Flush(shared.Stripes(), true);
        local.Cache().Flush();
        shared.newlyMarked.fetch_add(nNewlyMarked, std::memory_order_relaxed);
        MarkingStacks::VerifyEmpty(local.Stacks().Population());
        // zMark.cpp:909-918: publish both generations' stacks before this
        // worker reports completion.
        ThreadLocal::FlushCurrentThreadMarkStacks();
    }

private:
    void PublishStaging(MarkContext& ctx, TracingCollector::WorkStack& staging)
    {
        while (!staging.empty()) {
            const MarkStackEntry next = staging.back();
            staging.pop_back();
            const size_t stripeIndex = shared.StripeFor(next);
            const bool publish = stripeIndex != ctx.StripeId();
            ctx.Stacks().Push(shared.Stripes(), stripeIndex, next, publish);
        }
    }

    void ProcessEntry(MarkContext& ctx, const MarkStackEntry& entry, size_t& nNewlyMarked,
                      TracingCollector::WorkStack& staging)
    {
        TracingCollector& collector = *shared.collector;
        if (UNLIKELY(MarkPartialArray::IsPartialArrayEntry(entry))) {
            collector.FollowPartialArray(entry, staging);
            PublishStaging(ctx, staging);
            return;
        }
        BaseObject* obj = to_object(ZOffset::address(to_zoffset(entry.object_address())));
        const bool wasMarked = collector.MarkEntryObject(obj, entry, &ctx.Cache());
        if ((!entry.mark() || !wasMarked) && entry.follow()) {
            if (entry.mark()) {
                nNewlyMarked++;
            }
            if (!obj->HasRefField()) {
                return;
            }
            if (UNLIKELY(obj->IsWeakRef())) {
                collector.DiscoverWeakReference(obj, staging);
            } else {
                collector.TraceObjectRefFields(obj, staging, entry.finalizable());
            }
            PublishStaging(ctx, staging);
        }
    }

    MajorMarkShared& shared;
};


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
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
void TracingCollector::StartOldMarkWork()
{
    // ZGenerationOld::mark_start -> ZMark::start. Initialize the existing M3
    // domain before publishing old's mark phase to mutators and young workers.
    if (majorMark == nullptr) {
        majorMark = std::make_unique<ZMark>(64, MarkingStacks::MarkingGeneration::MAJOR);
    }
    ZWorkers& workers = GetWorkers(GCCycleGeneration::OLD);
    majorMark->BindWorkers(&workers);
    majorMark->BindAbort(&collectorResources.GetMajorDriverPort().Abort());
    majorMark->Start();
    oldCycle.BindMark(majorMark.get());
}

void TracingCollector::MarkOldObjectIfActive(BaseObject* object, bool gcThread) const
{
    const GCCycleSnapshot old = GetCycleSnapshot(GCCycleGeneration::OLD);
    if (!old.active || (old.phase != GC_PHASE_ENUM && old.phase != GC_PHASE_TRACE &&
                        old.phase != GC_PHASE_CLEAR_SATB_BUFFER)) {
        return;
    }
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    const bool marked = gcThread ? MarkObject(object) : IsMarkedObject<Generation::Old>(object);
    if (marked) {
        return;
    }
    // ZMark::mark_object marks before publishing GC-thread work. The entry
    // carries FollowOnly so old workers still traverse an already marked root.
    CHECK_DETAIL(majorMark != nullptr, "old mark domain must start before publication");
    MarkStripeSet& stripes = majorMark->Stripes();
    MarkThreadLocalStacks& publication = ThreadLocal::GetMarkStacks(*majorMark);
    publication.Push(stripes, stripes.StripeForAddress(reinterpret_cast<uintptr_t>(object)),
                     gcThread ? MarkStackEntry(untype(ZAddress::offset(from_object(object))), false, false, true, false)
                              : MarkStackEntry(untype(ZAddress::offset(from_object(object))), true, true, true, false),
                     true);
}

size_t TracingCollector::RunMajorStripeMark(WorkStack& workStack, bool partial)
{
    ZWorkers& workersSet = GetWorkers(GCCycleGeneration::OLD);
    const uint32_t workers = workersSet.active_workers();
    if (majorMark == nullptr) {
        majorMark = std::make_unique<ZMark>(64, MarkingStacks::MarkingGeneration::MAJOR);
    }
    majorMark->BindWorkers(&workersSet);
    majorMark->BindAbort(&collectorResources.GetMajorDriverPort().Abort());
    majorMark->PrepareWork(workers);
    MajorMarkShared shared;
    shared.collector = this;
    shared.workerCount = workers;
    shared.partial = partial;
    shared.domain = majorMark.get();

    MarkThreadLocalStacks& seed = majorMark->Stacks();
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();
        seed.Push(shared.Stripes(), shared.StripeFor(entry), entry, true);
    }
    (void)seed.Flush(shared.Stripes(), true);


    ConcurrentMarkingWork task(shared);
    workersSet.run(&task);
    majorMark->FinishWork();
    if (!partial && !collectorResources.GetMajorDriverPort().Abort().Poll()) {
        CHECK_DETAIL(shared.Terminate().Terminated(),
                     "major striped closure returned without coordinated worker termination");
    }
    return shared.newlyMarked.load(std::memory_order_relaxed);
}

void TracingCollector::TracingImpl(WorkStack& workStack)
{
    // ZMark::mark_follow (zMark.cpp:944-952): join workers, check abort,
    // then flush producers. Stopped stripes never start another follow pass.
    do {
        if (!workStack.empty() || !majorMark->Stripes().IsEmpty()) {
            markedObjectCount.fetch_add(RunMajorStripeMark(workStack), std::memory_order_relaxed);
        }
        if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
            return;
        }
    } while (FlushMarkProducers(majorMark.get()));
}

void TracingCollector::ProcessExportRoots(WorkStack& foreignRootsSet)
{
    while (!foreignRootsSet.empty()) {
        if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
            return;
        }
        const MarkStackEntry entry = foreignRootsSet.back();
        foreignRootsSet.pop_back();
        BaseObject* exportObj = to_object(ZOffset::address(to_zoffset(entry.object_address())));
        if (exportObj == nullptr) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(externMtx);
            // Multiple export handles may name the same owner.
            if (!discoveredExternObjects.emplace(exportObj, ValueRootList{}).second) {
                continue;
            }
        }
        WorkStack exportSeed;
        exportSeed.push_back(entry);
        markedObjectCount.fetch_add(RunMajorStripeMark(exportSeed), std::memory_order_relaxed);

        // ZMark::mark_and_follow (zMark.cpp:412-415) deduplicates GC liveness,
        // not ownership. Cangjie's foreign-cycle handoff has no JNI equivalent:
        // every export owner needs its own strong reachable foreign set, even
        // when the young-roots prelude or another owner already marked it.
        std::unordered_set<BaseObject*> visited;
        std::vector<BaseObject*> pending{exportObj};
        while (!pending.empty()) {
            BaseObject* object = pending.back();
            pending.pop_back();
            if (!visited.insert(object).second) {
                continue;
            }
            if (object->GetTypeInfo()->IsForeignType()) {
                std::lock_guard<std::mutex> lock(externMtx);
                discoveredExternObjects[exportObj].push_back(object);
            }
            // Discovery is not keep-alive (zReferenceProcessor.cpp:175-203):
            // do not turn a weak referent into an export ownership edge.
            HeapIterator::Fields(object, false, [&](BaseObject* holder, RefField<>& field) {
                BaseObject* target = GetAndTryTagObj(RefSlotKind::STRONG, holder, field);
                if (target != nullptr) {
                    pending.push_back(target);
                }
            });
        }
    }
}

void TracingCollector::FindUselessExternObjects()
{
    std::lock_guard<std::mutex> lock(externMtx);
    CurrentizeValueRootMap(discoveredExternObjects, Generation::Old);
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
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
bool TracingCollector::MarkEntryObject(BaseObject* obj, const MarkStackEntry& entry,
                                       MarkLiveCache* cache) const
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
                          ZMark* domain)
{
    const size_t assumed = context.NStripes();
    const size_t nstripes = stripes.NStripes();
    if (assumed != nstripes) {
        context.SetNStripes(nstripes);
    } else if (nstripes < stripes.CalculateNStripes(terminate.WorkerCount()) && stripes.IsCrowded()) {
        const size_t restored = nstripes << 1;
        if (stripes.TrySetNStripes(nstripes, restored)) {
            context.SetNStripes(restored);
        }
    }
    const size_t stripe = stripes.StripeForWorker(terminate.WorkerCount(), workerId);
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
                  size_t workerId, const ZMark::Process& process, ZMark* domain)
{
    MarkStackEntry entry;
    size_t processed = 0;
    context.SetStripeId(stripes.StripeForWorker(terminate.WorkerCount(), workerId));
    context.SetNStripes(stripes.NStripes());
    while (context.Stacks().Pop(smr, workerId, stripes, context.StripeId(), entry)) {
        process(entry);
        if ((processed++ & 31) == 0 && RebalanceWork(context, stripes, terminate, workerId, domain)) {
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
    for (;;) {
        if (!Drain(context, smr, stripes, terminate, workerId, process, domain)) {
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
    PrepareWork(gcWorkers->active_workers());
}

void ZMark::PrepareWork(size_t workers)
{
    CHECK_DETAIL(workers != 0, "mark domain needs a worker");
    nworkers = workers;
    targetNStripes = CalculateNStripes(workers);
    stripes.SetNStripes(targetNStripes);
    EnsureWorkers(workers);
    terminate.Reset(workers);
    workNProactiveFlush.store(0, std::memory_order_relaxed);
    workNTerminateFlush.store(0, std::memory_order_relaxed);
    terminate.SetResurrected(false);
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
    if (abortToken != nullptr && abortToken->Poll()) {
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

bool ZMark::Flush()
{
    return MutatorManager::Instance().HandshakeFlushMarkProducers(this);
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
    return Flush() || !stripes.IsEmpty() || terminate.Resurrected();
}

bool ZMark::TryEnd()
{
    if (terminate.Resurrected()) {
        return false;
    }
    (void)Flush();
    (void)FlushStacks();
    return stripes.IsEmpty();
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

#include "Heap/Collector/MarkPartialArray.h"

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
TracingCollector::TracingCollector(Allocator& allocator, CollectorResources& resources)
        : Collector(), theAllocator(allocator), collectorResources(resources)
    {}
}

namespace MapleRuntime {
void TracingCollector::FollowPartialArray(const MarkStackEntry& entry, WorkStack& workStack)
    {
        Collector::AbortUnimplemented("TracingCollector::FollowPartialArray");
    }
}

#include "Heap/z/zMark.inline.hpp"

namespace MapleRuntime {
bool TracingCollector::MarkObject(BaseObject* obj) const
    {
        ZPage* regionInfo = Heap::page(reinterpret_cast<MAddress>(obj));
        // ZPage::mark_object + inc_live on the first live claim (zMark.cpp:405-425).
        bool incLive = false;
        bool marked = !regionInfo->mark_object(from_object(obj), false, incLive);
        if (incLive) {
            regionInfo->inc_live(1, obj->GetSize());
        }
        if (!marked) {
            size_t objSize = obj->GetSize();
            if (!fixReferences && regionInfo->IsFromRegion()) {
                DLOG(TRACE, "marking tag w-obj %p<cls %p>+%zu", obj, obj->GetTypeInfo(), objSize);
            }
        }
        return marked;
    }
}
