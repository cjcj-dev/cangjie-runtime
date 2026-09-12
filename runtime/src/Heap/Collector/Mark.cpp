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
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Collector/GcTriggerFlags.h"
#include "Heap/Collector/MarkEngine.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/Collector/MarkStripe.h"
#include "Heap/Collector/TenuringThreshold.h"
#include "Heap/GcThreadPool.h"
#include "Heap/HeapWork.h"
#include "Heap/Verify/VerifyHeap.h"
#include "Heap/Verify/MarkCompleteVerify.h"
#include "Heap/Verify/VerifyOption.h"
#include "Heap/Verify/VerifyRememberedSet.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/VerifyRoots.h"
#include "Heap/Verify/VerifyMarkingStacks.h"
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
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
// Preserve the two existing product template instantiations while internal
// consumers move to the richer first-live result. Their inline definitions
// and visibility are unchanged; this does not export a test-only API.
template bool RegionInfo::MarkObject<Generation::Young>(
    MarkView<Generation::Young>, const BaseObject*, size_t, bool);
template bool RegionInfo::MarkObject<Generation::Old>(
    MarkView<Generation::Old>, const BaseObject*, size_t, bool);

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
    bool firstLive = false;
    bool marked = region->MarkObjectByOwnerWithLiveClaim(obj, objectSize, liveCache == nullptr, firstLive);
    if (firstLive && liveCache != nullptr) {
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

bool WCollector::MarkEntryObject(BaseObject* obj, const MarkStackEntry& entry, MarkLiveCache* cache) const
{
    if (entry.mark() && !entry.finalizable()) {
        return MarkObjectImpl(obj, false, cache);
    }
    return TracingCollector::MarkEntryObject(obj, entry, cache);
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

    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &field };
    BaseObject* latest = make_load_good(oldField, provenance);

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
        const ForwardingProvenance provenance{ ForwardingHolderKind::StackSlot, this, &ref };
        BaseObject* to = FindToVersion(root).GetOrFailClosed(
            "WCollector::MarkStackRoots", provenance);
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
            workStack.push_back(MarkStackEntry::MarkAndFollow(targetObj, finalizable));
        } else {
            SurvNodeDiag::NoteTraceVisit(&field, targetObj, SurvNodeDiag::TRACE_SKIP_MARKED);
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
        MAddress stored = ForwardingTable::FindTo(fromAddr);
        if (stored == 0) {
            stored = ForwardingTable::FindRetiredTo(fromAddr);
        }
        if (stored != 0) {
            BaseObject* to = reinterpret_cast<BaseObject*>(stored);
            if (ToHeaderCovered(to)) {
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

        SurvNodeDiag::NoteTraceVisit(&field, latest, SurvNodeDiag::TRACE_PUSH);
        workStack.push_back(MarkStackEntry::MarkAndFollow(latest, finalizable));
    } else {
        SurvNodeDiag::NoteTraceVisit(&field, latest, SurvNodeDiag::TRACE_SKIP_MARKED);
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
        MarkPartialArray::NoteNotEncodable();
        FollowArrayElementsSmall(nullptr, addr, length, workStack, finalizable);
        return;
    }
    MarkPartialArray::NoteChunkPushed();
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

    MarkPartialArray::NoteArraySplit();

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
    MarkPartialArray::NoteChunkFollowed();
    FollowArrayElements(nullptr, &HeapSlotAt<>(chunkStart), length, workStack, entry.finalizable());
}

void WCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack, bool finalizable)
{
    if (UNLIKELY(MarkCompleteVerify::Enabled())) {
        MarkCompleteVerify::NoteHolderTrace(obj);
    }
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
            if (UNLIKELY(MarkPartialArray::Enabled())) {
                FollowArrayElements(obj, arrayContent, arrayLength, workStack, finalizable);
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
void WCollector::MarkOldFromYoung(BaseObject* object) const
{
    // ZBarrier::mark routes a young traversal's old target to the active old
    // mark domain (zBarrier.inline.hpp:742-750), never a later heap rescan.
    const GCCycleSnapshot old = GetCycleSnapshot(GCCycleGeneration::OLD);
    if (!old.active || (old.phase != GC_PHASE_ENUM && old.phase != GC_PHASE_TRACE &&
                        old.phase != GC_PHASE_CLEAR_SATB_BUFFER)) {
        return;
    }
    if (MarkObject(object)) {
        return;
    }
    SatbBuffer& satb = SatbBuffer::Instance(GCCycleGeneration::OLD);
    SatbBuffer::Node* node = nullptr;
    satb.EnsureGoodNode(node);
    CHECK_DETAIL(node != nullptr, "old mark publication requires a SATB node");
    (void)node->Push(object, nullptr, true);
    satb.FlushQueue(node);
}

void WCollector::TraceHeap()
{
    WorkStack workStack = NewWorkStack();
    WorkStack foreignStack = NewWorkStack();
    VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                     VerifyMarkingStacks::MarkingBoundary::START,
                                     VerifyMarkingStacks::MarkingContainer::OWNER, workStack.size(), 0);
    VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                     VerifyMarkingStacks::MarkingBoundary::START,
                                     VerifyMarkingStacks::MarkingContainer::FOREIGN, foreignStack.size(), 0);
    VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                     VerifyMarkingStacks::MarkingBoundary::START,
                                     VerifyMarkingStacks::MarkingContainer::POOL,
                                     GetWorkers().GetSnapshot().remainingWorkers, 0);
    const bool concurrentStackScan = MutatorManager::ConcurrentStackScanEnabled();
    uint64_t stackScanEpoch = 0;

    // Old mark-start belongs to the preceding young pause. The old body
    // begins with concurrent roots/follow (zGeneration.cpp:1015-1020).
    if (concurrentStackScan) {
        ScopedStopTheWorld stw("major stack scan prepare", false);
        Heap::GetHeap().InstallBarrier(GCPhase::GC_PHASE_ENUM);
        Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
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
        DoTracing(workStack, foreignStack);

        VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                         VerifyMarkingStacks::MarkingBoundary::END,
                                         VerifyMarkingStacks::MarkingContainer::OWNER, workStack.size(), 0);
        VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                         VerifyMarkingStacks::MarkingBoundary::END,
                                         VerifyMarkingStacks::MarkingContainer::FOREIGN, foreignStack.size(), 0);
        VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                         VerifyMarkingStacks::MarkingBoundary::END,
                                         VerifyMarkingStacks::MarkingContainer::POOL,
                                         GetWorkers().GetSnapshot().remainingWorkers, 0);

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
        CurrentizeValueRootSet(resurrectedExportObjectes);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase);
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
    CurrentizeValueRootMap(cycleRefWorkStack);
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
    PushYoungObject(object, workStack, origin, false);
}

void WCollector::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin,
                                  bool finalizable) const
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    // markfloor / introot: interiors (RawArray+8) pass IsValidObject (tip=length≠null).
    // Recover host object so the live array is marked; do not push the interior itself.
    if (!Collector::PlausibleManagedObjectGate("PushYoungObject", object)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(object);
        if (host != nullptr && host != object) {
            PushYoungObject(host, workStack, origin, finalizable);
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
    if (!region->IsYoungRegion()) {
        MarkOldFromYoung(object);
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
    MarkThreadLocalStacks& Stacks(size_t workerId) { return domain->Stacks(workerId); }

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
        MarkContext local(shared.workerCount, workerId, shared.Stripes(), shared.Stacks(workerId));
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
        VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::YOUNG,
                                         VerifyMarkingStacks::MarkingBoundary::WORKER_EXIT,
                                         VerifyMarkingStacks::MarkingContainer::LOCAL,
                                         local.Stacks().Population(), 0, workerId);
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
        if (!Collector::PlausibleManagedObjectGate("ghostroute.wasMarked.child", target)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(target);
            if (host == nullptr || host == target) {
                return;
            }
            target = host;
        }
        RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (targetRegion != nullptr && !targetRegion->IsYoungRegion()) {
            collector->MarkOldFromYoung(target);
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
        if (!Collector::PlausibleManagedObjectGate("PushYoungObject", object)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(object);
            if (host != nullptr && host != object) {
                PushFilteredYoung(ctx, host, origin, finalizable);
            }
            return;
        }
        if (!object->IsValidObject()) {
            TracingCollector::WorkStack failClosed;
            shared.collector->PushYoungObject(object, failClosed, origin);
            return;
        }
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (!region->IsYoungRegion()) {
            shared.collector->MarkOldFromYoung(object);
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
        if (!Collector::PlausibleManagedObjectGate("TraceYoungClosure", object)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(object);
            if (host != nullptr && host != object) {
                BaseObject* admitted = AdmitYoungObject(host, "TraceYoungClosure.recover.striped");
                if (admitted != nullptr) {
                    PushObject(ctx, admitted);
                }
            }
            return;
        }

        auto& localObjects = output.objects;
        auto& localWeaks = output.weaks;
        WCollector* collector = shared.collector;
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        const bool isYoung = region->IsYoungRegion();

        if (isYoung) {
            bool wasMarked = collector->MarkEntryObject(object, entry, &ctx.Cache());
            if (wasMarked) {
                if (!entry.follow()) {
                    return;
                }

                if (object->HasRefField() && !object->IsWeakRef()) {
                    object->ForEachRefField([this, &ctx, object](RefField<>& field) {
                        PushResidualYoungChild(ctx, field, object, "ghostroute.striped.bitmap");
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
        if (!object->HasRefField()) {
            return;
        }
        if (!entry.follow()) {
            return;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            HeapSlot<>& referentField = HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            localWeaks.push_back(reinterpret_cast<MAddress>(&referentField));
#if defined(MRT_TESTABLE_INTERNALS)
            NoteYoungWeakClosureDiscovery(YoungWeakClosureVariant::STRIPED);
#endif
            return;
        }
        MarkPartialArray::FollowObjectReferences(object, entry.finalizable(), visitSlot, publish);
    }

    YoungStripedShared& shared;
};

void WCollector::TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,
                                          std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                          MinorSlotSet& weakSlots,
                                          const MinorSlotSet* reachableSlotDomain)
{
    g_markStripeArmed.fetch_add(1, std::memory_order_relaxed);
    const size_t dispelAtEntry = RegionInfo::GetDispelGhostCount();

    GCWorkers& workersSet = GetWorkers();
    size_t workers = workersSet.ActiveWorkers();
    if (workers == 0) {
        workers = 1;
        workersSet.SetActiveWorkers(1);
    }
    g_markStripeTurned.fetch_add(1, std::memory_order_relaxed);

    if (youngMarkDomain == nullptr) {
        youngMarkDomain = std::make_unique<MarkDomain>(kMarkStripeMax, VerifyMarkingStacks::MarkingGeneration::YOUNG);
    }
    youngMarkDomain->BindWorkers(&workersSet);
    youngMarkDomain->BindAbort(&collectorResources.GetMinorDriverPort().Abort());
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

    size_t rootCount = 0;
    MarkThreadLocalStacks seed(youngMarkDomain->Stripes().Count());
    VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::YOUNG,
                                     VerifyMarkingStacks::MarkingBoundary::START,
                                     VerifyMarkingStacks::MarkingContainer::LOCAL, seed.Population(), 0);
    VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::YOUNG,
                                     VerifyMarkingStacks::MarkingBoundary::START,
                                     VerifyMarkingStacks::MarkingContainer::STRIPE,
                                      shared.Stripes().Population(), VerifyMarkingStacks::NO_MARKING_INDEX,
                                      VerifyMarkingStacks::NO_MARKING_INDEX,
                                      shared.Stripes().FirstNonEmptyStripe());
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
        ++rootCount;
    }
    CHECK_DETAIL(rootCount != 0, "striped mark requires a non-empty root stack");
    VerifyMarkingStacks::NoteProducer(VerifyMarkingStacks::MarkingGeneration::YOUNG,
                                      VerifyMarkingStacks::MarkingContainer::LOCAL, seed.Population());
    (void)seed.Flush(shared.Stripes(), true);
    VerifyMarkingStacks::NoteProducer(VerifyMarkingStacks::MarkingGeneration::YOUNG,
                                      VerifyMarkingStacks::MarkingContainer::STRIPE,
                                      shared.Stripes().Population());
    VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::YOUNG,
                                     VerifyMarkingStacks::MarkingBoundary::SEED_PUBLISH,
                                     VerifyMarkingStacks::MarkingContainer::LOCAL, seed.Population(), 0);

    YoungStripedMarkingWork task(shared);
    workersSet.Run(task);
    youngMarkDomain->FinishWork();
    VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::YOUNG,
                                     VerifyMarkingStacks::MarkingBoundary::JOIN,
                                     VerifyMarkingStacks::MarkingContainer::STRIPE,
                                     shared.Stripes().Population(), VerifyMarkingStacks::NO_MARKING_INDEX,
                                     VerifyMarkingStacks::NO_MARKING_INDEX,
                                     shared.Stripes().FirstNonEmptyStripe());
    if (!collectorResources.GetMinorDriverPort().Abort().Poll()) {
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
    if (workStack.empty()) {
        return;
    }
    VerifyMarkingStacks::NoteProducer(VerifyMarkingStacks::MarkingGeneration::YOUNG,
                                      VerifyMarkingStacks::MarkingContainer::OWNER, workStack.size());
    TraceYoungClosureStriped(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots,
                             reachableSlotDomain);
}

// youngconc: SATB termination for concurrent young mark — same loop shape as
// TracingCollector::MarkSatbBuffer, but feeds TraceYoungClosure (young claim + FYS).
// Mutators run under TraceBarrier (InstallBarrier TRACE).
// Termination: ZMark::end -> try_end (zMark.cpp:954-971) + ZMark::flush
// (zMark.cpp:587-605 / :998-1006). Young mark uses the same ZMark
// (zMark.cpp:757-780). A failed pause_mark_end resumes concurrent mark-follow
// before trying the pause again (zGeneration.cpp:549-555).
bool WCollector::MarkYoungSatbBuffer(WorkStack& workStack, bool fullYoungScan,
                                     std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                     MinorSlotSet& weakSlots,
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
        SatbBuffer::Instance(GCCycleGeneration::YOUNG).GetRetiredEntries([&](BaseObject* obj, bool follow) {
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
    // ZMark::mark_follow() consumes SATB plus every mutator-local producer that
    // can publish grey without filling a SATB node (alloc-buffer roots,
    // allocate-black Follow, y2y dirty). Pause-mark-end still owns the frozen
    // non-full node flush; it must not be the first consumer of these producers.
#if defined(MRT_TESTABLE_INTERNALS)
    PublishConcurrentYoungProducersTestReceipt();
#endif
    theAllocator.VisitAllocBuffers([this, &workStack](AllocBuffer& buffer) {
        buffer.MergeRoots(workStack);
        buffer.MergeYoungAllocBlackFollow(workStack);
        buffer.MergeY2yDirtyHolders(workStack);
        buffer.MergeY2yDirtySlots([this, &workStack](MAddress slot) {
            RefField<>& field = HeapSlotAt<>(slot);
            BaseObject* target = ResolveMinorReference(field);
            PushYoungObject(target, workStack, "y2y_slot");
        });
    });
    visitSatbObj();
    if (windowStats != nullptr) {
        ++windowStats->satbIters;
    }
    if (!workStack.empty()) {
        if (windowStats != nullptr) {
            ++windowStats->closureCalls;
        }
        TraceYoungClosure(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots);
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
    SatbBuffer::Instance(GCCycleGeneration::YOUNG).GetRetiredEntries([this, &workStack, windowStats, &flushed](BaseObject* object,
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
