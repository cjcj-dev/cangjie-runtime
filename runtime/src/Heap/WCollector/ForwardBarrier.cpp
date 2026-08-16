// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "ForwardBarrier.h"

#include "Base/SysCall.h"
#include "Common/ScopedObjectLock.h"
#include "Heap/Verify/ToverFailDiag.h"
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
            }
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
