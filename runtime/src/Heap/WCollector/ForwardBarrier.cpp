// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "ForwardBarrier.h"

#include "Base/SysCall.h"
#include "Common/ScopedObjectLock.h"
#include "Heap/Verify/ZgcInvariants.h"
#include "Heap/Verify/ToverFailDiag.h"
#include "Heap/Verify/RegionLifeDiag.h"
#include "Heap/Verify/TraceClear.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
namespace {
// toverfail: barrier-moment snapshot of ObjectState bits (not post-return recompute).
unsigned ToverFailStateCode(BaseObject* obj)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return 0xffu;
    }
    uint64_t hdr = __atomic_load_n(reinterpret_cast<const uint64_t*>(obj), __ATOMIC_RELAXED);
    return static_cast<unsigned>((hdr >> 48) & 0x3u);
}

// Names the *slot* a zero-header target came out of, which is the half our existing gate is
// missing: PlausibleManagedObjectGate(site, obj) takes the object and a site string but never
// the field it was read from, so a rejection cannot be traced back to the writer.
//
// OpenJDK does carry both (zVerify.cpp:114-124):
//     #define BAD_OOP_ARG(o, p) "Bad oop " PTR_FORMAT " found at " PTR_FORMAT, untype(o), p2i(p)
//     guarantee(oopDesc::is_oop(obj), BAD_OOP_ARG(o, p));
// -- always on (guarantee, not assert), and it names the value *and* where it was found.
//
// Why this exists: cjcj::cjc --package packages/basic/src on a coloured runtime dies with
//     SIGSEGV si_addr=0x8 si_code=1 pc=_CN10cjcj:utils2AsIG_HCNat3AnyE gc_phase=forward_phase
//     1a95ea1: mov (%rbx),%rdi        <- loads the TypeInfo word of the object in rbx
//     1a95ea4: cmpb $0x15,0x8(%rdi)   <- faults, rdi == 0
// rbx held a plausible heap address (0x7dae21c1d770) whose header word was zero, i.e. an object
// that was never filled in. The reference had already been resolved, so the question is which
// slot handed it over -- and that is precisely what no existing message records.
//
// Reports only -- no abort, no behaviour change -- and caps itself at 8 lines.
//
// ⛔ Compile-time gated OFF: the check costs one relaxed header load on ReadReference's fast
// path, which is the hottest path in the runtime.  Flip kZeroHdrProbe to true and rebuild when
// chasing a zeroed-target crash; the campaign convention is a constant, never a new env var.
constexpr bool kZeroHdrProbe = false;
std::atomic<size_t> g_zeroHdrReported{ 0 };
constexpr size_t kZeroHdrReportMax = 8;

void NoteZeroHeaderTarget(const char* site, const RefField<false>& field, BaseObject* holder, BaseObject* target,
                          BaseObject* preResolve = nullptr, unsigned steps = 0xffu)
{
    if (!kZeroHdrProbe) {
        return;
    }
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return;
    }
    if (__atomic_load_n(reinterpret_cast<const uint64_t*>(target), __ATOMIC_RELAXED) != 0) {
        return;
    }
    size_t seen = g_zeroHdrReported.fetch_add(1, std::memory_order_relaxed);
    if (seen >= kZeroHdrReportMax) {
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    RegionInfo* preRegion = (preResolve != nullptr && Heap::IsHeapAddress(preResolve))
        ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(preResolve)) : nullptr;
    // Splits the two ways a live field can name a zeroed region:
    //   holder live  -> the field was missed by fix-up (the holder should have been rescanned)
    //   holder dead  -> we are walking a dead holder, and the target is collateral
    // RegionManager.cpp:1525-1541 calls that edge "the reuse edge": a garbage region is taken,
    // ClearUnits zeroes its payload, and InitRegion immediately re-uses it.  DrainScope there
    // waits for retained *routing* readers; a stale field value retains nothing, so it is not
    // covered.  What ZGC relies on instead is the remap epoch -- a pointer from an older epoch
    // is load-bad, so the barrier must remap it before use, and the page's virtual range is not
    // re-used until that epoch retires (zForwarding.cpp:171-181 detach_page, plus the four
    // Remapped states).  Here the value was load-good, i.e. it carried the current colour, so
    // the barrier had no reason to look at it at all.
    RegionInfo* holderRegion = (holder != nullptr && Heap::IsHeapAddress(holder))
        ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder)) : nullptr;
    // TraceClear already records every clear/reuse with the region's liveByteCount at the time
    // (RegionManager.cpp:1517 "garbage_reuse"), and exposes Lookup by address -- so ask it which
    // reclaim decided this range rather than building a second ledger.
    char clearInfo[256];
    if (!TraceClear::Lookup(reinterpret_cast<MAddress>(target), clearInfo, sizeof(clearInfo))) {
        clearInfo[0] = '-';
        clearInfo[1] = '\0';
    }
    LOG(RTLOG_ERROR,
        "[GCV2][zerohdr] site=%s target=%p slot=%p holder=%p n=%zu region=%p regionType=%u "
        "routeState=%u ghostFrom=%u pre=%p preRegionType=%u preGhost=%u steps=%u "
        "holderRegionType=%u holderHdr=%#lx holderValid=%u clear={%s} "
        "(target header word is 0; regionType 0 == FREE_REGION; steps bit0=make_load_good "
        "bit1=ForwardObject)",
        site, target, static_cast<const void*>(&field), holder, seen, region,
        region == nullptr ? 0xffu : static_cast<unsigned>(region->GetRegionType()),
        region == nullptr ? 0xffu : static_cast<unsigned>(region->GetRouteState()),
        static_cast<unsigned>(RegionInfo::InGhostFromRegion(target)),
        preResolve,
        preRegion == nullptr ? 0xffu : static_cast<unsigned>(preRegion->GetRegionType()),
        preResolve == nullptr ? 0xffu : static_cast<unsigned>(RegionInfo::InGhostFromRegion(preResolve)),
        steps,
        holderRegion == nullptr ? 0xffu : static_cast<unsigned>(holderRegion->GetRegionType()),
        (holder != nullptr && Heap::IsHeapAddress(holder))
            ? static_cast<unsigned long>(__atomic_load_n(reinterpret_cast<const uint64_t*>(holder),
                                                         __ATOMIC_RELAXED))
            : 0ul,
        (holder != nullptr && Heap::IsHeapAddress(holder))
            ? static_cast<unsigned>(holder->IsValidObject()) : 0xffu,
        clearInfo);
    // RegionLifeDiag already records every free with its PATH_*, phase, gcCount, knownEmpty and
    // liveBytes, and DumpJoinForTarget exists for exactly this question: which path freed the
    // region this address is in.  Restored from e90e22a4^ for the diagnostic build.
    RegionLifeDiag::DumpJoinForTarget(reinterpret_cast<uintptr_t>(target), "zerohdr");
}
} // namespace

BaseObject* ForwardBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    // Soft-resolve every tagged outcome. A bare `do { ... } while (true)` with no
    // progress path livelocks the mutator (no safepoint) and GC then spins forever in
    // EnsurePhaseTransition(IDLE). Bound kSelfHealAttempts: colour writers can re-tag
    // the same slot (ATOMIC_READ_PROTOCOL Q2).
    for (;;) {
        RefField<> oldField(field);
        BaseObject* oldTarget = to_object(oldField.GetTargetObject());
        if (oldTarget == nullptr || LIKELY(theCollector.is_load_good(oldField))) {
            if (oldTarget != nullptr) {
                ToverFailDiag::NoteLoadGoodFast();
                NoteZeroHeaderTarget("ForwardRead.fast", field, obj, oldTarget);
                // fastfrom: the state census isolated exactly one tuple ZGC's design excludes --
                // slot colour == current good colour, target FORWARDED, target in a ghost-from
                // region -- 53 in 24M hand-outs.  A reading taken *after* the barrier returned put
                // the to-version in the slot and the from-copy in the caller's hands, but the slow
                // path below heals and returns the same object, so the only place the two can part
                // is here: this arm returns oldTarget without writing anything, and another thread
                // heals the slot afterwards.
                //
                // That leaves one question, and it has to be answered with the value this arm
                // actually accepted rather than with a later re-read: how is a from-copy's pointer
                // load-good at all?  In ZGC that cannot happen -- becoming load-good means passing
                // through a barrier, and a barrier relocates a relocation-set address on the spot
                // (zGeneration.inline.hpp:131-140).  The raw colour plus the flip sequence separate
                // the two remaining stories: painted this publication (so the paint site missed it)
                // or painted an earlier one whose colour has come back around.
                ZgcInvariants::NoteFastPathAccept(static_cast<uintptr_t>(raw(oldField.GetFieldValue())), oldTarget);
            }
            return oldTarget;
        }

        ToverFailDiag::NoteSlowEnter();
        BaseObject* loadGood = oldTarget;
        unsigned zhSteps = 0;
        if (!theCollector.IsUnmovableFromObject(oldTarget)) {
            ToverFailDiag::NoteResolveEnter();
            loadGood = theCollector.make_load_good(oldField);
            zhSteps |= 1u;
            if (theCollector.IsGhostFromObject(loadGood)) {
                zhSteps |= 2u;
                BaseObject* fwd = theCollector.ForwardObject(loadGood);
                // tipnull: ForwardObject may null on soft miss; never hand null to mutator
                // for a live non-null ref (self-heal would CAS null into the slot).
                if (fwd != nullptr) {
                    loadGood = fwd;
                }
            }
            ToverFailDiag::NoteResolveOutcome(oldTarget, loadGood,
                                              loadGood != oldTarget ? 1u : 0u);
        } else {
            // 丁: barrier-moment IsUnmovableFromObject short-circuit (fromver §6).
            unsigned st = ToverFailStateCode(oldTarget);
            ToverFailDiag::NoteUnmovableSkip(oldTarget, st,
                                             st == static_cast<unsigned>(ObjectState::FORWARDED) ? 1u : 0u);
        }
        // relroroot / rostatic: non-heap targets (static constants under GNU_RELRO) are never
        // evacuated. Colouring + CAS into those slots faults (si_code=2 ACCERR). Skip write-back.
        if (loadGood != nullptr && !Heap::IsHeapAddress(loadGood)) {
            return loadGood;
        }

        RefField<> goodField = theCollector.GetAndTryTagRefField(loadGood);
        // OpenJDK ZBarrier::self_heal (zBarrier.inline.hpp:72-107): retain the exact
        // observed value as the CAS expected value and retry after a concurrent update.
        ZgcSelfHealLoadGood(field, oldField.GetFieldValue(), goodField.GetFieldValue(),
                            HealSite::ForwardReadReference);
        NoteZeroHeaderTarget("ForwardRead.healed", field, obj, loadGood, oldTarget, zhSteps);
        return loadGood;
    }
}

BaseObject* ForwardBarrier::ReadStaticRef(ReadOnlyRootSlot& field) const { return Barrier::ReadStaticRef(field); }

BaseObject* ForwardBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    return ReadReference(obj, field);
}

void ForwardBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    CHECK(!Heap::IsHeapAddress(dst));
    if (obj != nullptr) {
        obj->ForEachRefInStruct(
            [this, obj](RefField<false>& field) {
                BaseObject* target = ReadReference(obj, field);
                (void)target;
            },
            src, src + size);
    }
    CHECK(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK);
    FixupNonHeapStructRefs(dst, obj, src, size);
}

void ForwardBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    CHECK(!Heap::IsHeapAddress(src));
    CHECK(!Heap::IsHeapAddress(dst));
    gctib.ForEachBitmapWord(src, [=](RefField<>& srcField) {
        BaseObject* target = ReadReference(nullptr, srcField);
        (void)target;
    });
    CHECK(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK);
    FixupNonHeapStaticStructRefs(dst, src, size, gctib);
}

BaseObject* ForwardBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    // Bound kSelfHealAttempts: colour writers can re-tag the same slot (ATOMIC_READ_PROTOCOL Q2).
    for (;;) {
        RefField<false> oldField(field.GetFieldValue(order));
        BaseObject* oldTarget = to_object(oldField.GetTargetObject());
        if (oldTarget == nullptr || LIKELY(theCollector.is_load_good(oldField))) {
            if (oldTarget != nullptr) {
                ToverFailDiag::NoteLoadGoodFast();
            }
            DLOG(FBARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(oldField.GetFieldValue()), oldTarget);
            return oldTarget;
        }

        ToverFailDiag::NoteSlowEnter();
        BaseObject* loadGood = oldTarget;
        if (!theCollector.IsUnmovableFromObject(oldTarget)) {
            ToverFailDiag::NoteResolveEnter();
            loadGood = theCollector.make_load_good(oldField);
            if (theCollector.IsGhostFromObject(loadGood)) {
                BaseObject* fwd = theCollector.ForwardObject(loadGood);
                // tipnull: ForwardObject may null on soft miss; never hand null to mutator
                // for a live non-null ref (self-heal would CAS null into the slot).
                if (fwd != nullptr) {
                    loadGood = fwd;
                }
            }
            ToverFailDiag::NoteResolveOutcome(oldTarget, loadGood,
                                              loadGood != oldTarget ? 1u : 0u);
        } else {
            unsigned st = ToverFailStateCode(oldTarget);
            ToverFailDiag::NoteUnmovableSkip(oldTarget, st,
                                             st == static_cast<unsigned>(ObjectState::FORWARDED) ? 1u : 0u);
        }
        // relroroot / rostatic: non-heap targets under GNU_RELRO — skip colour CAS write-back.
        if (loadGood != nullptr && !Heap::IsHeapAddress(loadGood)) {
            return loadGood;
        }

        RefField<> goodField = theCollector.GetAndTryTagRefField(loadGood);
        // Replaces the old "not old-tag" assertion with the colour-era self-heal invariant.
        DCHECK(theCollector.is_load_good(goodField));
        ZgcSelfHealLoadGood(field, oldField.GetFieldValue(), goodField.GetFieldValue(),
                            HealSite::ForwardAtomicReadReference);
        return loadGood;
    }
}

void ForwardBarrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                          MemoryOrder order) const
{
    RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
    field.StoreColoured(newField.GetFieldValue(), order);
    if (obj != nullptr) {
        DLOG(FBARRIER, "atomic write obj %p<%p>(%zu) ref@%p: %#zx", obj, obj->GetTypeInfo(), obj->GetSize(), &field,
             raw(newField.GetFieldValue()));
    } else {
        DLOG(FBARRIER, "atomic write static ref@%p: %#zx", &field, raw(newField.GetFieldValue()));
    }
}

BaseObject* ForwardBarrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                                MemoryOrder order) const
{
    RefField<> coloured = theCollector.GetAndTryTagRefField(newRef);
    MAddress oldValue = raw(field.Exchange(coloured.GetFieldValue(), order));
    RefField<> oldField(oldValue);
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    DLOG(BARRIER, "atomic swap obj %p<%p>(%zu) ref-field@%p: old %#zx(%p), new %#zx(%p)", obj, obj->GetTypeInfo(),
         obj->GetSize(), &field, oldValue, oldRef, raw(field.GetFieldValue()), newRef);
    return oldRef;
}

bool ForwardBarrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                             BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    MAddress oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
    RefField<false> oldField(oldFieldValue);
    BaseObject* oldVersion = ReadReference(nullptr, oldField);
    // Bound kCasAttempts: colour self-heal can keep raw expected bits moving (c3179214).
    for (int attempt = 0; attempt < kCasAttempts && oldVersion == oldRef; ++attempt) {
        RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
        if (HealSlot(field, to_zpointer(oldFieldValue), newField.GetFieldValue(),
                     HealSite::ForwardCompareAndSwapReference, HealNull::Allow, succOrder, failOrder)) {
            return true;
        }
        oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
        RefField<false> tmp(oldFieldValue);
        oldVersion = ReadReference(nullptr, tmp);
    }

    return false;
}

void ForwardBarrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                     MAddress srcField, MIndex srcSize) const
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (!(static_cast<MArray*>(dstObj)->GetComponentTypeInfo()->IsStructType())) {
        LOG(RTLOG_FATAL, "array %p type is not struct type", dstObj);
        return;
    }
#endif

    if (!dstObj->HasRefField()) {
        CHECK(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) == EOK);
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), dstSize);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
        return;
    }

    MArray* srcArray = static_cast<MArray*>(srcObj);
    RefFieldVisitor srcVisitor = [this, srcArray](RefField<false>& field) { (void)ReadReference(srcArray, field); };
    srcArray->ForEachRefFieldInRange(srcVisitor, srcField, srcField + srcSize);

    CHECK(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) == EOK);

    // R9 bulk：堆 dst 补色（与 Idle/base 同形）。
    if (dstObj != nullptr && Heap::IsHeapAddress(dstObj) && dstObj->HasRefField()) {
        RefFieldVisitor recolour = [this](RefField<false>& field) {
            RefField<> oldField(field);
            MAddress oldValue = raw(oldField.GetFieldValue());
            BaseObject* latest = ReadReference(nullptr, oldField);
            RefField<> newField = theCollector.GetAndTryTagRefField(latest);
            if (oldValue != raw(newField.GetFieldValue())) {
                HealSlot(field, to_zpointer(oldValue), newField.GetFieldValue(),
                         HealSite::ForwardCopyStructArrayRecolour);
            }
        };
        static_cast<MArray*>(dstObj)->ForEachRefFieldInRange(recolour, dstField, dstField + srcSize);
    }

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), dstSize);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
}
} // namespace MapleRuntime
