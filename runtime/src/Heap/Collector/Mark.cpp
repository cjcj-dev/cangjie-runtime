// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/WCollector/WCollector.h"
#include "Heap/WCollector/RememberedHolderPolicy.h"
#include "Heap/Verify/ProbeReadRouteDiag.h"

#include <array>
#include <atomic>
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include <csignal>
#endif
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

#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Base/SysCall.h"
#endif
#include "Concurrency/Concurrency.h"
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Collector/GcTriggerFlags.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/Collector/MarkStripe.h"
#include "Heap/Collector/TenuringThreshold.h"
#include "Heap/GcThreadPool.h"
#include "Heap/HeapWork.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Heap/WCollector/UntagRefFieldBreadcrumb.h"
#endif
#include "Heap/Verify/VerifyHeap.h"
#include "Heap/Verify/MarkCompleteVerify.h"
#include "Heap/Verify/VerifyOption.h"
#include "Heap/Verify/VerifyRememberedSet.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/VerifyRoots.h"
#include "Heap/Verify/Zap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/NwDropAudit.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/Stw2CurrentAudit.h"
#include "Heap/Verify/MarkCompleteVerify.h"
#include "Heap/Verify/SurvNodeDiag.h"
#include "Heap/Collector/PromotedRegionDomain.h"
#include "Heap/Verify/CsetEmptyWho.h"
#include "Common/ColourPredicates.h"
#include "Heap/WCollector/RemapYoungRoots.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Verify/VerifyRegions.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "securec.h"
#endif
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::atomic<uint64_t> g_youngWeakSerialDiscoveries{ 0 };
std::atomic<uint64_t> g_youngWeakLegacyParallelDiscoveries{ 0 };
std::atomic<uint64_t> g_youngWeakStripedDiscoveries{ 0 };
} // namespace

void ResetYoungWeakClosureTestReceipt()
{
    g_youngWeakSerialDiscoveries.store(0, std::memory_order_relaxed);
    g_youngWeakLegacyParallelDiscoveries.store(0, std::memory_order_relaxed);
    g_youngWeakStripedDiscoveries.store(0, std::memory_order_relaxed);
}

void NoteYoungWeakClosureDiscovery(YoungWeakClosureVariant variant)
{
    switch (variant) {
        case YoungWeakClosureVariant::SERIAL:
            g_youngWeakSerialDiscoveries.fetch_add(1, std::memory_order_relaxed);
            return;
        case YoungWeakClosureVariant::LEGACY_PARALLEL:
            g_youngWeakLegacyParallelDiscoveries.fetch_add(1, std::memory_order_relaxed);
            return;
        case YoungWeakClosureVariant::STRIPED:
            g_youngWeakStripedDiscoveries.fetch_add(1, std::memory_order_relaxed);
            return;
    }
}

YoungWeakClosureTestReceipt ReadYoungWeakClosureTestReceipt()
{
    return { g_youngWeakSerialDiscoveries.load(std::memory_order_relaxed),
             g_youngWeakLegacyParallelDiscoveries.load(std::memory_order_relaxed),
             g_youngWeakStripedDiscoveries.load(std::memory_order_relaxed) };
}
#endif

bool WCollector::MarkObject(BaseObject* obj) const
{
    return MarkObjectImpl(obj, false);
}

bool WCollector::MarkObjectImpl(BaseObject* obj, bool youngClaim, MarkLiveCache* liveCache) const
{
    // markfloor: work stack may hold RawArray+8 interiors (tip word = length, e.g. 0x200).
    // Return true ⇒ ConcurrentMarkingWork treats as already-marked and skips HasRefField.
    if (!Collector::PlausibleManagedObjectGate("WCollector::MarkObject", obj)) {
        SurvNodeDiag::NoteFollowHolder(obj, SurvNodeDiag::FOLLOW_SKIP_GATE);
        return true;
    }
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));

    size_t objectSize = obj->GetSize();
    // livesame: MarkObject adds live only on 0→1 (ZGC inc_live); no second AddLiveByteCount.
    // ZGC zPage.inline.hpp:284-294: the target page owns mark authority. gcReason
    // names the running closure, not the target object's face.
    // When liveCache is set, mark bits stay atomic and live bytes are coalesced
    // per worker (ZGC zMarkCache.hpp).
    bool marked = region->MarkObjectByOwner(obj, objectSize, liveCache == nullptr);
    if (!marked && liveCache != nullptr) {
        liveCache->IncLive(region, objectSize);
    }
    if (!marked) {
        SurvNodeDiag::NotePaint(obj, region);
    }

    if (!marked) {
        DLOG(TRACE, "mark obj %p<%p>(%zu) in region %p(%u)@%#zx, live %zu", obj, obj->GetTypeInfo(), objectSize,
             region, region->GetRegionType(), region->GetRegionStart(), region->GetLiveByteCount());
    }
    return marked;
}

bool WCollector::ResurrectObject(BaseObject* obj, size_t offset, RegionInfo* region)
{
    // getsize7: ResurrectObject → GetSize; finalizer work-stack should be gated but base path was not.
    if (!Collector::PlausibleManagedObjectGate("WCollector::ResurrectObject", obj)) {
        return true;
    }
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
    // A mark-good root has passed this mark epoch and is necessarily load-good
    // (OpenJDK zAddress.inline.hpp:658-664).
    if (is_mark_good(oldField)) {
        // Anchor main 8cd248497dd8c251ca824d9f089d5e30125c80c9
        BaseObject* target = to_object(oldField.GetTargetObject());
        // Plain/uncoloured non-null is mark-good under g_cjMarkBadMask; mirror the slow path.
        // Reject non-heap: do not call make_load_good (remap would touch non-heap).
        if (!Collector::MarkGoodHeapGate("EnumRefFieldRoot", target)) {
            return;
        }
        if (!Collector::PlausibleManagedObjectGate("EnumRefFieldRoot", target)) {
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

    BaseObject* latest = make_load_good(oldField);

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    if (!Collector::PlausibleManagedObjectGate("EnumRefFieldRoot.slow", latest)) {
        return;
    }
    if (VerifyRoots::Enabled()) {
        RootVerifyContext vctx;
        vctx.phase = "EnumRefFieldRoot";
        vctx.kind = RootKind::STATIC_ROOT;
        VerifyRoots::VerifyRootPayload(vctx, &field, latest);
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

void WCollector::EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const
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
        BaseObject* to = FindToVersion(root).GetOrFailClosed("WCollector::MarkStackRoots");
        if (to != nullptr) {
            root = to;
        }
    }
    if (!Collector::PlausibleManagedObjectGate("EnumAndTagRawRoot.plain", root)) {
        // introot: a raw-root stack-map entry may still identify RawArray+8.
        // The paired derived path cannot reach this branch because it is a DerivedSlot.
        BaseObject* host = Collector::TryRecoverInteriorBase(root);
        if (host != nullptr && host->IsValidObject()) {
            HealRootWriteback(ref, root, HealSite::WCollectorEnumRawInteriorRoot);
            rootSet.push_back(host);
        }
        return;
    }
    CHECK_DETAIL(root->IsValidObject(), "Enum and tag runtime root %p(%p) encounters invalid object", root, &ref);
    ProbeReadRouteDiag::NoteRoot(reinterpret_cast<MAddress>(root), reinterpret_cast<MAddress>(&ref),
                                 static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed)),
                                 ProbeReadRouteDiag::RootKind::MajorRaw);
    HealRootWriteback(ref, root, HealSite::WCollectorEnumRawRoot);
    rootSet.push_back(root);
}

// note each ref-field will not be traced twice, so each old pointer the tracer meets must come from previous gc.
void WCollector::TraceRefField(BaseObject* obj, RefField<>& field, WorkStack& workStack) const
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
        if (!Collector::MarkGoodHeapGate("TraceRefField", targetObj)) {
            // gatedrop: reject arm only (default off). leave untraced.

            SurvNodeDiag::NoteTraceVisit(&field, targetObj, SurvNodeDiag::TRACE_SKIP_GATE);
            return;
        }
        // markfloor: skip interiors (RawArray+8 etc.) before IsValidObject/GetSize.
        if (!Collector::PlausibleManagedObjectGate("TraceRefField", targetObj)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(targetObj);
            if (host != nullptr && host != targetObj &&
                Collector::PlausibleManagedObjectGate("TraceRefField.host", host)) {
                targetObj = host;
            } else {

                SurvNodeDiag::NoteTraceVisit(&field, targetObj, SurvNodeDiag::TRACE_SKIP_GATE);
                return;
            }
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

            SurvNodeDiag::NoteTraceVisit(&field, targetObj, SurvNodeDiag::TRACE_PUSH);
            workStack.push_back(targetObj);
        } else {
            SurvNodeDiag::NoteTraceVisit(&field, targetObj, SurvNodeDiag::TRACE_SKIP_MARKED);
        }
        return;
    }

    BaseObject* latest = make_load_good(oldField);

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    if (!Collector::PlausibleManagedObjectGate("TraceRefField.slow", latest)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(latest);
        if (host != nullptr && host != latest &&
            Collector::PlausibleManagedObjectGate("TraceRefField.slow.host", host)) {
            latest = host;
        } else {

            SurvNodeDiag::NoteTraceVisit(&field, latest, SurvNodeDiag::TRACE_SKIP_GATE);
            return;
        }
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by strong object %p: %s and offset %zd",
                 latest, obj, obj == nullptr ? "<partial-array chunk>" : obj->GetTypeInfo()->GetName(),
                 obj == nullptr ? static_cast<ssize_t>(-1) : BaseObject::FieldOffset(obj, &field));
    RefField<> newField = ColourResolvedRefField(latest);
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

        SurvNodeDiag::NoteTraceVisit(&field, latest, SurvNodeDiag::TRACE_PUSH);
        workStack.push_back(latest);
    } else {
        SurvNodeDiag::NoteTraceVisit(&field, latest, SurvNodeDiag::TRACE_SKIP_MARKED);
    }
}

// Ported from ZGC ZMark::push_partial_array (zMark.cpp:185-196). ZGC pushes a
// tagged entry onto its mark stack; we push the same descriptor encoded into a
// BaseObject* slot of our work stack, so TryForkTask can hand it to another
// worker. If the descriptor does not fit one word we trace the chunk here
// rather than drop it -- correctness never depends on the encoding succeeding.
void WCollector::PushPartialArray(RefField<>* addr, size_t length, WorkStack& workStack) const
{
    if (UNLIKELY(!MarkPartialArray::Encodable(addr, length))) {
        MarkPartialArray::NoteNotEncodable();
        FollowArrayElementsSmall(nullptr, addr, length, workStack);
        return;
    }
    MarkPartialArray::NoteChunkPushed();
    workStack.push_back(MarkPartialArray::Encode(addr, length));
}

// zMark.cpp:208-214 (follow_array_elements_small).
void WCollector::FollowArrayElementsSmall(BaseObject* holder, RefField<>* addr, size_t length,
                                          WorkStack& workStack) const
{
    for (size_t i = 0; i < length; ++i) {
        TraceRefField(holder, addr[i], workStack);
    }
}

// zMark.cpp:216-255 (follow_array_elements_large), transcribed.
void WCollector::FollowArrayElementsLarge(BaseObject* holder, RefField<>* addr, size_t length,
                                          WorkStack& workStack) const
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

    MarkPartialArray::NoteArraySplit();

    // Push unaligned trailing part
    if (end > middleEnd) {
        PushPartialArray(middleEnd, static_cast<size_t>(end - middleEnd), workStack);
    }

    // Push aligned middle part(s)
    RefField<>* partialAddr = middleEnd;
    while (partialAddr > middleStart) {
        const size_t parts = 2;
        const size_t partialLength = AlignUp(static_cast<size_t>(partialAddr - middleStart) / parts,
                                             MarkPartialArray::MIN_LENGTH);
        partialAddr -= partialLength;
        PushPartialArray(partialAddr, partialLength, workStack);
    }

    // Follow leading part
    CHECK_DETAIL(start < middleStart, "Miscalculated middle start");
    FollowArrayElementsSmall(holder, start, static_cast<size_t>(middleStart - start), workStack);
}

// zMark.cpp:257-263 (follow_array_elements). The encodability probe has no ZGC
// counterpart: ZGC bounds its heap so the entry always fits, whereas ours is
// only checked here. Failing it means "trace inline", i.e. today's behaviour.
void WCollector::FollowArrayElements(BaseObject* holder, RefField<>* addr, size_t length,
                                     WorkStack& workStack) const
{
    if (length <= MarkPartialArray::MIN_LENGTH) {
        FollowArrayElementsSmall(holder, addr, length, workStack);
        return;
    }
    // Every chunk this split can produce starts inside [addr, addr+length) and
    // is no longer than `length`, so probing the last element bounds them all.
    if (UNLIKELY(length > MarkPartialArray::MAX_LENGTH ||
                 !MarkPartialArray::Encodable(
                     reinterpret_cast<const void*>(
                         AlignDown(reinterpret_cast<MAddress>(addr + length),
                                   static_cast<MAddress>(MarkPartialArray::MIN_SIZE))),
                     1))) {
        MarkPartialArray::NoteNotEncodable();
        FollowArrayElementsSmall(holder, addr, length, workStack);
        return;
    }
    FollowArrayElementsLarge(holder, addr, length, workStack);
}

// zMark.cpp:265-270 (follow_partial_array).
void WCollector::FollowPartialArray(const MarkStackEntry& entry, WorkStack& workStack)
{
    MAddress chunkStart = 0;
    size_t length = 0;
    MarkPartialArray::Decode(entry, chunkStart, length);
    MarkPartialArray::NoteChunkFollowed();
    FollowArrayElements(nullptr, &HeapSlotAt<>(chunkStart), length, workStack);
}

void WCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack)
{
    if (UNLIKELY(MarkCompleteVerify::Enabled())) {
        MarkCompleteVerify::NoteHolderTrace(obj);
    }
    auto visitor = [this, obj, &workStack](RefField<>& field) { TraceRefField(obj, field, workStack); };
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
            if (UNLIKELY(MarkPartialArray::Enabled())) {
                FollowArrayElements(obj, arrayContent, arrayLength, workStack);
                return;
            }
            for (MIndex i = 0; i < arrayLength; ++i) {
                visitor(arrayContent[i]);
            }
        } else {
            LOG(RTLOG_FATAL, "array object %p has wrong component type", array);
        }
        return;
    }

    MAddress contentAddr = reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    obj->GetGCTib().ForEachBitmapWord(contentAddr, visitor);
}

BaseObject* WCollector::GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field)
{
    RefField<> oldField(field);
    const char* sourceKind = kind == RefSlotKind::WEAK_REFERENT ? "weak" : "strong";
    BaseObject* latest = nullptr;
    if (is_mark_good(oldField)) {
        BaseObject* targetObj = to_object(oldField.GetTargetObject());
        if (!Collector::MarkGoodHeapGate("GetAndTryTagObj", targetObj)) {
            return nullptr;
        }
        // Anchor main ced6b14fe41380fd2dfb94c91b7fe6973786a80e
        CHECK_DETAIL(targetObj->IsValidObject(),
                     "Invalid object %p is referenced by %s object %p: %s and offset %zd", targetObj, sourceKind, obj,
                     obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
        return targetObj;
    }
    latest = make_load_good(oldField);
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
void WCollector::SeedOldMarkFromYoungSurvivors(WorkStack& workStack, std::vector<BaseObject*>* collectOnly)
{
    // Nested young (DoYoungGarbageCollection) traces only young targets
    // (TraceYoungClosure skips !IsYoungRegion). ZGC overlapping mark paints
    // whichever generation the stored address lives in
    // (zBarrier.inline.hpp:742-749 mark()). Seed old objects named by the
    // remaining young survivors so old TRACE sees those young→old edges.
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    size_t holders = 0;
    size_t seeded = 0;
    size_t painted = 0;
    auto seedFrom = [this, &workStack, collectOnly, &holders, &seeded, &painted](RegionInfo* region) {
        // Before Assemble: current-space only. Nested young has already
        // promoted survivors (IsYoungRegion=false) onto recentFull.
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion() ||
            region->IsFromRegion() || region->IsLoneFromRegion() ||
            region->IsUnmovableFromRegion()) {
            return;
        }
        ++holders;
        region->VisitAllObjects([this, &workStack, collectOnly, &seeded, &painted](BaseObject* obj) {
            if (obj == nullptr || !obj->HasRefField() || obj->IsWeakRef()) {
                return;
            }
            if (!Collector::PlausibleManagedObjectGate("SeedOldMarkFromYoungSurvivors.holder", obj)) {
                return;
            }
            obj->ForEachRefField([this, &workStack, collectOnly, &seeded, &painted](RefField<>& field) {
                BaseObject* target = to_object(field.GetTargetObject());
                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                    return;
                }
                if (!Collector::PlausibleManagedObjectGate("SeedOldMarkFromYoungSurvivors.target",
                                                           target)) {
                    BaseObject* host = Collector::TryRecoverInteriorBase(target);
                    if (host == nullptr || host == target) {
                        return;
                    }
                    target = host;
                }
                RegionInfo* tr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                if (tr == nullptr || tr->IsYoungRegion() || tr->IsFreeRegion() ||
                    tr->IsGarbageRegion()) {
                    return;
                }
                ++seeded;
                if (collectOnly != nullptr) {
                    collectOnly->push_back(target);
                    return;
                }
                // zMark.inline.hpp:58-65 mark_before_push: paint on the GC
                // thread so a bounded work stack cannot drop the live bit.
                if (MarkObject(target)) {
                    return;
                }
                ++painted;
                // The GC-thread claim above already painted and counted the
                // object. Keep the independent follow obligation in the entry;
                // a plain pointer used to be popped as already-marked and lost
                // this old closure frontier.
                workStack.push_back(MarkStackEntry::FollowOnly(target));
            });
        });
    };
    manager.VisitAllManagedRegionsForProbe([&seedFrom](RegionInfo* region, const char*) {
        seedFrom(region);
    });
    LOG(RTLOG_ERROR, "[WHODEAD][oldseed] youngHolders=%zu oldSeeded=%zu painted=%zu stack=%zu collect=%zu",
        holders, seeded, painted, workStack.size(), collectOnly != nullptr ? collectOnly->size() : 0);
}

void WCollector::TraceHeap()
{
    WorkStack workStack = NewWorkStack();
    WorkStack foreignStack = NewWorkStack();
    // Collect young→old targets before Assemble (survivors still current-space).
    // Paint after Assemble+PrepareTrace so ClearLiveInfo cannot wipe the bits
    // (zMark.inline.hpp:58-65 mark_before_push).
    std::vector<BaseObject*> youngToOld;
    SeedOldMarkFromYoungSurvivors(workStack, &youngToOld);
    // assemble garbage candidates for tracing.
    reinterpret_cast<RegionSpace&>(theAllocator).AssembleGarbageCandidates();

    // Full-colour gate: reject any plain HeapSlot before major mark.

    const bool concurrentStackScan = MutatorManager::ConcurrentStackScanEnabled();
    uint64_t stackScanEpoch = 0;

    if (concurrentStackScan) {
        // Publish the old mark colour and ENUM barrier while every mutator is stopped.
        // Nested young already flipped young (DoYoungGarbageCollection). A second
        // flip_young_mark_start here XOR-undoes that colour and the remset face.
        // ZGenerationOld::mark_start only flips old (zGeneration.cpp:1074-1077 / :1219).
        ScopedStopTheWorld stw("major stack scan prepare", false);
        flip_old_mark_start();
        Heap::GetHeap().InstallBarrier(GCPhase::GC_PHASE_ENUM);
        Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
    } else {
        // After the nested young collection, this is old mark-start only.
        // VM_ZMarkStartYoungAndOld (zGeneration.cpp:583-605) flips both in one
        // pause when the generations start together. We already ran a full young
        // cycle; flipping young again here undoes its mark/remset colours and
        // left old from-pages unmarked (SD256 CSet-empty ke=0 residual keep≈99%).
        // Match ZGenerationOld::mark_start (zGeneration.cpp:1212-1219).
        ScopedStopTheWorld stw("major mark start", false);
        flip_old_mark_start();
    }

    if (concurrentStackScan) {

        EpochHandshakeStats handshake = MutatorManager::Instance().RunEpochHandshake("pre-major-stack");
        stackScanEpoch = handshake.epoch;
        CHECK_DETAIL(stackScanEpoch != 0 && handshake.stackScanned + handshake.stackFallback == handshake.requested,
                     "major concurrent stack scan accounting failed: epoch=%llu requested=%zu scanned=%zu "
                     "fallback=%zu",
                     static_cast<unsigned long long>(stackScanEpoch), handshake.requested, handshake.stackScanned,
                     handshake.stackFallback);
    }

    {
        MRT_PHASE_TIMER("enum roots & update old pointers within");
        if (concurrentStackScan) {
            // This is major's root-enumeration closing edge. StopTheWorld establishes
            // InSaferegion for the fixed mutator roster, so WM_OWNER_GC may finish a
            // different mutator's epoch cursor. If completion still cannot be
            // established, run the legacy enum but leave the watermark incomplete;
            // the report-only postcondition below must observe that residual state.
            {
                ScopedStopTheWorld stw("major stack scan close", false);
                TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
                MutatorManager::Instance().VisitAllMutators([stackScanEpoch](Mutator& mutator) {
                    if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                        (void)mutator.GcPhaseEnum(GCPhase::GC_PHASE_ENUM, stackScanEpoch, false);
                    }
                    if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                        (void)mutator.GcPhaseEnum(GCPhase::GC_PHASE_ENUM);
                    }
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
            VerifyStackRootPostcondition(stackScanEpoch, "major");

            TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        } else {
            TransitionToGCPhase(GCPhase::GC_PHASE_ENUM, true);
            DoEnumeration(workStack, foreignStack);
        }
    }

    {
        MRT_PHASE_TIMER("trace live objects & update old pointers in ref-fields");
        markedObjectCount.store(0, std::memory_order_relaxed);
        if (!concurrentStackScan) {
            TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        }
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        {
            // Push unmarked only. ConcurrentMarkingWork skips follow when
            // MarkObject already returned true (wasMarked). Pre-paint would
            // leave young→old edges as live bits without a field scan.
            size_t pushed = 0;
            for (BaseObject* target : youngToOld) {
                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                    continue;
                }
                if (IsMarkedObject<Generation::Old>(target)) {
                    continue;
                }
                workStack.push_back(target);
                ++pushed;
            }
            LOG(RTLOG_ERROR, "[WHODEAD][oldseed] post-assemble pushed=%zu stack=%zu from=%zu", pushed,
                workStack.size(), youngToOld.size());
        }
        DoTracing(workStack, foreignStack);

        ProcessFinalizers();
    }

    // ZVerify::after_mark (zVerify.cpp:496-506) runs here, between mark completing and
    // anything acting on the mark face.  PostTrace -> HandleTraceRegions is the first
    // consumer, so this is the last instant at which "mark says X is dead" can still be
    // contradicted by a live holder rather than by a crash three phases later.
    MarkCompleteVerify::RunAtMarkEnd("major-mark-end");
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
    MutatorManager::Instance().VisitAllMutators([&](Mutator& mutator) {
        if (stackScanEpoch != 0 && mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
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
    gMinorRootOrigin = "static";
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    Heap::GetHeap().VisitStaticRoots([&remset, &visitedRawRootVisitor](RootSlot& root) {
        remset.VisitStaticForCrossCheck(reinterpret_cast<MAddress>(&root));
        visitedRawRootVisitor(root);
    });
#else
    Heap::GetHeap().VisitStaticRoots(visitedRawRootVisitor);
#endif
    gMinorRootOrigin = "concurrency";
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitedRawRootVisitor);
    gMinorRootOrigin = "finalizer";
    collectorResources.GetFinalizerProcessor().VisitRawPointers(visitedRawRootVisitor);
    gMinorRootOrigin = "export";
    Heap::GetHeap().VisitAllExportRoots(visitedRawRootVisitor);
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    remset.CheckStaticCoverageForMinor();
#endif
    gMinorRootOrigin = "unknown";
}

void WCollector::VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
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
    gMinorRootOrigin = "value_cycle";
    for (const auto& entry : cycleRefWorkStack) {
        visitor(entry.first);
        for (BaseObject* object : entry.second) {
            visitor(object);
        }
    }
    gMinorRootOrigin = "unknown";
}

void WCollector::VisitMinorRoots(const std::function<void(BaseObject*)>& visitor,
                                 const std::function<void(BaseObject*)>& invisibleVisitor,
                                 uint64_t stackScanEpoch)
{
    RootVisitor rawRootVisitor = [this, &visitor](ObjectRef& root) {
        BaseObject* obj = ResolveMinorReference(root);
        if (obj != nullptr && Heap::IsHeapAddress(obj)) {
            ProbeReadRouteDiag::NoteRoot(reinterpret_cast<MAddress>(obj), reinterpret_cast<MAddress>(&root),
                                         static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed)),
                                         ProbeReadRouteDiag::RootKind::MinorRaw);
        }

        if (obj != nullptr && Heap::IsHeapAddress(obj) &&
            !Collector::PlausibleManagedObjectGate("VisitMinorRoots.raw", obj)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(obj);
            if (host != nullptr) {
                visitor(host);
            }
            return;
        }
        visitor(obj);
    };
    RootVisitor invisibleRootVisitor = [this, &invisibleVisitor](ObjectRef& root) {
        BaseObject* obj = ResolveMinorReference(root);
        if (obj != nullptr && Heap::IsHeapAddress(obj) &&
            !Collector::PlausibleManagedObjectGate("VisitMinorRoots.invisible", obj)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(obj);
            if (host != nullptr) {
                invisibleVisitor(host);
            }
            return;
        }
        invisibleVisitor(obj);
    };
    VisitMinorRootSlots(rawRootVisitor, invisibleRootVisitor, stackScanEpoch);
    VisitMinorValueRoots(visitor);
}

void WCollector::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin) const
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    // markfloor / introot: interiors (RawArray+8) pass IsValidObject (tip=length≠null).
    // Recover host object so the live array is marked; do not push the interior itself.
    if (!Collector::PlausibleManagedObjectGate("PushYoungObject", object)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(object);
        if (host != nullptr && host != object) {
            PushYoungObject(host, workStack, origin);
        }
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
            // HEADER_DUMP: first 64 bytes as hex + field decode + zap check.
            auto* bytes = reinterpret_cast<const uint8_t*>(object);
            char hex[64 * 2 + 16];
            size_t pos = 0;
            for (size_t i = 0; i < 64 && pos + 2 < sizeof(hex); ++i) {
                static const char* kHex = "0123456789abcdef";
                hex[pos++] = kHex[(bytes[i] >> 4) & 0xf];
                hex[pos++] = kHex[bytes[i] & 0xf];
            }
            hex[pos] = '\0';
            uint64_t w0 = 0;
            uint64_t w1 = 0;
            uint64_t w2 = 0;
            uint64_t w3 = 0;
            std::memcpy(&w0, bytes + 0, sizeof(w0));
            std::memcpy(&w1, bytes + 8, sizeof(w1));
            std::memcpy(&w2, bytes + 16, sizeof(w2));
            std::memcpy(&w3, bytes + 24, sizeof(w3));
            bool allZero = true;
            for (size_t i = 0; i < 64; ++i) {
                if (bytes[i] != 0) {
                    allZero = false;
                    break;
                }
            }
            bool isZap = HeapZap::IsZapWord(static_cast<uintptr_t>(w0));
            // tipBits: raw first 48 bits of header word (layout-dependent; not GetTypeInfo).
            uintptr_t tipBits = (static_cast<uintptr_t>(w0) & 0xffffffffffffULL);
            VLOG(REPORT,
                 "[GCV2][HEADER_DUMP] obj=%p hex64=%s w0=%#llx w1=%#llx w2=%#llx w3=%#llx "
                 "allZero=%u isZapWord=%u tipBits48=%#zx ZAP_WORD=%#llx "
                 "ZAP_VERDICT_%s",
                 object, hex, static_cast<unsigned long long>(w0), static_cast<unsigned long long>(w1),
                 static_cast<unsigned long long>(w2), static_cast<unsigned long long>(w3),
                 static_cast<unsigned>(allZero), static_cast<unsigned>(isZap), tipBits,
                 static_cast<unsigned long long>(HeapZap::ZAP_WORD),
                 isZap ? "是毒值_乙" : (allZero ? "非毒值_全零" : "非毒值_有内容"));
            VLOG(REPORT, "[GCV2][ROOT_ORIGIN] origin=%s obj=%p", src, object);
            // gcfwdfix: was this address inside a recent CompactRegion/ClearUnits zero range?
            char clearDetail[256];
            bool wasCleared = TraceClear::Lookup(reinterpret_cast<MAddress>(object), clearDetail, sizeof(clearDetail));
            VLOG(REPORT, "[GCV2][WAS_LIVE_BEFORE_CLEAR] hit=%u detail=%s obj=%p",
                 static_cast<unsigned>(wasCleared), clearDetail, object);
        }
        CHECK_DETAIL(false, "minor root/reference %p is not a valid object origin=%s", object, src);
    }
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
    if (region->IsYoungRegion() &&
        !region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object)) {


        workStack.push_back(object);
    }
}

// R3 markpar: STW-parallel young.mark_closure — sibling of ConcurrentMarkingWork
// (TracingCollector.cpp ConcurrentMarkingWork). Claim = MarkObject atomic bit;
// per-worker reachableVec/slots/weaks merged after pool barrier.
//
// Two young-parallel paths:
//   TraceYoungClosureParallel — old stack-split steal (markpar). Still behind the
//     pinned-off WORKERS getenv; product never takes it (6× slower under FYS1).
//   TraceYoungClosureStriped  — address-striped (zstripe 1ced7b2f, ZGC zMark.cpp:94-120).
//     Independent constexpr kMarkStriped, NOT nested under WORKERS.
//
// History of the nesting this commit undoes: 58ee51e7 (WORKERS gate) and 1ced7b2f
// (striped path) both touched this decision on branches that did not contain each
// other. The merge was textually clean; the nesting was leftover, not design.
// Both env gates were later pinned-off, which made the nested striped test dead.
// zstripe's 1.143× was measured on 1ced7b2f, where STRIPED=1 alone reached the path.
//
// kMarkStriped=false keeps current product behaviour (serial). Flip after the two
// verifiers are green. armed = decision taken; turned = workers>1 striped ran.
// Port of fix/markpar@3f869baa onto setbitmap ledger (reachableVec + useBitmapLedger).
namespace {
// MarkStack::size() counts buffers (64 objs each), not objects. Major uses 16/8 for
// deep concurrent stacks; young LIFO DFS stays shallow ⇒ those thresholds never fire.
// Buffer-level 2/1 so steal engages on ~64–128 greys (markpar 3f869baa).
constexpr size_t kMarkparMaxWorkSize = 2;
constexpr size_t kMarkparMinWorkSize = 1;
constexpr size_t kMarkStripeShift = 20; // 1 MiB address chunks, from zstripe phase-A locality data.
constexpr size_t kMarkStripeMultiplier = 4;
constexpr size_t kMarkStripeLocalCapacity = 64;
constexpr size_t kMarkStripeMax = 64;
// Independent of the WORKERS getenv. Product default: address-striped young mark
// (zstripe 1ced7b2f / ZGC zMark.cpp:94-120). Flip false to restore serial.
constexpr bool kMarkStriped = true;

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

    if (!Collector::PlausibleManagedObjectGate("AdmitYoungObject", object)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(object);
        if (host == nullptr || host == object) {
            return nullptr;
        }
        object = host;
        if (!Collector::PlausibleManagedObjectGate("AdmitYoungObject.host", object)) {
            return nullptr;
        }
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
bool ScrubMinorFreeTarget(RefField<>& field, BaseObject* target, bool /*fromFix*/,
                          bool holderIsCurrentMinorRoot)
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
    if (KeepRememberedHolder(SlotHeldByLiveObject(&field), holderIsCurrentMinorRoot)) {
        return false;
    }
    RefField<> oldField(field);
    const MAddress oldVal = raw(oldField.GetFieldValue());
    // zBarrier.inline.hpp:294-343 has no unresolved-to-null installation arm.
    // A free/garbage target means forwarding authority was retired before
    // coverage completed; fail closed instead of manufacturing a null heal.
    (void)HealSlot(field, oldField.GetFieldValue(), zpointer::null,
                   HealSite::WCollectorMinorFixForwardNull, HealNull::Disallow);
    Collector::FailClosedLoad("WCollector::ScrubMinorFreeTarget.unresolved", target, oldVal);
}

} // namespace WCollectorInternal

struct YoungMarkingShared {
    WCollector* collector = nullptr;
    GCThreadPool* pool = nullptr;
    // Owned by DoYoungGarbageCollection and immutable for this STW closure;
    // all workers may therefore query it while filling only their local slot vector.
    const std::unordered_set<MAddress>* reachableSlotDomain = nullptr;
    bool fullYoungScan = false;
    bool useBitmapLedger = true;
    bool recordSlots = false;
    std::vector<std::vector<BaseObject*>> objects;
    std::vector<std::vector<MAddress>> slots;
    std::vector<std::vector<MAddress>> weaks;
    std::vector<size_t> objectsMarked;
    std::atomic<size_t> nextWorkerId{ 0 };
};

class YoungMarkingWork : public HeapWork {
public:
    YoungMarkingWork(YoungMarkingShared& shared, TracingCollector::WorkStack&& stack, size_t workerSlot)
        : shared(shared), workStack(std::move(stack)), workerSlot(workerSlot)
    {}

    void TryForkTask()
    {
        if (shared.pool == nullptr) {
            return;
        }
        size_t childSlot = shared.nextWorkerId.load(std::memory_order_relaxed);
        if (childSlot >= shared.objects.size()) {
            return;
        }
        size_t size = workStack.size();
        if (size <= kMarkparMinWorkSize) {
            return;
        }
        bool doFork = false;
        size_t newSize = 0;
        if (size > kMarkparMaxWorkSize) {
            newSize = size >> 1;
            doFork = true;
        } else if (shared.pool->GetWaitingThreadNumber() > 0) {
            constexpr uint8_t shiftForEight = 3;
            newSize = size >> shiftForEight;
            doFork = true;
        }
        if (!doFork || newSize == 0) {
            return;
        }
        while (childSlot < shared.objects.size() &&
               !shared.nextWorkerId.compare_exchange_weak(childSlot, childSlot + 1, std::memory_order_relaxed)) {
        }
        if (childSlot >= shared.objects.size()) {
            return;
        }
        TracingCollector::WorkStackBuf* hSplit = workStack.split(newSize);
        shared.pool->AddWork(new YoungMarkingWork(shared, TracingCollector::WorkStack(hSplit), childSlot));
    }

    void Execute(size_t) override
    {
        auto& localObjects = shared.objects[workerSlot];
        auto& localSlots = shared.slots[workerSlot];
        auto& localWeaks = shared.weaks[workerSlot];
        size_t nMarked = 0;
        // FYS non-young / legacy set: per-worker dedup (no shared set write under race).
        std::unordered_set<BaseObject*> localNonYoungSeen;
        WCollector* collector = shared.collector;
        const bool useBitmapLedger = shared.useBitmapLedger;
        const bool recordSlots = shared.recordSlots;

        auto pushTarget = [collector, this](RefField<>& field) {
            BaseObject* target = collector->ResolveMinorReference(field);
            // h3seed3: same free|garbage scrub as TraceYoungClosureSerial.
            if (ScrubMinorFreeTarget(field, target, false)) {
                return;
            }
            collector->PushYoungObject(target, workStack, "closure_edge");
        };

        for (;;) {
            if (workStack.empty()) {
                break;
            }
            const MarkStackEntry entry = workStack.back();
            workStack.pop_back();
            BaseObject* object = entry.object();
            if (!Heap::IsHeapAddress(object)) {
                continue;
            }
            if (!Collector::PlausibleManagedObjectGate("TraceYoungClosure", object)) {
                BaseObject* host = Collector::TryRecoverInteriorBase(object);
                if (host != nullptr && host != object) {
                    PushAdmittedYoung(host, workStack, "TraceYoungClosure.recover");
                }
                continue;
            }
            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
            const bool isYoung = region->IsYoungRegion();

            if (useBitmapLedger) {
                if (isYoung) {
                    bool wasMarked = entry.mark() && collector->MarkYoungObject(object);
                    if (wasMarked) {
                        if (!entry.follow()) {
                            continue;
                        }
                        // ghostroute: residual unmarked young only (no FYS re-push of marked).

                        if (object->HasRefField() && !object->IsWeakRef()) {
                            object->ForEachRefField([collector, this, object](RefField<>& field) {
                                BaseObject* target = collector->ResolveMinorReference(field);
                                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                                    return;
                                }
                                if (!Collector::PlausibleManagedObjectGate("ghostroute.wasMarked.child",
                                                                          target)) {
                                    BaseObject* host = Collector::TryRecoverInteriorBase(target);
                                    if (host == nullptr || host == target) {
                                        return;
                                    }
                                    target = host;
                                }
                                RegionInfo* tr =
                                    RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                                if (tr == nullptr || !tr->IsYoungRegion() ||
                                    tr->IsMarkedObject(tr->GetMarkView<Generation::Young>(), target)) {
                                    return;
                                }

                                PushAdmittedYoung(target, workStack, "ghostroute.parallel.bitmap", &field, object);
                            });
                        }
                        if (shared.pool != nullptr) {
                            TryForkTask();
                        }
                        continue;
                    }
                    if (entry.mark()) {
                        ++nMarked;
                    }
                    CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
                    localObjects.push_back(object);
                } else {
                    continue;
                }
            } else {
                if (isYoung) {
                    bool wasMarked = entry.mark() && collector->MarkYoungObject(object);
                    if (wasMarked) {
                        if (!entry.follow()) {
                            continue;
                        }

                        if (object->HasRefField() && !object->IsWeakRef()) {
                            object->ForEachRefField([collector, this, object](RefField<>& field) {
                                BaseObject* target = collector->ResolveMinorReference(field);
                                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                                    return;
                                }
                                if (!Collector::PlausibleManagedObjectGate("ghostroute.wasMarked.child",
                                                                          target)) {
                                    BaseObject* host = Collector::TryRecoverInteriorBase(target);
                                    if (host == nullptr || host == target) {
                                        return;
                                    }
                                    target = host;
                                }
                                RegionInfo* tr =
                                    RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                                if (tr == nullptr || !tr->IsYoungRegion() ||
                                    tr->IsMarkedObject(tr->GetMarkView<Generation::Young>(), target)) {
                                    return;
                                }

                                PushAdmittedYoung(target, workStack, "ghostroute.parallel.legacy", &field, object);
                            });
                        }
                        if (shared.pool != nullptr) {
                            TryForkTask();
                        }
                        continue;
                    }
                    if (entry.mark()) {
                        ++nMarked;
                    }
                    if (!localNonYoungSeen.insert(object).second) {
                        continue;
                    }
                } else {
                    continue;
                }
                CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
                localObjects.push_back(object);
            }

            if (!object->HasRefField() || !entry.follow()) {
                if (shared.pool != nullptr) {
                    TryForkTask();
                }
                continue;
            }
            if (UNLIKELY(object->IsWeakRef())) {
                HeapSlot<>& referentField = HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
                localWeaks.push_back(reinterpret_cast<MAddress>(&referentField));
#if defined(MRT_TESTABLE_INTERNALS)
                NoteYoungWeakClosureDiscovery(YoungWeakClosureVariant::LEGACY_PARALLEL);
#endif
                if (shared.pool != nullptr) {
                    TryForkTask();
                }
                continue;
            }
            object->ForEachRefField([this, &localSlots, &pushTarget, recordSlots](RefField<>& field) {
                MAddress slot = reinterpret_cast<MAddress>(&field);
                if (recordSlots &&
                    (shared.reachableSlotDomain == nullptr || shared.reachableSlotDomain->count(slot) != 0)) {
                    localSlots.push_back(slot);
                }
                pushTarget(field);
            });
            if (shared.pool != nullptr) {
                TryForkTask();
            }
        }
        shared.objectsMarked[workerSlot] += nMarked;
    }

private:
    YoungMarkingShared& shared;
    TracingCollector::WorkStack workStack;
    size_t workerSlot;
};

struct alignas(64) YoungMarkStripeSeen {
    std::mutex lock;
    std::unordered_set<BaseObject*> seen;
};

struct alignas(64) YoungStripedWorkerOutput {
    std::vector<BaseObject*> objects;
    std::vector<MAddress> slots;
    std::vector<MAddress> weaks;
    size_t objectsMarked = 0;
    bool touched = false;
};

// ZMarkTerminate.inline.hpp:66-123 analogue. A worker becomes non-working
// while it waits; the last non-working worker may terminate only after the
// published stripe set is empty. Publishing work wakes one waiter instead of
// relying on yield polling.
class YoungMarkTerminate {
public:
    void Reset(size_t workers)
    {
        CHECK_DETAIL(workers != 0, "young mark termination needs a worker");
        std::lock_guard<std::mutex> lock(mutex);
        workerCount = workers;
        working = workers;
        awakening = 0;
        terminated = false;
    }

    bool TryTerminate(const MarkStripeSet& stripes)
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK_DETAIL(working != 0, "young mark worker left termination twice");
        --working;
        if (working == 0 && stripes.IsEmpty()) {
            terminated = true;
            condition.notify_all();
            return true;
        }
        if (!stripes.IsEmpty()) {
            ++working;
            return false;
        }
        condition.wait(lock, [this]() { return terminated || awakening != 0; });
        if (terminated) {
            return true;
        }
        --awakening;
        ++working;
        return false;
    }

    void WakeUp()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (terminated || working == 0 || working + awakening == workerCount) {
            return;
        }
        ++awakening;
        condition.notify_one();
    }

    bool Saturated() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return terminated && working == 0;
    }

private:
    size_t workerCount = 0;
    size_t working = 0;
    size_t awakening = 0;
    bool terminated = false;
    mutable std::mutex mutex;
    std::condition_variable condition;
};

struct YoungStripedShared {
    WCollector* collector = nullptr;
    const std::unordered_set<MAddress>* reachableSlotDomain = nullptr;
    bool fullYoungScan = false;
    bool useBitmapLedger = true;
    bool recordSlots = false;
    size_t workerCount = 0;
    std::unique_ptr<MarkStripeSet> stripes;
    std::unique_ptr<MarkingSMR> smr;
    std::vector<std::unique_ptr<YoungMarkStripeSeen>> seen;
    std::vector<std::unique_ptr<YoungStripedWorkerOutput>> outputs;
    YoungMarkTerminate terminate;
    std::atomic<size_t> stealSuccess{ 0 };
    std::atomic<size_t> stealFailure{ 0 };

    size_t StripeFor(BaseObject* object) const
    {
        return stripes->StripeForAddress(reinterpret_cast<uintptr_t>(object));
    }
};

class YoungStripedMarkingWork : public HeapWork {
public:
    YoungStripedMarkingWork(YoungStripedShared& shared, size_t workerSlot)
        : shared(shared), workerSlot(workerSlot),
          context(shared.workerCount, workerSlot, *shared.stripes)
    {}

    void Execute(size_t) override
    {
        size_t nMarked = 0;
        MarkingSMR& smr = *shared.smr;
        MarkStripeSet& stripes = *shared.stripes;
        for (;;) {
            MarkStackEntry entry;
            if (context.Stacks().Pop(smr, workerSlot, stripes, context.StripeId(), entry)) {
                shared.outputs[workerSlot]->touched = true;
                ProcessObject(entry, nMarked);
                continue;
            }
            if (TrySteal()) {
                continue;
            }
            if (context.Stacks().Flush(stripes, false)) {
                shared.terminate.WakeUp();
            }
            if (WaitForWorkOrDone()) {
                break;
            }
        }
        context.Cache().Flush();
        smr.Reclaim(workerSlot);
        shared.outputs[workerSlot]->objectsMarked += nMarked;
    }

private:
    bool TrySteal()
    {
        MarkingSMR& smr = *shared.smr;
        MarkStripeSet& stripes = *shared.stripes;
        const size_t home = context.StripeId();
        for (size_t victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
            MarkStripeStack* stack = context.Stacks().StealLocal(victim);
            if (stack == nullptr) {
                stack = stripes.At(victim).StealStack(smr, workerSlot);
                if (stack != nullptr) {
                    shared.stealSuccess.fetch_add(1, std::memory_order_relaxed);
                } else {
                    shared.stealFailure.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (stack != nullptr) {
                context.Stacks().Install(home, stack);
                return true;
            }
        }
        return false;
    }

    bool WaitForWorkOrDone()
    {
        return shared.terminate.TryTerminate(*shared.stripes);
    }

    void PushObject(BaseObject* object)
    {
        const size_t stripeIndex = shared.StripeFor(object);
        const bool publish = stripeIndex != context.StripeId();
        context.Stacks().Push(*shared.stripes, stripeIndex, MarkStackEntry::MarkAndFollow(object), publish);
        if (publish) {
            shared.terminate.WakeUp();
        }
    }

    bool ClaimSeen(BaseObject* object)
    {
        YoungMarkStripeSeen& stripeSeen = *shared.seen[context.StripeId()];
        std::lock_guard<std::mutex> guard(stripeSeen.lock);
        return stripeSeen.seen.insert(object).second;
    }

    void PushResidualYoungChild(RefField<>& field, BaseObject* holder, const char* origin)
    {
        WCollector* collector = shared.collector;
        BaseObject* target = collector->ResolveMinorReference(field);
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            return;
        }
        if (!Collector::PlausibleManagedObjectGate("ghostroute.wasMarked.child", target)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(target);
            if (host == nullptr || host == target) {
                return;
            }
            target = host;
        }
        RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (targetRegion == nullptr || !targetRegion->IsYoungRegion() ||
            targetRegion->IsMarkedObject(targetRegion->GetMarkView<Generation::Young>(), target)) {
            return;
        }

        PushObject(target);
    }

    void PushFilteredYoung(BaseObject* object, const char* origin)
    {
        if (!Heap::IsHeapAddress(object)) {
            return;
        }
        if (!Collector::PlausibleManagedObjectGate("PushYoungObject", object)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(object);
            if (host != nullptr && host != object) {
                PushFilteredYoung(host, origin);
            }
            return;
        }
        if (!object->IsValidObject()) {
            TracingCollector::WorkStack failClosed;
            shared.collector->PushYoungObject(object, failClosed, origin);
            return;
        }
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (!region->IsYoungRegion() ||
            region->IsMarkedObject(region->GetMarkView<Generation::Young>(), object)) {
            return;
        }


        PushObject(object);
    }

    void ProcessObject(const MarkStackEntry& entry, size_t& nMarked)
    {
        BaseObject* object = entry.object();
        if (!Heap::IsHeapAddress(object)) {
            return;
        }
        if (!Collector::PlausibleManagedObjectGate("TraceYoungClosure", object)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(object);
            if (host != nullptr && host != object) {
                BaseObject* admitted = AdmitYoungObject(host, "TraceYoungClosure.recover.striped");
                if (admitted != nullptr) {
                    PushObject(admitted);
                }
            }
            return;
        }

        YoungStripedWorkerOutput& output = *shared.outputs[workerSlot];
        auto& localObjects = output.objects;
        auto& localSlots = output.slots;
        auto& localWeaks = output.weaks;
        WCollector* collector = shared.collector;
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        const bool isYoung = region->IsYoungRegion();

        if (shared.useBitmapLedger) {
            if (isYoung) {
                bool wasMarked = entry.mark() && collector->MarkYoungObject(object, &context.Cache());
                if (wasMarked) {
                    if (!entry.follow()) {
                        return;
                    }

                    if (object->HasRefField() && !object->IsWeakRef()) {
                        object->ForEachRefField([this, object](RefField<>& field) {
                            PushResidualYoungChild(field, object, "ghostroute.striped.bitmap");
                        });
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
        } else {
            if (isYoung) {
                bool wasMarked = entry.mark() && collector->MarkYoungObject(object, &context.Cache());
                if (wasMarked) {
                    if (!entry.follow()) {
                        return;
                    }

                    if (object->HasRefField() && !object->IsWeakRef()) {
                        object->ForEachRefField([this, object](RefField<>& field) {
                            PushResidualYoungChild(field, object, "ghostroute.striped.legacy");
                        });
                    }
                    return;
                }
                if (entry.mark()) {
                    ++nMarked;
                }
                if (!ClaimSeen(object)) {
                    return;
                }
            } else {
                return;
            }
            CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
            localObjects.push_back(object);
        }

        if (!object->HasRefField()) {
            return;
        }
        if (!entry.follow()) {
            return;
        }
        auto pushTarget = [this, collector](RefField<>& field) {
            BaseObject* target = collector->ResolveMinorReference(field);
            // h3seed3: same free|garbage scrub as TraceYoungClosureSerial.
            if (ScrubMinorFreeTarget(field, target, false)) {
                return;
            }
            PushFilteredYoung(target, "closure_edge");
        };
        if (UNLIKELY(object->IsWeakRef())) {
            HeapSlot<>& referentField = HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            localWeaks.push_back(reinterpret_cast<MAddress>(&referentField));
#if defined(MRT_TESTABLE_INTERNALS)
            NoteYoungWeakClosureDiscovery(YoungWeakClosureVariant::STRIPED);
#endif
            return;
        }
        object->ForEachRefField([this, &localSlots, &pushTarget](RefField<>& field) {
            MAddress slot = reinterpret_cast<MAddress>(&field);
            if (shared.recordSlots &&
                (shared.reachableSlotDomain == nullptr || shared.reachableSlotDomain->count(slot) != 0)) {
                localSlots.push_back(slot);
            }
            pushTarget(field);
        });
    }

    YoungStripedShared& shared;
    size_t workerSlot;
    MarkContext context;
};

void WCollector::TraceYoungClosureSerial(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                         std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                         MinorSlotSet& weakSlots, bool useBitmapLedger,
                                         const MinorSlotSet* reachableSlotDomain)
{
    const bool recordSlots = fullYoungScan;
    auto recordReachableSlot = [&reachableSlots, reachableSlotDomain](RefField<>& field) {
        MAddress slot = reinterpret_cast<MAddress>(&field);
        if (reachableSlotDomain != nullptr && reachableSlotDomain->count(slot) == 0) {
            return;
        }
        (void)LedgerInsert(reachableSlots, slot);
    };
    auto pushTarget = [this, &workStack](RefField<>& field) {
        BaseObject* target = ResolveMinorReference(field);
        if (ScrubMinorFreeTarget(field, target, false)) {
            return;
        }

        PushYoungObject(target, workStack, "closure_edge");
    };
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();
        BaseObject* object = entry.object();
        if (!Heap::IsHeapAddress(object)) {
            continue;
        }
        if (!Collector::PlausibleManagedObjectGate("TraceYoungClosure", object)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(object);
            if (host != nullptr && host != object) {
                PushAdmittedYoung(host, workStack, "TraceYoungClosure.recover");
            }
            continue;
        }
        CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        const bool isYoung = region->IsYoungRegion();

        if (useBitmapLedger) {
            if (isYoung) {
                bool wasMarked = entry.mark() && MarkYoungObject(object);
                if (wasMarked) {
                    if (!entry.follow()) {
                        continue;
                    }

                    if (!object->HasRefField() || object->IsWeakRef()) {
                        continue;
                    }
                    object->ForEachRefField([this, &workStack, object](RefField<>& field) {
                        BaseObject* target = ResolveMinorReference(field);
                        if (target == nullptr || !Heap::IsHeapAddress(target)) {
                            return;
                        }
                        if (!Collector::PlausibleManagedObjectGate("ghostroute.wasMarked.child", target)) {
                            BaseObject* host = Collector::TryRecoverInteriorBase(target);
                            if (host == nullptr || host == target) {
                                return;
                            }
                            target = host;
                        }
                        RegionInfo* tr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                        if (tr == nullptr || !tr->IsYoungRegion() ||
                            tr->IsMarkedObject(tr->GetMarkView<Generation::Young>(), target)) {
                            return;
                        }

                        PushAdmittedYoung(target, workStack, "ghostroute.serial.bitmap", &field, object);
                    });
                    continue;
                }
                reachableVec.push_back(object);
            } else {
                continue;
            }
        } else {
            if (!LedgerInsert(reachableObjects, object)) {

                if (isYoung && object->HasRefField() && !object->IsWeakRef()) {
                    object->ForEachRefField([this, &workStack, object](RefField<>& field) {
                        BaseObject* target = ResolveMinorReference(field);
                        if (target == nullptr || !Heap::IsHeapAddress(target)) {
                            return;
                        }
                        if (!Collector::PlausibleManagedObjectGate("ghostroute.wasMarked.child", target)) {
                            BaseObject* host = Collector::TryRecoverInteriorBase(target);
                            if (host == nullptr || host == target) {
                                return;
                            }
                            target = host;
                        }
                        RegionInfo* tr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                        if (tr == nullptr || !tr->IsYoungRegion() ||
                            tr->IsMarkedObject(tr->GetMarkView<Generation::Young>(), target)) {
                            return;
                        }

                        PushAdmittedYoung(target, workStack, "ghostroute.serial.legacy", &field, object);
                    });
                }
                continue;
            }
            if (!isYoung) {
                continue;
            }
            if (entry.mark()) {
                (void)MarkYoungObject(object);
            }
            reachableVec.push_back(object);
        }

        if (!object->HasRefField() || !entry.follow()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            HeapSlot<>& referentField = HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            (void)LedgerInsert(weakSlots, reinterpret_cast<MAddress>(&referentField));
#if defined(MRT_TESTABLE_INTERNALS)
            NoteYoungWeakClosureDiscovery(YoungWeakClosureVariant::SERIAL);
#endif
            continue;
        }
        object->ForEachRefField([&recordReachableSlot, &pushTarget, recordSlots](RefField<>& field) {
            if (recordSlots) {
                recordReachableSlot(field);
            }
            pushTarget(field);
        });
    }
}

void WCollector::TraceYoungClosureParallel(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                           std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                           MinorSlotSet& weakSlots, bool useBitmapLedger, GCThreadPool* threadPool,
                                           const MinorSlotSet* reachableSlotDomain)
{
    // T-D ③: dispel frozen across parallel mark window (same as R2 reffix).
    const size_t dispelAtEntry = RegionInfo::GetDispelGhostCount();

    const int32_t helperNum = threadPool->GetMaxThreadNum();
    int32_t poolCap = helperNum + 1;
    int32_t workers = poolCap;
    {
        const char* value = std::getenv("MRT_GCV2_MARKPAR_WORKERS");
        if (value != nullptr && value[0] != '\0') {
            int32_t requested = static_cast<int32_t>(std::strtol(value, nullptr, 10));
            if (requested >= 1 && requested < workers) {
                workers = requested;
            }
        }
    }
    if (workers < 1) {
        workers = 1;
    }
    // workers=1 apparatus: main only, no pool Start (markpar 0cd9df7c).
    if (workers == 1) {
        TraceYoungClosureSerial(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                                useBitmapLedger, reachableSlotDomain);
        VLOG(REPORT,
             "[GCV2][markpar][parallel] workers_active=1 workers_scheduled=1 objects_marked=[%zu] "
             "reachable_n=%zu parallel=0",
             reachableVec.size(), reachableVec.size());
        return;
    }

    const size_t slotBudget = static_cast<size_t>(workers) * 8 + 16;
    YoungMarkingShared shared;
    shared.collector = this;
    shared.pool = threadPool;
    shared.reachableSlotDomain = reachableSlotDomain;
    shared.fullYoungScan = fullYoungScan;
    shared.useBitmapLedger = useBitmapLedger;
    shared.recordSlots = fullYoungScan;
    shared.objects.resize(slotBudget);
    shared.slots.resize(slotBudget);
    shared.weaks.resize(slotBudget);
    shared.objectsMarked.assign(slotBudget, 0);
    shared.nextWorkerId.store(1, std::memory_order_relaxed);

    const int32_t prevActive = threadPool->GetMaxActiveThreadNum();
    const int32_t wantActive = workers - 1;
    if (wantActive != prevActive) {
        threadPool->SetMaxActiveThreadNum(wantActive);
    }

    // Seed: peel root buffers to helpers first, then Start + main + WaitFinish.
    size_t slot = 1;
    while (workStack.size() > 1 && slot < static_cast<size_t>(workers)) {
        TracingCollector::WorkStackBuf* hSplit = workStack.split(1);
        if (hSplit == nullptr) {
            break;
        }
        threadPool->AddWork(new YoungMarkingWork(shared, TracingCollector::WorkStack(hSplit), slot));
        shared.nextWorkerId.store(slot + 1, std::memory_order_relaxed);
        ++slot;
    }
    threadPool->Start();
    YoungMarkingWork mainTask(shared, std::move(workStack), 0);
    mainTask.Execute(0);
    threadPool->WaitFinish();

    if (wantActive != prevActive) {
        threadPool->SetMaxActiveThreadNum(prevActive);
    }

    const size_t dispelAtExit = RegionInfo::GetDispelGhostCount();
    CHECK_DETAIL(dispelAtExit == dispelAtEntry,
                 "T-D ghost dispel during parallel mark_closure window entry=%zu exit=%zu", dispelAtEntry,
                 dispelAtExit);

    // Merge per-worker ledgers → global reachableVec / sets (downstream ⑦ consumes them).
    size_t active = 0;
    std::string markedStr;
    for (size_t i = 0; i < shared.objects.size(); ++i) {
        if (shared.objects[i].empty() && shared.slots[i].empty() && shared.weaks[i].empty() &&
            shared.objectsMarked[i] == 0) {
            continue;
        }
        if (shared.objectsMarked[i] != 0) {
            ++active;
        }
        if (!markedStr.empty()) {
            markedStr += ',';
        }
        markedStr += std::to_string(shared.objectsMarked[i]);
        for (BaseObject* obj : shared.objects[i]) {
            if (!useBitmapLedger) {
                reachableObjects.insert(obj);
            } else {
                RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
                if (region != nullptr && !region->IsYoungRegion() && fullYoungScan) {
                    reachableObjects.insert(obj);
                }
            }
            reachableVec.push_back(obj);
        }
        for (MAddress s : shared.slots[i]) {
            reachableSlots.insert(s);
        }
        for (MAddress s : shared.weaks[i]) {
            weakSlots.insert(s);
        }
    }
    if (markedStr.empty()) {
        markedStr = "0";
    }

    VLOG(REPORT,
         "[GCV2][markpar][parallel] workers_active=%zu workers_scheduled=%d objects_marked=[%s] "
         "reachable_n=%zu parallel=1",
         active, workers, markedStr.c_str(), reachableVec.size());
}

void WCollector::TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                          std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                          MinorSlotSet& weakSlots, bool useBitmapLedger, GCThreadPool* threadPool,
                                          const MinorSlotSet* reachableSlotDomain)
{
    g_markStripeArmed.fetch_add(1, std::memory_order_relaxed);
    const size_t dispelAtEntry = RegionInfo::GetDispelGhostCount();

    int32_t workers = threadPool->GetMaxThreadNum() + 1;
    {
        const char* value = std::getenv("MRT_GCV2_MARKPAR_WORKERS");
        if (value != nullptr && value[0] != '\0') {
            int32_t requested = static_cast<int32_t>(std::strtol(value, nullptr, 10));
            if (requested >= 1 && requested < workers) {
                workers = requested;
            }
        }
    }
    if constexpr (kGcTriggerDynamicWorkersEnabled) {
        // zDirector.cpp:783-793 — initial_workers selected each cycle.
        const uint32_t selected = g_gcTriggerYoungWorkers.load(std::memory_order_relaxed);
        if (selected >= 1 && selected < static_cast<uint32_t>(workers)) {
            workers = static_cast<int32_t>(selected);
        }
    }
    workers = std::max(workers, 1);
    if (workers == 1) {
        TraceYoungClosureSerial(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                                useBitmapLedger, reachableSlotDomain);
        VLOG(REPORT,
             "[GCV2][markpar][striped] workers_active=1 workers_scheduled=1 stripes=1 stripe_shift=%zu "
             "objects_marked=[%zu] reachable_n=%zu parallel=0 armed=%zu turned=%zu",
             kMarkStripeShift, reachableVec.size(), reachableVec.size(),
             g_markStripeArmed.load(std::memory_order_relaxed),
             g_markStripeTurned.load(std::memory_order_relaxed));
        return;
    }
    g_markStripeTurned.fetch_add(1, std::memory_order_relaxed);

    const size_t stripeCount = MarkStripeCount(static_cast<size_t>(workers));
    YoungStripedShared shared;
    shared.collector = this;
    shared.reachableSlotDomain = reachableSlotDomain;
    shared.fullYoungScan = fullYoungScan;
    shared.useBitmapLedger = useBitmapLedger;
    shared.recordSlots = fullYoungScan;
    shared.workerCount = static_cast<size_t>(workers);
    shared.terminate.Reset(shared.workerCount);
    shared.stripes = std::make_unique<MarkStripeSet>(stripeCount);
    shared.smr = std::make_unique<MarkingSMR>(shared.workerCount);
    shared.outputs.reserve(shared.workerCount);
    for (size_t i = 0; i < shared.workerCount; ++i) {
        shared.outputs.emplace_back(std::make_unique<YoungStripedWorkerOutput>());
    }
    shared.seen.reserve(stripeCount);
    for (size_t i = 0; i < stripeCount; ++i) {
        shared.seen.emplace_back(std::make_unique<YoungMarkStripeSeen>());
    }

    size_t rootCount = 0;
    MarkThreadLocalStacks seed(stripeCount);
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();
        seed.Push(*shared.stripes, shared.StripeFor(entry.object()), entry, true);
        ++rootCount;
    }
    CHECK_DETAIL(rootCount != 0, "striped mark requires a non-empty root stack");
    (void)seed.Flush(*shared.stripes, true);

    const int32_t prevActive = threadPool->GetMaxActiveThreadNum();
    const int32_t wantActive = workers - 1;
    if (wantActive != prevActive) {
        threadPool->SetMaxActiveThreadNum(wantActive);
    }
    for (int32_t worker = 1; worker < workers; ++worker) {
        threadPool->AddWork(new YoungStripedMarkingWork(shared, static_cast<size_t>(worker)));
    }
    threadPool->Start();
    YoungStripedMarkingWork mainTask(shared, 0);
    mainTask.Execute(0);
    threadPool->WaitFinish();
    CHECK_DETAIL(shared.terminate.Saturated(),
                 "young striped closure returned without coordinated worker termination");
    if (wantActive != prevActive) {
        threadPool->SetMaxActiveThreadNum(prevActive);
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
            if (!useBitmapLedger) {
                reachableObjects.insert(object);
            } else {
                RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
                if (region != nullptr && !region->IsYoungRegion() && fullYoungScan) {
                    reachableObjects.insert(object);
                }
            }
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
         "[GCV2][markpar][striped] workers_active=%zu workers_scheduled=%d stripes=%zu stripe_shift=%zu "
         "objects_marked=[%s] reachable_n=%zu parallel=1 armed=%zu turned=%zu steal_ok=%zu steal_fail=%zu",
         active, workers, stripeCount, kMarkStripeShift, markedStr.c_str(), reachableVec.size(),
         g_markStripeArmed.load(std::memory_order_relaxed),
         g_markStripeTurned.load(std::memory_order_relaxed),
         shared.stealSuccess.load(std::memory_order_relaxed),
         shared.stealFailure.load(std::memory_order_relaxed));
}

void WCollector::TraceYoungClosure(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                   std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                   MinorSlotSet& weakSlots, bool useBitmapLedger,
                                   const MinorSlotSet* reachableSlotDomain)
{
    if (workStack.empty()) {
        return;
    }
    GCThreadPool* threadPool = GetThreadPool();
#if defined(MRT_TESTABLE_INTERNALS)
    // The unit product SO reaches each shipping closure through the real minor
    // dispatcher.  Default builds have no selector and retain the production
    // striped decision below.
    if (const char* variant = std::getenv("MRT_GC_UNIT_YOUNG_WEAK_VARIANT")) {
        if (std::strcmp(variant, "serial") == 0) {
            TraceYoungClosureSerial(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                    weakSlots, useBitmapLedger, reachableSlotDomain);
            return;
        }
        CHECK_DETAIL(threadPool != nullptr, "young weak variant %s requires a GC thread pool", variant);
        if (std::strcmp(variant, "legacy-parallel") == 0) {
            TraceYoungClosureParallel(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                      weakSlots, useBitmapLedger, threadPool, reachableSlotDomain);
            return;
        }
        if (std::strcmp(variant, "striped") == 0) {
            TraceYoungClosureStriped(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                     weakSlots, useBitmapLedger, threadPool, reachableSlotDomain);
            return;
        }
        CHECK_DETAIL(false, "unknown MRT_GC_UNIT_YOUNG_WEAK_VARIANT=%s", variant);
    }
#endif
    static const bool forceSerial = []() {
        const char* value = std::getenv("MRT_GCV2_MARKPAR_FORCE_SERIAL");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    const bool workersSet = std::getenv("MRT_GCV2_MARKPAR_WORKERS") != nullptr;
    if (kMarkStriped && threadPool != nullptr && !forceSerial) {
        TraceYoungClosureStriped(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                                 useBitmapLedger, threadPool, reachableSlotDomain);
        return;
    }
    const bool useParallel = threadPool != nullptr && workersSet && !forceSerial;
    if (!useParallel) {
        const char* reason = "workers_unset";
        if (threadPool == nullptr) {
            reason = "pool_unavailable";
        } else if (forceSerial) {
            reason = "force_serial";
        } else if (!kMarkStriped) {
            reason = "striped_off";
        }
        VLOG(REPORT, "[GCV2][markpar][parallel] fallback=serial %s kMarkStriped=%d armed=%zu turned=%zu", reason,
             static_cast<int>(kMarkStriped), g_markStripeArmed.load(std::memory_order_relaxed),
             g_markStripeTurned.load(std::memory_order_relaxed));
        TraceYoungClosureSerial(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                                useBitmapLedger, reachableSlotDomain);
        VLOG(REPORT,
             "[GCV2][markpar][parallel] workers_active=1 workers_scheduled=1 objects_marked=[%zu] "
             "reachable_n=%zu parallel=0",
             reachableVec.size(), reachableVec.size());
        return;
    }
    TraceYoungClosureParallel(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                              useBitmapLedger, threadPool, reachableSlotDomain);
}

// youngconc: SATB termination for concurrent young mark — same loop shape as
// TracingCollector::MarkSatbBuffer, but feeds TraceYoungClosure (young claim + FYS).
// Mutators run under TraceBarrier (InstallBarrier TRACE).
// Termination: ZMark::end -> try_end (zMark.cpp:954-971) + ZMark::flush
// (zMark.cpp:587-605 / :998-1006). Young mark uses the same ZMark
// (zMark.cpp:757-780). A failed pause_mark_end resumes concurrent mark-follow
// before trying the pause again (zGeneration.cpp:549-555).
bool WCollector::MarkYoungSatbBuffer(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                     std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                     MinorSlotSet& weakSlots, bool useBitmapLedger,
                                     YoungConcWindowStats* windowStats)
{
    MRT_PHASE_TIMER("young.mark_satb");
    // portyoungconc: count what the window actually consumes. A retired SATB object that
    // Push* discards (non-heap / already marked / not young) is still SATB traffic, so it
    // is counted at the pop, not at the push -- the question this answers is "did the
    // window do GC work", not "how many greys survived the filter".
    size_t satbSeen = 0;
    auto visitSatbObj = [this, &workStack, windowStats, &satbSeen]() {
        WorkStack remarkStack;
        SatbBuffer::Instance().GetRetiredEntries([&](BaseObject* obj, bool follow) {
            ++satbSeen;
            if (windowStats != nullptr) {
                ++windowStats->satbObjects;
            }
            if (follow) {
                remarkStack.push_back(MarkStackEntry::FollowOnly(obj));
            } else {
                // Keep ordinary SATB semantics (including the young mark
                // claim/filter) unchanged; only allocate-black uses the
                // explicit Follow publication.
                if (Heap::IsHeapAddress(obj)) {
                    PushYoungObject(obj, workStack, "young_satb");
                }
            }
        });
        while (!remarkStack.empty()) {
            const MarkStackEntry entry = remarkStack.back();
            BaseObject* obj = entry.object();
            remarkStack.pop_back();
            if (!Heap::IsHeapAddress(obj)) {
                continue;
            }
            // Allocate-black has already claimed the mark bit; preserve
            // Follow so TraceYoungClosure still records the object and
            // traverses its children.
            workStack.push_back(entry);
        }
    };
    // ZMark::mark_follow() runs the coordinated worker termination once. Local
    // mutator buffers are deliberately not flushed here; pause-mark-end owns one
    // flush and returns failure to Generation when that flush discovers work.
    visitSatbObj();
    if (windowStats != nullptr) {
        ++windowStats->satbIters;
    }
    if (!workStack.empty()) {
        if (windowStats != nullptr) {
            ++windowStats->closureCalls;
        }
        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                          useBitmapLedger);
    }
    CHECK_DETAIL(workStack.empty(), "young concurrent follow returned with owner work");
    return true;
}

bool WCollector::TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats)
{
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "young mark-end flush requires stopped mutators");
    NoteMarkTerminatePause();
    size_t flushed = 0;
    MutatorManager::Instance().VisitAllMutators([](Mutator& mutator) { mutator.FlushSatbBuffer(); });
    SatbBuffer::Instance().GetRetiredEntries([this, &workStack, windowStats, &flushed](BaseObject* object,
                                                                                     bool follow) {
        ++flushed;
        if (windowStats != nullptr) {
            ++windowStats->satbObjects;
        }
        if (follow) {
            workStack.push_back(MarkStackEntry::FollowOnly(object));
        } else if (Heap::IsHeapAddress(object)) {
            PushYoungObject(object, workStack, "young_mark_end");
        }
    });
    NoteMarkTerminateFlushed(flushed);
    return workStack.empty();
}
void WCollector::MarkNewObject(BaseObject* obj)
{
    GCPhase mutatorPhase = Mutator::GetMutator()->GetMutatorPhase();
    if (UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_ENUM) || UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_TRACE) ||
        UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER)) {
        MarkObject(obj);
    }
}

void WCollector::ProcessFinalizers()
{
    FinalizerProcessor& fp = collectorResources.GetFinalizerProcessor();
    fp.ProcessReferences([this](BaseObject* obj) { return IsMarkedObject<Generation::Old>(obj); });
}
} // namespace MapleRuntime
