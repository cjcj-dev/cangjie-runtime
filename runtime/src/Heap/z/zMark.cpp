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
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
// Preserve the two existing product template instantiations while internal
// consumers move to the richer first-live result. Their inline definitions
// and visibility are unchanged; this does not export a test-only API.
template bool RegionInfo::MarkObject<Generation::Young>(
    MarkView<Generation::Young>, const BaseObject*, size_t, bool);
template bool RegionInfo::MarkObject<Generation::Old>(
    MarkView<Generation::Old>, const BaseObject*, size_t, bool);



bool WCollector::MarkObject(BaseObject* obj) const
{
    return MarkObjectImpl(obj, false);
}

bool WCollector::MarkObjectImpl(BaseObject* obj, bool youngClaim, MarkLiveCache* liveCache) const
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));

    size_t objectSize = obj->GetSize();
    // livesame: MarkObject adds live only on 0→1 (ZGC inc_live); no second AddLiveByteCount.
    // ZGC zPage.inline.hpp:284-294: the target page owns mark authority. gcReason
    // names the running closure, not the target object's face.
    // When liveCache is set, mark bits stay atomic and live bytes are coalesced
    // per worker (ZGC zMarkCache.hpp).
    bool firstLive = false;
    bool marked = region->MarkObjectByOwnerWithLiveClaim(obj, objectSize, liveCache == nullptr, firstLive);
    if (firstLive && liveCache != nullptr) {
        liveCache->IncLive(region, objectSize);
    }
    if (!marked) {
        DLOG(TRACE, "mark obj %p<%p>(%zu) in region %p(%u)@%#zx, live %zu", obj, obj->GetTypeInfo(), objectSize,
             region, region->GetRegionType(), region->GetRegionStart(), region->GetLiveByteCount());
    }
    return marked;
}

bool WCollector::MarkEntryObject(BaseObject* obj, const MarkStackEntry& entry, MarkLiveCache* cache) const
{
    if (entry.mark() && !entry.finalizable()) {
        return MarkObjectImpl(obj, false, cache);
    }
    return TracingCollector::MarkEntryObject(obj, entry, cache);
}

bool WCollector::ResurrectObject(BaseObject* obj, size_t offset, RegionInfo* region)
{
    // livesame: ResurrectObject counts on 0→1 inside.
    bool resurrected = region->ResurrectObject(obj, offset);
    if (!resurrected) {
        DLOG(TRACE, "resurrect region %p@%#zx obj %p<%p>(%zu), live bytes %zu", region, region->GetRegionStart(),
             obj, obj->GetTypeInfo(), obj->GetSize(), region->GetLiveByteCount());
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
                      (REMAP_COLOUR_MASK | MARKED_YOUNG_MASK | MARKED_OLD_MASK)) != 0,
                 "NativeSlot requires colored value at EnumRefFieldRoot slot=%p word=%#zx",
                 &field, raw(oldField.GetFieldValue()));
    // A mark-good root has passed this mark epoch and is necessarily load-good
    // (OpenJDK zAddress.inline.hpp:658-664).
    if (is_mark_good(oldField)) {
        // Anchor main 8cd248497dd8c251ca824d9f089d5e30125c80c9
        BaseObject* target = to_object(oldField.GetTargetObject());
        // Reject non-heap: do not call make_load_good (remap would touch non-heap).
        if (!Heap::IsHeapAddress(target)) {
            return;
        }
        CHECK_DETAIL(target->IsValidObject(), "Enum static root %p(%p) encounters invalid object", target, &field);
        rootSet.push_back(target);
        return;
    }

    // tracecov: is the mark's field walk broad enough to be the thing that keeps colours fresh?
    // The mark-good fast path above returns without healing, which is correct because mark-good
    // implies load-good; a stale field is therefore mark-bad and reaches the code below, which does
    // heal (HealSlot at the end of this function).  So "stale slots survive" reduces to "the mark
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
    } else if (HealSlot(field, oldField.GetFieldValue(), newField.GetFieldValue(),
                        HealSite::WCollectorEnumRefFieldRoot)) {
        DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &field, raw(oldField.GetFieldValue()),
             raw(newField.GetFieldValue()), latest, latest->GetTypeInfo(), latest->GetSize());
    } else {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, raw(oldField.GetFieldValue()), latest,
             latest->GetTypeInfo(), latest->GetSize());
    }
    rootSet.push_back(latest);
}

void WCollector::EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet, Generation generation) const
{
    zaddress_unsafe observed = ref.LoadPlain();
    if (is_null(observed)) {
        return;
    }

    // RootSlot contains an uncoloured address. Constructing a local HeapSlot is
    // only a bit-layout decoder for legacy coloured roots at external ABI edges;
    // the root storage itself is never exposed as a HeapSlot.
    HeapSlot<> observedBits(to_zpointer(raw(observed)));
    BaseObject* root = to_object(observedBits.GetTargetObject());
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
    HealRootWriteback(ref, root, HealSite::WCollectorEnumRawRoot);
    rootSet.push_back(root);
}

// note each ref-field will not be traced twice, so each old pointer the tracer meets must come from previous gc.
void WCollector::TraceRefField(BaseObject* obj, RefField<>& field, WorkStack& workStack, bool finalizable) const
{
    RefField<> oldField(field);
    // markstale: the mark-good fast path returns without healing, which is only safe if mark-good
    // implies "target is current".  It does not here.  mark-good is a superset of load-good, the
    // remap space is four values and a flip is an xor, so a colour published at N is published
    // again at N+2 -- a stale pointer whose colour has come back around passes this test and the
    // one walk that would have repaired the slot skips it.  Both crash families reduce to that:
    // the read barrier hands the value out (fixed by kStaleGuard in Barrier::ReadReference), and
    // the mark never repairs the slot, so after the route retires FindToVersion can no longer
    // answer and the slot is unrepairable for good.
    //
    // ZGC's mark has the same fast path (ZBarrier::barrier returns early on fast_path(o)), and it
    // is safe there because staleness is bounded: ZGenerationOld runs Phase 8
    // concurrent_remap_young_roots before old relocate start specifically so that no pointer
    // accumulates two remap-bit errors (zGeneration.cpp:1503-1508).  We have no such phase, so the
    // bound has to be enforced where the assumption is used.
    //
    // Cost: one relaxed header load on the mark's fast path, only for heap targets.
    static constexpr bool kMarkStaleGuard = true;
    bool staleTarget = false;
    if (kMarkStaleGuard) {
        BaseObject* t = to_object(oldField.GetTargetObject());
        if (t != nullptr && Heap::IsHeapAddress(t)) {
            const uint64_t hdr = __atomic_load_n(reinterpret_cast<const uint64_t*>(t), __ATOMIC_RELAXED);
            const unsigned sc = static_cast<unsigned>((hdr >> 48) & 0x3u);
            if (sc == 3u || (hdr & 0xffffffffffffull) == 0) {
                staleTarget = true;
                static std::atomic<uint64_t> markStaleHits{ 0 };
                const uint64_t n = markStaleHits.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((n & (n - 1)) == 0) {
                    LOG(RTLOG_ERROR, "[MARKSTALE] n=%lu target=%p sc=%u", n, static_cast<void*>(t), sc);
                }
            }
        }
    }
    if (is_mark_good(oldField) && !staleTarget) {
        BaseObject* targetObj = to_object(oldField.GetTargetObject());
        // zbisect: plain non-heap (0x55–0x65) was admitted here → IsMarkedObject → GetUnitIdxAt OOB.
        // Skip field on reject — same as pre-zcolor7 slow path for plain non-heap.
        if (!Heap::IsHeapAddress(targetObj)) {
            // gatedrop: reject arm only (default off). leave untraced.

            return;
        }
        // Anchor main 9a124c4f14ddd5944330ddbf68d1659cbb629e56
        // obj is null when the field arrived as a partial-array chunk, which
        // carries no holder (ZGC's entry does not either). Only this message
        // loses detail; the check itself is unchanged.
        CHECK_DETAIL(targetObj->IsValidObject(),
                     "Invalid object %p is referenced by strong object %p: %s and offset %zd", targetObj, obj,
                     obj == nullptr ? "<partial-array chunk>" : obj->GetTypeInfo()->GetName(),
                     obj == nullptr ? static_cast<ssize_t>(-1) : BaseObject::FieldOffset(obj, &field));
        if (!IsMarkedObject<Generation::Old>(targetObj)) {

            workStack.push_back(MarkStackEntry::MarkAndFollow(targetObj, finalizable));
        }
        return;
    }

    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, obj, &field };
    BaseObject* latest = make_load_good(oldField, provenance);

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    // ZBarrier::mark_barrier_on_oop_field remaps via the forwarding table before
    // it colours (zBarrier.inline.hpp:591-623). make_load_good correctly returns
    // a load-good address without a table walk; a colour wrap can still name a
    // previous-cycle from. Consult current then retired tables only — TRACE is
    // not relocate phase, so do not TryMutatorRelocate / forward_object here.
    {
        const MAddress fromAddr = reinterpret_cast<MAddress>(latest);
        MAddress stored = is_load_good(oldField) ? 0 : ForwardingTable::FindTo(
            raw(oldField.GetTargetObject()), static_cast<Generation>(remap_generation(oldField)));
        if (stored != 0) {
            BaseObject* to = reinterpret_cast<BaseObject*>(stored);
            if ((to != nullptr)) {
                latest = to;
            }
        } else if (latest->IsForwarded()) {
            RegionInfo* ghost = RegionInfo::GetGhostFromRegionAt(fromAddr);
            BaseObject* published = GetForwardPointer(latest, ghost);
            if (published != nullptr) {
                latest = published;
            }
        }
        if (!Heap::IsHeapAddress(latest)) {
            return;
        }
        // Both tables miss: do not treat the from address as remapped.
        // ColourResolvedRefField → CheckStoreGoodTarget fail-closes.
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by strong object %p: %s and offset %zd",
                 latest, obj, obj == nullptr ? "<partial-array chunk>" : obj->GetTypeInfo()->GetName(),
                 obj == nullptr ? static_cast<ssize_t>(-1) : BaseObject::FieldOffset(obj, &field));
    RefField<> newField = ColourResolvedRefField(latest, provenance);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if ([&]() {
                   const bool ok = HealSlot(field, oldField.GetFieldValue(), newField.GetFieldValue(),
                                            HealSite::WCollectorTraceRefField);
                   if (ok) {
                       Collector::HealFpMark(reinterpret_cast<uintptr_t>(&field));
                       static std::atomic<uint64_t> healed{ 0 };
                       const uint64_t h = healed.fetch_add(1, std::memory_order_relaxed) + 1;
                       if ((h & (h - 1)) == 0) {
                           LOG(RTLOG_ERROR, "[TRACECOV] healed=%lu", h);
                       }
                   }
                   return ok;
               }()) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, raw(oldField.GetFieldValue()),
             raw(newField.GetFieldValue()), latest, latest->GetTypeInfo(), latest->GetSize());
    }

    if (!IsMarkedObject<Generation::Old>(latest)) {

        workStack.push_back(MarkStackEntry::MarkAndFollow(latest, finalizable));
    }
}

// Ported from ZGC ZMark::push_partial_array (zMark.cpp:185-196). ZGC pushes a
// tagged entry onto its mark stack; we push the same descriptor encoded into a
// BaseObject* slot of our work stack so another stripe worker can steal it.
// If the descriptor does not fit one word we trace the chunk here
// rather than drop it -- correctness never depends on the encoding succeeding.
void WCollector::PushPartialArray(RefField<>* addr, size_t length, WorkStack& workStack, bool finalizable) const
{
    if (UNLIKELY(!MarkPartialArray::Encodable(addr, length))) {
        FollowArrayElementsSmall(nullptr, addr, length, workStack, finalizable);
        return;
    }
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

// zMark.cpp:257-263 (follow_array_elements). The encodability probe has no ZGC
// counterpart: ZGC bounds its heap so the entry always fits, whereas ours is
// only checked here. Failing it means "trace inline", i.e. today's behaviour.
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
    if (is_mark_good(oldField)) {
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
    } else if (HealSlot(field, oldField.GetFieldValue(), newField.GetFieldValue(),
                        HealSite::WCollectorGetAndTryTagObj)) {
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
    MarkingStacks::VerifyEmpty(GetWorkers(GCCycleGeneration::OLD).GetSnapshot().remainingWorkers);
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
        MarkingStacks::VerifyEmpty(GetWorkers(GCCycleGeneration::OLD).GetSnapshot().remainingWorkers);

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

namespace {
// ZMarkOopClosure (zMark.cpp:666-670). P08 owns the missing dedicated old
// mark barrier; this adapter consumes the existing old publication producer.
class MarkOopClosure {
public:
    explicit MarkOopClosure(const TracingCollector& collector) : collector(collector) {}
    void DoOop(NativeSlot& slot) const
    {
        BaseObject* object = Heap::GetBarrier().ReadStaticRef(slot);
        collector.MarkOldObjectIfActive(object);
    }
private:
    const TracingCollector& collector;
};

// ZMarkOldRootsTask, zMark.cpp:797-834. Root results are published to the
// generation mark domain by closures, then flushed by each participating worker.
class MarkOldRootsTask final : public GCWorkerTask {
public:
    MarkOldRootsTask(const TracingCollector& collector, MarkDomain& domain,
                     std::function<void()> uncolored, unsigned workers)
        : rootsColored(collector, workers), coloredClosure(collector), domain(domain), uncolored(std::move(uncolored)) {}
    void Work(uint32_t) override
    {
        rootsColored.Apply([&](NativeSlot& slot) {
            coloredClosure.DoOop(slot);
#if defined(MRT_TESTABLE_INTERNALS)
            if (TracingCollector::testColoredRootResult) {
                TracingCollector::testColoredRootResult(GCWorkers::Generation::OLD, &slot);
            }
#endif
        });
        rootsUncolored.Apply(uncolored);
        (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), domain);
#if defined(MRT_TESTABLE_INTERNALS)
        if (TracingCollector::testColoredRootResult) {
            TracingCollector::testColoredRootResult(GCWorkers::Generation::OLD, nullptr);
        }
#endif
    }
private:
    RootsIteratorStrongColored rootsColored;
    RootsIteratorStrongUncolored rootsUncolored;
    MarkOopClosure coloredClosure;
    MarkDomain& domain;
    std::function<void()> uncolored;
};
} // namespace

void TracingCollector::EnumAllCommonRoots(GCWorkers& workers)
{
    CHECK_DETAIL(majorMarkDomain != nullptr, "old mark domain must start before roots");
    MarkOldRootsTask task(*this, *majorMarkDomain, [&] {
        VisitStrongPlainRoots([&](ObjectRef& root) {
            MarkOldObjectIfActive(Heap::GetBarrier().ReadPlainRoot(root));
        }, {});
        VisitSurrectedExportRoots([&](BaseObject* object) { MarkOldObjectIfActive(object); });
    }, workers.ActiveWorkers());
    workers.Run(task);
#if defined(MRT_TESTABLE_INTERNALS)
    ObservePublishedRoots(workers.GetSnapshot().generation);
#endif
}

namespace {
// ZMarkYoungOopClosure, zMark.cpp:678-681.
class MarkYoungOopClosure {
public:
    void DoOop(NativeSlot& slot) const
    {
        Heap::GetBarrier().MarkYoungGoodBarrierOnOopField(slot);
    }
};

// ZMarkYoungRootsTask, zMark.cpp:852-891. Colored roots share one closure;
// Cangjie's stack/value-root scanner replaces HotSpot thread/nmethod closures.
class MarkYoungRootsTask final : public GCWorkerTask {
public:
    MarkYoungRootsTask(const TracingCollector& collector, MarkDomain& domain,
                       std::function<void()> uncolored, unsigned workers)
        : rootsColored(collector, workers), domain(domain), uncolored(std::move(uncolored)) {}

    void Work(uint32_t) override
    {
        rootsColored.Apply([this](NativeSlot& slot) {
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
            Heap::GetHeap().GetRememberedSet().VisitStaticForCrossCheck(reinterpret_cast<MAddress>(&slot));
#endif
            coloredClosure.DoOop(slot);
#if defined(MRT_TESTABLE_INTERNALS)
            if (TracingCollector::testColoredRootResult) {
                TracingCollector::testColoredRootResult(GCWorkers::Generation::YOUNG, &slot);
            }
#endif
        });
        rootsUncolored.Apply(uncolored);
        (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), domain);
#if defined(MRT_TESTABLE_INTERNALS)
        if (TracingCollector::testColoredRootResult) {
            TracingCollector::testColoredRootResult(GCWorkers::Generation::YOUNG, nullptr);
        }
#endif
    }
private:
    RootsIteratorAllColored rootsColored;
    MarkYoungOopClosure coloredClosure;
    MarkDomain& domain;
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
    MarkYoungRootsTask task(*this, *youngMarkDomain, [&] {
        VisitMinorRootSlots(rawRootVisitor, invisibleRootVisitor, stackScanEpoch);
        VisitMinorValueRoots(visitor);
    }, GetWorkers(GCCycleGeneration::YOUNG).ActiveWorkers());
    GetWorkers(GCCycleGeneration::YOUNG).Run(task);
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
            RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
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
                                   : static_cast<unsigned>(region->GetMarkBitmap(
                                         region->GetMarkView<Generation::Young>()) == nullptr &&
                                                          region->GetRegionAllocPtr() > region->GetRegionStart()));
        }
        CHECK_DETAIL(false, "minor root/reference %p is not a valid object origin=%s", object, src);
    }
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
    if (!region->IsYoungRegion()) {
        if (GetGenerationCycle(GCCycleGeneration::YOUNG).IsMajorRoots()) {
            MarkOldObjectIfActive(object, true);
        }
        return;
    }
    if (region->IsYoungRegion() &&
        !region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object)) {


        bool firstLive = false;
        const bool already = finalizable
            ? region->ResurrectObjectWithLiveClaim(object,
                region->GetAddressOffset(reinterpret_cast<MAddress>(object)), false, firstLive)
            : region->MarkObjectByOwnerWithLiveClaim(object, object->GetSize(), false, firstLive);
        if (!already) {
            workStack.push_back(MarkStackEntry::Claimed(object, firstLive, true, finalizable));
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

        workStack.push_back(admitted);
    }
}

void PushAdmittedYoung(const MarkStackEntry& entry, TracingCollector::WorkStack& workStack, const char* origin,
                       const void* slot, BaseObject* holder)
{
    BaseObject* admitted = AdmitYoungObject(entry.object(), origin, slot, holder);
    if (admitted != nullptr) {

        workStack.push_back(MarkStackEntry(admitted, entry.mark(), entry.incLive(), entry.follow(),
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
// h3seed3 乙: live-holder slot → free|garbage target → HealSlot null.
// Criterion fields (RegionInfo state word): IsFreeRegion() / IsGarbageRegion()
// via TryGetRegionInfoAt(target) at the call site (closure edge or Fix).
// Returns true if the slot was scrubbed (caller must not push / treat as live edge).
bool ScrubMinorFreeTarget(RefField<>& field, BaseObject* target, bool /*fromFix*/)
{
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
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
    (void)HealSlot(field, oldField.GetFieldValue(), zpointer::null,
                   HealSite::WCollectorMinorFixForwardNull, HealNull::Disallow);
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
    MarkDomain* domain = nullptr;
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

class YoungStripedMarkingWork : public GCRestartableWorkerTask {
public:
    explicit YoungStripedMarkingWork(YoungStripedShared& shared) : shared(shared) {}

    void ResizeWorkers(uint32_t workers) override
    {
        shared.workerCount = workers;
        shared.domain->ResizeWorkers(workers);
        while (shared.outputs.size() < workers) {
            shared.outputs.emplace_back(std::make_unique<YoungStripedWorkerOutput>());
        }
    }

    void Work(uint32_t workerId) override
    {
        MarkContext local(shared.workerCount, workerId, shared.Stripes(), shared.Stacks());
        size_t nMarked = 0;
        (void)MarkEngine::FollowWork(local, shared.Smr(), shared.Stripes(), shared.Terminate(), workerId,
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
    }

private:

    void PushObject(MarkContext& ctx, BaseObject* object, bool finalizable = false)
    {
        PublishEntry(ctx, MarkStackEntry::MarkAndFollow(object, finalizable));
    }

    void PublishEntry(MarkContext& ctx, const MarkStackEntry& entry)
    {
        MAddress address = 0;
        if (entry.partialArray()) {
            size_t length = 0;
            MarkPartialArray::Decode(entry, address, length);
        } else {
            address = reinterpret_cast<MAddress>(entry.object());
        }
        const size_t stripeIndex = shared.StripeFor(reinterpret_cast<BaseObject*>(address));
        const bool publish = stripeIndex != ctx.StripeId();
        ctx.Stacks().Push(shared.Stripes(), stripeIndex, entry, publish);
        if (publish) {
            shared.Terminate().Wake();
        }
    }

    void PushResidualYoungChild(MarkContext& ctx, RefField<>& field, BaseObject* holder, const char* origin)
    {
        WCollector* collector = shared.collector;
        BaseObject* target = collector->ResolveMinorReference(field);
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            return;
        }
        RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (targetRegion != nullptr && !targetRegion->IsYoungRegion()) {
            if (collector->GetGenerationCycle(GCCycleGeneration::YOUNG).IsMajorRoots()) {
                collector->MarkOldObjectIfActive(target, true);
            }
            return;
        }
        if (targetRegion == nullptr ||
            targetRegion->IsMarkedObject(targetRegion->GetMarkView<Generation::Young>(), target)) {
            return;
        }

        PushObject(ctx, target);
    }

    void PushFilteredYoung(MarkContext& ctx, BaseObject* object, const char* origin, bool finalizable = false)
    {
        if (!Heap::IsHeapAddress(object)) {
            return;
        }
        if (!object->IsValidObject()) {
            TracingCollector::WorkStack failClosed;
            shared.collector->PushYoungObject(object, failClosed, origin);
            return;
        }
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (!region->IsYoungRegion()) {
            if (shared.collector->GetGenerationCycle(GCCycleGeneration::YOUNG).IsMajorRoots()) {
                shared.collector->MarkOldObjectIfActive(object, true);
            }
            return;
        }
        if (region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object)) {
            return;
        }


        PushObject(ctx, object, finalizable);
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
            BaseObject* target = shared.collector->ResolveMinorReference(field);
            if (!ScrubMinorFreeTarget(field, target, false)) {
                PushFilteredYoung(ctx, target, "closure_edge", entry.finalizable());
            }
        };
        auto publish = [this, &ctx](const MarkStackEntry& work) { PublishEntry(ctx, work); };
        if (entry.partialArray()) {
            MarkPartialArray::FollowPartialReferences(entry, visitSlot, publish);
            return;
        }
        BaseObject* object = entry.object();
        if (!Heap::IsHeapAddress(object)) {
            return;
        }
        auto& localObjects = output.objects;
        WCollector* collector = shared.collector;
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        const bool isYoung = region->IsYoungRegion();

        if (isYoung) {
            bool wasMarked = collector->MarkEntryObject(object, entry, &ctx.Cache());
            if (wasMarked) {
                if (!entry.follow()) {
                    return;
                }

                if (object->HasRefField()) {
                    auto fields = [this, &ctx, object](RefField<>& field) {
                        PushResidualYoungChild(ctx, field, object, "ghostroute.striped.bitmap");
                    };
                    ZBasicOopIterateClosure<decltype(fields)> closure(fields);
                    ZIterator::oop_iterate(object, &closure);
                }
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
    if (youngMarkDomain == nullptr) {
        youngMarkDomain = std::make_unique<MarkDomain>(kMarkStripeMax, MarkingStacks::MarkingGeneration::YOUNG);
    }
    GCWorkers& workers = GetWorkers(GCCycleGeneration::YOUNG);
    youngMarkDomain->BindWorkers(&workers);
    youngMarkDomain->BindAbort(&collectorResources.GetYoungDriverPort().Abort());
    youngMarkDomain->PrepareWork(workers.ActiveWorkers());
    MarkingStacks::VerifyEmpty(youngMarkDomain->Stripes().Population());
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
    CHECK_DETAIL(youngMarkDomain != nullptr, "young mark domain must start before publication");
    MarkStripeSet& stripes = youngMarkDomain->Stripes();
    MarkThreadLocalStacks& publication = ThreadLocal::GetMarkStacks(*youngMarkDomain);
    publication.Push(stripes, stripes.StripeForAddress(reinterpret_cast<uintptr_t>(object)),
                     MarkStackEntry::MarkAndFollow(object), true);
}

// ZMark::mark_object<DontResurrect, GCThread, Follow, Strong>,
// zMark.inline.hpp:49-94. Claim before publishing; retain the live-count duty.
void MarkDomain::MarkRootObject(BaseObject* object)
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
    // ZMark::mark_object skips the allocating page: these objects are already
    // implicitly live. Cangjie represents that page boundary by its watermark.
    if (region->AllocatedAfterMarkStart(region->GetAddressOffset(reinterpret_cast<MAddress>(object)))) {
        return;
    }
    bool firstLive = false;
    if (region->MarkObjectWithLiveClaim(region->GetMarkView<MapleRuntime::Generation::Young>(),
                                       object, object->GetSize(), false, firstLive)) {
        return;
    }
    MarkThreadLocalStacks& publication = ThreadLocal::GetMarkStacks(*this);
    publication.Push(stripes, stripes.StripeForAddress(reinterpret_cast<uintptr_t>(object)),
                     MarkStackEntry::Claimed(object, firstLive, true, false), false);
}

void WCollector::TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,
                                          std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                          MinorSlotSet& weakSlots,
                                          const MinorSlotSet* reachableSlotDomain)
{
    g_markStripeArmed.fetch_add(1, std::memory_order_relaxed);
    const size_t dispelAtEntry = RegionInfo::GetDispelGhostCount();

    GCWorkers& workersSet = GetWorkers(GCCycleGeneration::YOUNG);
    size_t workers = workersSet.ActiveWorkers();
    if (workers == 0) {
        workers = 1;
        workersSet.SetActiveWorkers(1);
    }
    g_markStripeTurned.fetch_add(1, std::memory_order_relaxed);

    youngMarkDomain->PrepareWork(workers);
    const size_t stripeCount = youngMarkDomain->Stripes().Count();
    YoungStripedShared shared;
    shared.collector = this;
    shared.reachableSlotDomain = reachableSlotDomain;
    shared.fullYoungScan = fullYoungScan;
    shared.recordSlots = fullYoungScan;
    shared.workerCount = workers;
    shared.domain = youngMarkDomain.get();
    shared.outputs.reserve(shared.workerCount);
    for (size_t i = 0; i < shared.workerCount; ++i) {
        shared.outputs.emplace_back(std::make_unique<YoungStripedWorkerOutput>());
    }

    MarkThreadLocalStacks& seed = youngMarkDomain->Stacks();
    (void)seed.Flush(shared.Stripes(), true);
    MarkingStacks::VerifyEmpty(seed.Population());
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();
        MAddress address = 0;
        if (entry.partialArray()) {
            size_t length = 0;
            MarkPartialArray::Decode(entry, address, length);
        } else {
            address = reinterpret_cast<MAddress>(entry.object());
        }
        seed.Push(shared.Stripes(), shared.StripeFor(reinterpret_cast<BaseObject*>(address)), entry, true);
    }
    // Roots and concurrently published stripes are independent sources of work.

    (void)seed.Flush(shared.Stripes(), true);

    MarkingStacks::VerifyEmpty(seed.Population());

    YoungStripedMarkingWork task(shared);
    workersSet.Run(task);
    youngMarkDomain->FinishWork();
    if (!collectorResources.GetYoungDriverPort().Abort().Poll()) {
        MarkingStacks::VerifyEmpty(shared.Stripes().Population());
        CHECK_DETAIL(shared.Terminate().Terminated(),
                     "young striped closure returned without coordinated worker termination");
    }

    const size_t dispelAtExit = RegionInfo::GetDispelGhostCount();
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
    if (workStack.empty() && (youngMarkDomain == nullptr || youngMarkDomain->Stripes().IsEmpty())) {
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
    (void)MutatorManager::Instance().HandshakeFlushMarkProducers(youngMarkDomain.get());
    do {
        if (!workStack.empty() || !youngMarkDomain->Stripes().IsEmpty()) {
            if (windowStats != nullptr) {
                ++windowStats->closureCalls;
            }
            TraceYoungClosure(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots);
        }
        if (collectorResources.GetYoungDriverPort().Abort().Poll()) {
            return false;
        }
    } while (youngMarkDomain->TryTerminateFlush());
    CHECK_DETAIL(workStack.empty(), "young concurrent follow returned with owner work");
    return true;
}

bool WCollector::TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats)
{
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "young mark-end flush requires stopped mutators");
    NoteMarkTerminatePause();
    const size_t before = youngMarkDomain->Stripes().Population();
    (void)MutatorManager::Instance().HandshakeFlushMarkProducers(youngMarkDomain.get());
    const size_t after = youngMarkDomain->Stripes().Population();
    NoteMarkTerminateFlushed(after >= before ? after - before : 0);
    if (!workStack.empty() || !youngMarkDomain->Stripes().IsEmpty()) {
        return false;
    }
    // zMark.cpp:973-989: successful mark-end verifies the current generation.
    MarkingStacks::VerifyAllEmpty(*youngMarkDomain);
    return true;
}
void WCollector::MarkNewObject(BaseObject* obj)
{
    // Match the page owner used by MarkObjectImpl, independently of the
    // last young/old operation acknowledged by this mutator.
    const GCPhase phase = GetGCPhase(static_cast<GCCycleGeneration>(ObjectGeneration(obj)));
    if (UNLIKELY(phase == GCPhase::GC_PHASE_ENUM) || UNLIKELY(phase == GCPhase::GC_PHASE_TRACE) ||
        UNLIKELY(phase == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER)) {
        MarkObject(obj);
    }
}

void WCollector::ProcessFinalizers()
{
    FinalizerProcessor& fp = collectorResources.GetFinalizerProcessor();
    fp.ProcessReferences([this](BaseObject* obj) { return IsMarkedObject<Generation::Old>(obj); });
}

bool WCollector::PublishHandshakeMarkWork(WorkStack& work, MarkDomain* domain)
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
        if (entry.partialArray()) {
            size_t length = 0;
            MarkPartialArray::Decode(entry, address, length);
        } else {
            address = reinterpret_cast<MAddress>(entry.object());
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
            work.push_back(MarkStackEntry::MarkAndFollow(target, false));
        }
    });
}

void WCollector::PublishThreadRoot(BaseObject* object, bool young, bool follow)
{
    MarkDomain* domain = young ? youngMarkDomain.get() : majorMarkDomain.get();
    CHECK_DETAIL(domain != nullptr, "root publication requires an active mark domain");
    MarkStripeSet& stripes = domain->Stripes();
    ThreadLocal::GetMarkStacks(*domain).Push(stripes,
        stripes.StripeForAddress(reinterpret_cast<uintptr_t>(object)),
        follow ? MarkStackEntry::MarkAndFollow(object) : MarkStackEntry::MarkOnly(object), true);
}

bool WCollector::FlushThreadMarkProducers(ThreadLocalData* tls)
{
    bool published = FlushThreadMarkProducers(tls, youngMarkDomain.get());
    return FlushThreadMarkProducers(tls, majorMarkDomain.get()) || published;
}

bool WCollector::FlushThreadMarkProducers(ThreadLocalData* tls, MarkDomain* domain)
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
    MarkDomain* domain = nullptr;
    bool partial = false;
    std::atomic<size_t> newlyMarked{ 0 };

    MarkStripeSet& Stripes() { return domain->Stripes(); }
    MarkingSMR& Smr() { return domain->Smr(); }
    MarkTerminate& Terminate() { return domain->Terminate(); }
    MarkThreadLocalStacks& Stacks() { return domain->Stacks(); }

    size_t StripeFor(const MarkStackEntry& entry) const
    {
        MAddress address = 0;
        if (entry.partialArray()) {
            size_t length = 0;
            MarkPartialArray::Decode(entry, address, length);
        } else {
            address = reinterpret_cast<MAddress>(entry.object());
        }
        return domain->Stripes().StripeForAddress(address);
    }
};

class ConcurrentMarkingWork : public GCRestartableWorkerTask {
public:
    explicit ConcurrentMarkingWork(MajorMarkShared& shared) : shared(shared) {}

    void ResizeWorkers(uint32_t workers) override
    {
        shared.workerCount = workers;
        shared.domain->ResizeWorkers(workers);
    }

    void Work(uint32_t workerId) override
    {
        MarkContext local(shared.workerCount, workerId, shared.Stripes(), shared.Stacks());
        size_t nNewlyMarked = 0;
        TracingCollector::WorkStack staging;
        (void)MarkEngine::FollowWork(local, shared.Smr(), shared.Stripes(), shared.Terminate(), workerId,
                                     shared.partial,
                                     [this, &nNewlyMarked, &staging, &local](const MarkStackEntry& entry) {
                                         ProcessEntry(local, entry, nNewlyMarked, staging);
                                     },
                                     nullptr, nullptr, shared.domain);
        (void)local.Stacks().Flush(shared.Stripes(), true);
        local.Cache().Flush();
        shared.newlyMarked.fetch_add(nNewlyMarked, std::memory_order_relaxed);
        MarkingStacks::VerifyEmpty(local.Stacks().Population());
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
        BaseObject* obj = entry.object();
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
    if (majorMarkDomain == nullptr) {
        majorMarkDomain = std::make_unique<MarkDomain>(64, MarkingStacks::MarkingGeneration::MAJOR);
    }
    GCWorkers& workers = GetWorkers(GCCycleGeneration::OLD);
    majorMarkDomain->BindWorkers(&workers);
    majorMarkDomain->BindAbort(&collectorResources.GetMajorDriverPort().Abort());
    majorMarkDomain->PrepareWork(workers.ActiveWorkers());
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
    CHECK_DETAIL(majorMarkDomain != nullptr, "old mark domain must start before publication");
    MarkStripeSet& stripes = majorMarkDomain->Stripes();
    MarkThreadLocalStacks& publication = ThreadLocal::GetMarkStacks(*majorMarkDomain);
    publication.Push(stripes, stripes.StripeForAddress(reinterpret_cast<uintptr_t>(object)),
                     gcThread ? MarkStackEntry::FollowOnly(object) : MarkStackEntry::MarkAndFollow(object), true);
}

size_t TracingCollector::RunMajorStripeMark(WorkStack& workStack, bool partial)
{
    GCWorkers& workersSet = GetWorkers(GCCycleGeneration::OLD);
    const uint32_t workers = workersSet.ActiveWorkers();
    if (majorMarkDomain == nullptr) {
        majorMarkDomain = std::make_unique<MarkDomain>(64, MarkingStacks::MarkingGeneration::MAJOR);
    }
    majorMarkDomain->BindWorkers(&workersSet);
    majorMarkDomain->BindAbort(&collectorResources.GetMajorDriverPort().Abort());
    majorMarkDomain->PrepareWork(workers);
    MajorMarkShared shared;
    shared.collector = this;
    shared.workerCount = workers;
    shared.partial = partial;
    shared.domain = majorMarkDomain.get();

    MarkThreadLocalStacks& seed = majorMarkDomain->Stacks();
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();
        seed.Push(shared.Stripes(), shared.StripeFor(entry), entry, true);
    }
    (void)seed.Flush(shared.Stripes(), true);


    ConcurrentMarkingWork task(shared);
    workersSet.Run(task);
    majorMarkDomain->FinishWork();
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
        if (!workStack.empty() || !majorMarkDomain->Stripes().IsEmpty()) {
            markedObjectCount.fetch_add(RunMajorStripeMark(workStack), std::memory_order_relaxed);
        }
        if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
            return;
        }
    } while (FlushMarkProducers(majorMarkDomain.get()));
    MarkingStacks::VerifyEmpty(GetWorkers(GCCycleGeneration::OLD).GetSnapshot().remainingWorkers);
}

void TracingCollector::ProcessExportRoots(WorkStack& foreignRootsSet)
{
    while (!foreignRootsSet.empty()) {
        if (collectorResources.GetMajorDriverPort().Abort().Poll()) {
            return;
        }
        const MarkStackEntry entry = foreignRootsSet.back();
        foreignRootsSet.pop_back();
        BaseObject* exportObj = entry.object();
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
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    bool firstLive = entry.incLive();
    bool already = false;
    if (entry.mark()) {
        if (entry.finalizable()) {
            already = region->ResurrectObjectWithLiveClaim(
                obj, region->GetAddressOffset(reinterpret_cast<MAddress>(obj)), false, firstLive);
        } else {
            already = region->MarkObjectByOwnerWithLiveClaim(obj, obj->GetSize(), false, firstLive);
        }
    }
    if (!already && firstLive) {
        if (cache != nullptr) {
            cache->IncLive(region, obj->GetSize());
        } else {
            region->AddLiveCounts(1, obj->GetSize());
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
                          MarkDomain* domain)
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
                  size_t workerId, const MarkEngine::Process& process, MarkDomain* domain)
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

MarkEngine::Result MarkEngine::FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                                          MarkTerminate& terminate, size_t workerId, bool partial,
                                          const Process& process, std::atomic<size_t>* stealSuccess,
                                          std::atomic<size_t>* stealFailure, MarkDomain* domain)
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
            smr.Reclaim(workerId);
            return Result::Completed;
        }
    }
}

MarkDomain::MarkDomain(size_t capacity, MarkingStacks::MarkingGeneration generation)
    : generation(generation), stripes(capacity)
{
    stripes.SetTerminate(&terminate);
}

void MarkDomain::EnsureWorkers(size_t workers)
{
    if (smr == nullptr || smr->WorkerCount() < workers) {
        smr = std::make_unique<MarkingSMR>(workers);
    }
}

void MarkDomain::PrepareWork(size_t workers)
{
    CHECK_DETAIL(workers != 0, "mark domain needs a worker");
    nworkers = workers;
    targetNStripes = stripes.CalculateNStripes(workers);
    stripes.SetNStripes(targetNStripes);
    EnsureWorkers(workers);
    terminate.Reset(workers);
    proactiveFlushes = 0;
}

void MarkDomain::ResizeWorkers(size_t workers)
{
    // ZMark::resize_workers keeps this task's proactive flush budget.
    CHECK_DETAIL(workers != 0, "mark domain needs a worker");
    nworkers = workers;
    targetNStripes = stripes.CalculateNStripes(workers);
    stripes.SetNStripes(targetNStripes);
    EnsureWorkers(workers);
    terminate.Reset(workers);
}

void MarkDomain::FinishWork() {}

bool MarkDomain::PollStop()
{
    if (abortToken != nullptr && abortToken->Poll()) {
        return true;
    }
    if (gcWorkers != nullptr && gcWorkers->ShouldWorkerResize()) {
        return true;
    }
    return false;
}

MarkThreadLocalStacks& MarkDomain::Stacks()
{
    return ThreadLocal::GetMarkStacks(*this);
}

bool MarkDomain::FlushStacks()
{
    return ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), *this);
}

bool MarkDomain::TryProactiveFlush(size_t workerId)
{
    // zMark.cpp:608-623, zGlobals.hpp:87. Only worker zero changes this count.
    constexpr size_t proactiveFlushMax = 10;
    if (workerId != 0 || proactiveFlushes == proactiveFlushMax) {
        return false;
    }
    ++proactiveFlushes;
    return MutatorManager::Instance().HandshakeFlushMarkProducers(this) || !stripes.IsEmpty();
}

bool MarkDomain::TryTerminateFlush()
{
    // Called by the coordinator after every worker has flushed at its task exit.
    return MutatorManager::Instance().HandshakeFlushMarkProducers(this) || !stripes.IsEmpty();
}

bool MarkDomain::TryEnd()
{
    (void)FlushStacks();
    return stripes.IsEmpty();
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
void VerifyAllEmpty(MarkDomain& domain)
{
    if (!ZVerifyMarking) { return; }
    const size_t index = domain.Generation() == MarkingGeneration::YOUNG ? 0 : 1;
    MutatorManager::Instance().VisitMarkingThreads([&](const ThreadLocalData* tls) {
        if (tls == nullptr || tls->gcData == nullptr) { return; }
        const auto& stacks = tls->gcData->markStacks[index];
        CHECK_DETAIL(stacks == nullptr || stacks->IsEmpty(),
                     "Thread marking stack is not empty: thread=%p generation=%zu", tls, index);
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
bool Encodable(const void* chunkStart, size_t length)
{
    const MAddress addr = reinterpret_cast<MAddress>(chunkStart);
    const MAddress base = Heap::GetHeapStartAddress();
    if (addr < base) {
        return false;
    }
    // Encode/Decode store (addr - base) >> MIN_SIZE_SHIFT and reconstruct
    // base + (offset << MIN_SIZE_SHIFT).  Keep this predicate relative to
    // that same base (zMark.cpp:177-186).
    if (((addr - base) & (MIN_SIZE - 1)) != 0) {
        return false;
    }
    if (length == 0 || length > MAX_LENGTH) {
        return false;
    }
    return ((addr - base) >> MIN_SIZE_SHIFT) <= MAX_OFFSET;
}

MarkStackEntry Encode(const void* chunkStart, size_t length, bool finalizable)
{
    const MAddress addr = reinterpret_cast<MAddress>(chunkStart);
    const size_t offset = static_cast<size_t>((addr - Heap::GetHeapStartAddress()) >> MIN_SIZE_SHIFT);
    return MarkStackEntry::PartialArray(offset, length, finalizable);
}

void Decode(const MarkStackEntry& entry, MAddress& chunkStart, size_t& length)
{
    const size_t offset = entry.partialArrayOffset();
    length = entry.partialArrayLength();
    chunkStart = Heap::GetHeapStartAddress() + (offset << MIN_SIZE_SHIFT);
}

// ZGC zMark.cpp:216-270. Always visit the leading range locally; only
// publish ranges whose complete descriptor can be represented. The inline
// fallback visits every field of a legal but unencodable array.
void FollowElements(MAddress start, size_t length, bool finalizable,
                    const FieldVisitor& visit, const EntryPublisher& publish)
{
    const MAddress end = start + length * sizeof(MAddress);
    const MAddress middleStart = AlignUp(start + sizeof(MAddress), MIN_SIZE);
    if (length <= MIN_LENGTH || length > MAX_LENGTH ||
        !Encodable(reinterpret_cast<const void*>(AlignDown(end, MIN_SIZE)), 1)) {
        for (size_t i = 0; i < length; ++i) {
            visit(start + i * sizeof(MAddress));
        }
        return;
    }
    const size_t middleLength = AlignDown((end - middleStart) / sizeof(MAddress), MIN_LENGTH);
    const MAddress middleEnd = middleStart + middleLength * sizeof(MAddress);
    auto push = [&](MAddress address, size_t count) {
        if (!Encodable(reinterpret_cast<const void*>(address), count)) {
            for (size_t i = 0; i < count; ++i) {
                visit(address + i * sizeof(MAddress));
            }
            return;
        }
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
