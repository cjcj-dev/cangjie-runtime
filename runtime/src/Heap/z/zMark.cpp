// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zHeap.hpp"
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
#include "Heap/Collector/FinalizerProcessor.h"
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
        stackScanEpoch = StackWatermark::epoch_id();
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
        if (ZAbort::should_abort()) {
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
    RootVisitor& visitedRawRootVisitor = rawRootVisitor;
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
void CopyCollector::DiscoverFinalizableRoot(NativeSlot& slot) const
{
    CHECK(oldCycle.IsPhaseMark());
    BaseObject* object = ZBarrier::ReadStaticRef(slot);
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &slot };
    object = ValidateCurrentValue(object, provenance);
    if (object == nullptr) return;
    auto* page = Heap::page(reinterpret_cast<MAddress>(object));
    if (page->IsYoungRegion() || page->is_object_strongly_live(from_object(object))) return;
    auto& processor = collectorResources.GetFinalizerProcessor().GetReferenceProcessor();
    (void)processor.DiscoverReference(object, ReferenceType::FINAL);
    ZBarrier::MarkFinalizableBarrierOnRoot(slot);
}

void CopyCollector::DiscoverWeakReference(BaseObject* reference, WorkStack& workStack)
{
    HeapSlot<>& referentField =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(reference) + TYPEINFO_PTR_SIZE);
    BaseObject* referent = GetAndTryTagObj(RefSlotKind::WEAK_REFERENT, reference, referentField);
    if (referent == nullptr) {
        return;
    }
    (void)DiscoverReference(reference, ReferenceType::WEAK);
    (void)workStack;
}

namespace {
// ZMarkOopClosure (zMark.cpp:666-670). P08 owns the missing dedicated old
// mark barrier; this adapter consumes the existing old publication producer.
class MarkOopClosure {
public:
    explicit MarkOopClosure(const CopyCollector& collector) : collector(collector) {}
    void DoOop(NativeSlot& slot) const
    {
        BaseObject* object = ZBarrier::ReadStaticRef(slot);
        ZBarrier::MarkBarrierOnOldOopField(nullptr, slot, false);
    }
private:
    const CopyCollector& collector;
};

class MarkThreadClosure {
public:
    static StackWatermarkProcessOopClosure::RootFunction root_function() { return ZUncoloredRoot::mark; }
    void DoThread(Mutator& mutator) const
    {
        RootVisitor markRoot = [](ObjectRef& root) {
            ZUncoloredRoot::mark(reinterpret_cast<zaddress_unsafe*>(&root), ZPointerLoadGoodMask);
        };
        size_t frames = 0;
        (void)StackWatermarkSet::finish_processing(mutator, markRoot, markRoot, StackWatermark::epoch_id(),
                                                   nullptr, frames, reinterpret_cast<void*>(root_function()));
#if defined(MRT_TESTABLE_INTERNALS)
        if (CopyCollector::testOldMarkThreadResult) {
            CopyCollector::testOldMarkThreadResult(mutator);
        }
#endif
    }
};

// ZMarkOldRootsTask, zMark.cpp:797-834. Root results are published to the
// generation mark domain by closures, then flushed by each participating worker.
class MarkOldRootsTask final : public ZTask {
public:
    MarkOldRootsTask(const CopyCollector& collector, ZMark& domain,
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
            if (CopyCollector::testColoredRootResult) {
                CopyCollector::testColoredRootResult(GCCycleGeneration::OLD, &slot);
            }
#endif
        });
        rootsUncolored.Apply(uncolored);
        rootsUncolored.ApplyThreads([&](Mutator& mutator) { threadClosure.DoThread(mutator); });
        // zMark.cpp:830-834: flush and free worker stacks for both generations
        // here, since the set of workers executing during root scanning can be
        // different from the set of workers executing during mark.
        ThreadLocal::FlushCurrentThreadMarkStacks();
#if defined(MRT_TESTABLE_INTERNALS)
        if (CopyCollector::testColoredRootResult) {
            CopyCollector::testColoredRootResult(GCCycleGeneration::OLD, nullptr);
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

void CopyCollector::EnumAllCommonRoots(ZWorkers& workers)
{
    CHECK_DETAIL(oldCycle.MarkPtr() != nullptr, "old mark domain must start before roots");
    MarkOldRootsTask task(*this, oldCycle.Mark(),
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
    MarkYoungRootsTask(const CopyCollector& collector, std::function<void()> uncolored, unsigned workers)
        : ZTask("ZMarkYoungRootsTask"), rootsColored(collector, workers), uncolored(std::move(uncolored)) {}

    void work() override
    {
        rootsColored.Apply([this](NativeSlot& slot) {
            coloredClosure.DoOop(slot);
#if defined(MRT_TESTABLE_INTERNALS)
            if (CopyCollector::testColoredRootResult) {
                CopyCollector::testColoredRootResult(GCCycleGeneration::YOUNG, &slot);
            }
#endif
        });
        rootsUncolored.Apply(uncolored);
        // zMark.cpp:887-891: flush and free worker stacks for both generations.
        ThreadLocal::FlushCurrentThreadMarkStacks();
#if defined(MRT_TESTABLE_INTERNALS)
        if (CopyCollector::testColoredRootResult) {
            CopyCollector::testColoredRootResult(GCCycleGeneration::YOUNG, nullptr);
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
    SuspendibleThreadSetJoiner joiner;
    GetWorkers(GCCycleGeneration::YOUNG).run(&task);

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
    (void)workStack;
    if (finalizable) {
        const_cast<GenerationCycle&>(GetGenerationCycle(GCCycleGeneration::YOUNG))
            .MarkObjectIfActive<false, true, true, true>(from_object(object));
    } else {
        const_cast<GenerationCycle&>(GetGenerationCycle(GCCycleGeneration::YOUNG))
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

namespace WCollectorInternal {
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

void WCollector::StartYoungMarkWork()
{
    ZWorkers& workers = GetWorkers(GCCycleGeneration::YOUNG);
    youngCycle.Mark().BindWorkers(&workers);
    youngCycle.Mark().Start();
    MarkingStacks::VerifyEmpty(youngCycle.Mark().Stripes().Population());
}

void WCollector::MarkYoungObjectIfActive(BaseObject* object) const
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    const_cast<GenerationCycle&>(GetGenerationCycle(GCCycleGeneration::YOUNG))
        .MarkObjectIfActive<false, false, true, false>(from_object(object));
}

void WCollector::TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,
                                          std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                          MinorSlotSet& weakSlots,
                                          const MinorSlotSet* reachableSlotDomain)
{
    (void)fullYoungScan;
    (void)reachableVec;
    (void)reachableSlots;
    (void)weakSlots;
    (void)reachableSlotDomain;
    (void)workStack;
    g_markStripeArmed.fetch_add(1, std::memory_order_relaxed);
    const size_t dispelAtEntry = ZPage::GetTdWindowCount();
    ZWorkers& workersSet = GetWorkers(GCCycleGeneration::YOUNG);
    g_markStripeTurned.fetch_add(1, std::memory_order_relaxed);
    ZMark& domain = youngCycle.Mark();
    (void)PublishHandshakeMarkWork(workStack, &domain);
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
    (void)youngCycle.Mark().Flush(ThreadLocal::GetThreadLocalData());
    if (workStack.empty() && youngCycle.Mark().Stripes().IsEmpty() &&
        youngCycle.Mark().Stacks().IsEmpty()) {
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
    (void)youngCycle.Mark().Flush();
    (void)youngCycle.Mark().Flush(ThreadLocal::GetThreadLocalData());
    (void)PublishHandshakeMarkWork(workStack, &youngCycle.Mark());
    do {
        if (!workStack.empty() || !youngCycle.Mark().Stripes().IsEmpty() ||
            !youngCycle.Mark().Stacks().IsEmpty()) {
            if (windowStats != nullptr) {
                ++windowStats->closureCalls;
            }
            TraceYoungClosure(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots);
        }
        if (ZAbort::should_abort()) {
            return false;
        }
    } while (youngCycle.Mark().TryTerminateFlush());
    return true;
}

bool WCollector::TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats)
{
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "young mark-end flush requires stopped mutators");
    NoteMarkTerminatePause();
    const size_t before = youngCycle.Mark().Stripes().Population();
    (void)PublishHandshakeMarkWork(workStack, &youngCycle.Mark());
    const bool ended = youngCycle.Mark().TryEnd();
    const size_t after = youngCycle.Mark().Stripes().Population();
    NoteMarkTerminateFlushed(after >= before ? after - before : 0);
    if (!ended) {
        return false;
    }
    MarkingStacks::VerifyAllEmpty(youngCycle.Mark());
    return true;
}
void WCollector::MarkNewObject(BaseObject* obj)
{
    // Registration follows object initialization (BaseObject::RegisterFinalizer).
    // ZMark::AnyThread / DontFollow: publish mark-only work for this current object.
    GenerationCycle& cycle = GetGenerationCycle(ObjectGeneration(obj));
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
    ZMark* domain = young ? youngCycle.MarkPtr() : oldCycle.MarkPtr();
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
    const bool young = FlushGCDataMarkProducers(data, youngCycle.MarkPtr());
    return FlushGCDataMarkProducers(data, oldCycle.MarkPtr()) || young;
}

bool WCollector::FlushThreadMarkProducers(ThreadLocalData* tls)
{
    bool published = FlushThreadMarkProducers(tls, youngCycle.MarkPtr());
    return FlushThreadMarkProducers(tls, oldCycle.MarkPtr()) || published;
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
void CopyCollector::StartOldMarkWork()
{
    // ZGenerationOld::mark_start -> ZMark::start. Initialize the existing M3
    // domain before publishing old's mark phase to mutators and young workers.
    ZWorkers& workers = GetWorkers(GCCycleGeneration::OLD);
    oldCycle.Mark().BindWorkers(&workers);
    oldCycle.Mark().Start();
}

void CopyCollector::MarkOldObjectIfActive(BaseObject* object, bool gcThread) const
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    auto& cycle = const_cast<GenerationCycle&>(GetGenerationCycle(GCCycleGeneration::OLD));
    if (gcThread) {
        cycle.MarkObjectIfActive<false, true, true, false>(from_object(object));
    } else {
        cycle.MarkObjectIfActive<false, false, true, false>(from_object(object));
    }
}

size_t CopyCollector::RunMajorStripeMark(WorkStack& workStack, bool partial)
{
    (void)workStack;
    ZWorkers& workersSet = GetWorkers(GCCycleGeneration::OLD);
    ZMark& domain = oldCycle.Mark();
    domain.BindWorkers(&workersSet);
    (void)domain.Stacks().Flush(domain.Stripes(), true);
    ZMarkTask task(&domain, partial);
    workersSet.run(&task);
    if (!partial && !ZAbort::should_abort()) {
        CHECK_DETAIL(domain.Stripes().IsEmpty(),
                     "major striped closure returned without coordinated worker termination");
    }
    return 0;
}

void CopyCollector::TracingImpl(WorkStack& workStack)
{
    // ZMark::mark_follow (zMark.cpp:944-952): join workers, check abort,
    // then flush producers. Stopped stripes never start another follow pass.
    (void)workStack;
    oldCycle.Mark().MarkFollow(false);
}

void CopyCollector::ProcessExportRoots(WorkStack& foreignRootsSet)
{
    while (!foreignRootsSet.empty()) {
        if (ZAbort::should_abort()) {
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
        MarkOldObjectIfActive(exportObj, true);
        WorkStack exportSeed;
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
            object->ForEachRefField([&](RefField<>& field) {
                BaseObject* target = GetAndTryTagObj(RefSlotKind::STRONG, object, field);
                if (target != nullptr) {
                    pending.push_back(target);
                }
            });
        }
    }
}

void CopyCollector::FindUselessExternObjects()
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
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
bool CopyCollector::MarkEntryObject(BaseObject* obj, const MarkStackEntry& entry,
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
    auto& collector = static_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    if (generation == MarkingStacks::MarkingGeneration::YOUNG) {
        auto& w = static_cast<WCollector&>(collector);
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
        const bool wasMarked = w.MarkEntryObject(object, entry, &ctx.Cache());
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
        MarkPartialArray::FollowPartialReferences(entry, [](MAddress slot) {
            auto& field = HeapSlotAt<>(slot);
            ZBarrier::MarkBarrierOnOldOopField(nullptr, field, false);
        }, publish);
        return;
    }
    BaseObject* obj = to_object(ZOffset::address(to_zoffset(entry.object_address())));
    const bool wasMarked = collector.MarkEntryObject(obj, entry, &ctx.Cache());
    if ((!entry.mark() || !wasMarked) && entry.follow()) {
        if (!obj->HasRefField()) {
            return;
        }
        if (UNLIKELY(obj->IsWeakRef())) {
            WorkStack discovered;
            collector.DiscoverWeakReference(obj, discovered);
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
    auto& collector = static_cast<WCollector&>(Heap::GetHeap().GetCollector());
    return domain == nullptr ? collector.FlushGCDataMarkProducers(data)
                             : collector.FlushGCDataMarkProducers(data, domain);
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
    auto& collector = static_cast<WCollector&>(Heap::GetHeap().GetCollector());
    return (domain == nullptr ? collector.FlushThreadMarkProducers(tls)
                              : collector.FlushThreadMarkProducers(tls, domain)) || published;
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
    return Flush() || !stripes.IsEmpty() || terminate.Resurrected();
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
    const bool flushed = HandshakeFlush(this) || FlushStacks();
    if (flushed || !stripes.IsEmpty()) {
        return false;
    }
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
CopyCollector::CopyCollector(Allocator& allocator, CollectorResources& resources)
        : Collector(), theAllocator(allocator), collectorResources(resources)
    {
        collectorType = CollectorType::COPY_COLLECTOR;
    }
}

namespace MapleRuntime {
void CopyCollector::FollowPartialArray(const MarkStackEntry& entry, WorkStack& workStack)
    {
        Collector::AbortUnimplemented("CopyCollector::FollowPartialArray");
    }
}

#include "Heap/z/zMark.inline.hpp"

namespace MapleRuntime {
bool CopyCollector::MarkObject(BaseObject* obj) const
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
